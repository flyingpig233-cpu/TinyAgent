"""End-to-end checks for the full-screen CLI.

The fake server exercises Responses and Chat Completions, streaming and
non-streaming, tools, cancellation and approval. The CLI runs on a real PTY so
ncurses sees a terminal; assertions use server requests, file state, exit codes
and restored termios flags rather than fragile ANSI text matching.
"""

import fcntl
import http.server
import json
import os
import pty
import select
import signal
import struct
import subprocess
import sys
import threading
import time
import termios

REASONING = {'type': 'reasoning', 'id': 'rs_1', 'summary': [],
             'encrypted_content': 'opaque'}


def message(text):
    return {'type': 'message', 'id': 'msg_1', 'role': 'assistant',
            'status': 'completed',
            'content': [{'type': 'output_text', 'text': text}]}


def function_call(call_id, name, arguments):
    return {'type': 'function_call', 'id': 'fc_' + call_id,
            'call_id': call_id, 'name': name, 'arguments': arguments}


def responses_events(text):
    return [
        {'type': 'response.output_text.delta', 'delta': text},
        {'type': 'response.completed',
         'response': {'status': 'completed',
                      'output': [REASONING, message(text)]}},
    ]


def responses_tool_events(calls, before=None):
    events = []
    output = [REASONING]
    if before is not None:
        events.append({'type': 'response.output_text.delta', 'delta': before})
        output.append(message(before))
    output.extend(calls)
    events.append({'type': 'response.completed',
                   'response': {'status': 'completed', 'output': output}})
    return events


def responses_json(text):
    return {'status': 'completed', 'output': [REASONING, message(text)]}


def responses_tool_json(calls):
    return {'status': 'completed', 'output': [REASONING] + calls}


def chat_events(text):
    return [
        {'choices': [{'delta': {'content': text}, 'finish_reason': None}]},
        {'choices': [{'delta': {}, 'finish_reason': 'stop'}]},
    ]


def chat_tool_events(call):
    return [
        {'choices': [{'delta': {'tool_calls': [
            {'index': 0, 'id': call['call_id'], 'type': 'function',
             'function': {'name': call['name'],
                          'arguments': call['arguments']}}]},
            'finish_reason': None}]},
        {'choices': [{'delta': {}, 'finish_reason': 'tool_calls'}]},
    ]


def sse_payload(events):
    payload = bytearray()
    for event in events:
        payload += b'data: '
        payload += json.dumps(event, ensure_ascii=False).encode('utf-8')
        payload += b'\n\n'
    return bytes(payload)


def last_user_text(body):
    if isinstance(body.get('input'), list):
        for item in reversed(body['input']):
            if isinstance(item, dict) and item.get('role') == 'user':
                return item.get('content')
    if isinstance(body.get('messages'), list):
        for item in reversed(body['messages']):
            if isinstance(item, dict) and item.get('role') == 'user':
                return item.get('content')
    return None


def function_outputs(body):
    outputs = []
    if isinstance(body.get('input'), list):
        for item in body['input']:
            if isinstance(item, dict) and item.get('type') == 'function_call_output':
                outputs.append((item.get('call_id'), item.get('output')))
    if isinstance(body.get('messages'), list):
        for item in body['messages']:
            if isinstance(item, dict) and item.get('role') == 'tool':
                outputs.append((item.get('tool_call_id'), item.get('content')))
    return outputs


class FakeServer(http.server.ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self):
        super().__init__(('127.0.0.1', 0), Handler)
        self.lock = threading.Lock()
        self.condition = threading.Condition(self.lock)
        self.requests = []
        self.tempdir = None

    def record(self, path, body):
        with self.condition:
            self.requests.append({'path': path, 'body': body})
            self.condition.notify_all()

    def requests_for(self, needle):
        with self.lock:
            return [r for r in self.requests if needle in r['path']]

    def request_count(self, needle):
        return len(self.requests_for(needle))

    def wait_requests(self, needle, count, timeout=8.0):
        deadline = time.time() + timeout
        with self.condition:
            while sum(1 for r in self.requests if needle in r['path']) < count:
                remaining = deadline - time.time()
                if remaining <= 0:
                    raise AssertionError(
                        f'timed out waiting for request {count} on {needle}')
                self.condition.wait(remaining)

    def reset(self):
        with self.condition:
            self.requests.clear()


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *args):
        pass

    def do_POST(self):
        length = int(self.headers.get('Content-Length', 0))
        raw = self.rfile.read(length)
        try:
            body = json.loads(raw)
        except json.JSONDecodeError:
            body = {}
        self.server.record(self.path, body)
        try:
            self.dispatch(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def send_json(self, obj):
        data = json.dumps(obj).encode('utf-8')
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Connection', 'close')
        self.end_headers()
        self.wfile.write(data)
        self.close_connection = True

    def send_sse(self, payload, split_at=None):
        self.send_response(200)
        self.send_header('Content-Type', 'text/event-stream')
        self.send_header('Connection', 'close')
        self.end_headers()
        if split_at is None:
            self.wfile.write(payload)
        else:
            self.wfile.write(payload[:split_at])
            self.wfile.flush()
            time.sleep(0.05)
            self.wfile.write(payload[split_at:])
        self.wfile.flush()
        self.close_connection = True

    def dispatch(self, body):
        path = self.path
        if '/chat/completions' in path:
            if 'toolmulti' in path:
                if self.server.request_count('chat/toolmulti') == 1:
                    call = function_call('call_1', 'list_dir', '{"path":"."}')
                    self.send_sse(sse_payload(chat_tool_events(call)))
                else:
                    self.send_sse(sse_payload(chat_events('after')))
            else:
                self.send_sse(sse_payload(chat_events('hello back')))
            return

        if 'split' in path:
            payload = sse_payload(responses_events('中文答案'))
            marker = '中'.encode('utf-8')
            index = payload.find(marker)
            split_at = index + 1 if index >= 0 else len(payload) // 2
            self.send_sse(payload, split_at)
            return
        if 'jsonplain' in path:
            self.send_json(responses_json('hello back'))
            return
        if 'toolmulti' in path:
            if self.server.request_count('toolmulti') == 1:
                call = function_call('call_1', 'list_dir', '{"path":"."}')
                self.send_sse(sse_payload(responses_tool_events([call], 'before')))
            else:
                self.send_sse(sse_payload(responses_events('after')))
            return
        if 'slow' in path:
            if self.server.request_count('slow') == 1:
                time.sleep(5.0)
            self.send_sse(sse_payload(responses_events('resumed')))
            return
        if 'approval' in path:
            self.approval(path, body)
            return
        if 'modes' in path:
            self.send_json(responses_json('hello back'))
            return

        if body.get('stream'):
            self.send_sse(sse_payload(responses_events('done')))
        else:
            self.send_json(responses_json('done'))

    def approval(self, path, body):
        count = self.server.request_count(path)
        tempdir = self.server.tempdir
        if count != 1:
            self.send_json(responses_json('finished'))
            return
        if 'cancel' in path:
            calls = [
                function_call('call_1', 'run_command', json.dumps({
                    'command': 'sleep 5', 'cwd': None, 'timeout_ms': None,
                    'max_output_bytes': None})),
                function_call('call_2', 'write_file', json.dumps({
                    'path': os.path.join(tempdir, 'second.txt'),
                    'content': 'second', 'mode': 'create'})),
            ]
        elif 'readtool' in path:
            calls = [function_call('call_1', 'list_dir',
                                   json.dumps({'path': tempdir}))]
        elif '/approval/a/' in path:
            calls = [
                function_call('call_1', 'write_file', json.dumps({
                    'path': os.path.join(tempdir, 'first.txt'),
                    'content': 'first', 'mode': 'create'})),
                function_call('call_2', 'write_file', json.dumps({
                    'path': os.path.join(tempdir, 'second.txt'),
                    'content': 'second', 'mode': 'create'})),
            ]
        else:
            calls = [function_call('call_1', 'write_file', json.dumps({
                'path': os.path.join(tempdir, 'first.txt'),
                'content': 'first', 'mode': 'create'}))]
        if body.get('stream'):
            self.send_sse(sse_payload(responses_tool_events(calls)))
        else:
            self.send_json(responses_tool_json(calls))


class Pty:
    def __init__(self, binary, args, env):
        self.master, self.slave = pty.openpty()
        set_winsize(self.master, 28, 100)
        self.original = termios.tcgetattr(self.slave)
        wrapper = (
            "import fcntl,os,sys,termios;"
            "fcntl.ioctl(0,getattr(termios,'TIOCSCTTY',0x540E),0);"
            "os.execv(sys.argv[1],sys.argv[1:])"
        )
        self.proc = subprocess.Popen(
            [sys.executable, '-c', wrapper, binary] + args,
            stdin=self.slave, stdout=self.slave, stderr=self.slave,
            close_fds=True, start_new_session=True, env=env)
        self.data = bytearray()
        self.closed = False

    def write(self, data):
        if isinstance(data, str):
            data = data.encode('utf-8')
        os.write(self.master, data)

    def read_available(self, seconds=0.05):
        ready, _, _ = select.select([self.master], [], [], seconds)
        if not ready:
            return b''
        try:
            chunk = os.read(self.master, 65536)
        except OSError:
            return b''
        self.data += chunk
        return chunk

    def wait_for(self, needle, timeout=6.0):
        if isinstance(needle, str):
            needle = needle.encode('utf-8')
        deadline = time.time() + timeout
        while time.time() < deadline:
            if needle in self.data:
                return True
            if self.proc.poll() is not None:
                self.read_available(0.05)
                return needle in self.data
            self.read_available(0.05)
        return needle in self.data

    def clear(self):
        self.data.clear()

    def wait_ready(self, timeout=6.0):
        return self.wait_for(b'ready', timeout) or self.wait_for(
            b'Terminal too small', 0.5)

    def wait_ready_after(self, marker, timeout=8.0):
        if isinstance(marker, str):
            marker = marker.encode('utf-8')
        deadline = time.time() + timeout
        while time.time() < deadline:
            index = self.data.rfind(marker)
            if index >= 0 and b'ready' in self.data[index + len(marker):]:
                return True
            if self.proc.poll() is not None:
                self.read_available(0.05)
                index = self.data.rfind(marker)
                return index >= 0 and b'ready' in self.data[index + len(marker):]
            self.read_available(0.05)
        return False

    def wait_exit(self, timeout=12.0):
        try:
            self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
            raise AssertionError('process did not exit')
        while True:
            chunk = self.read_available(0.05)
            if not chunk:
                break
        return self.proc.returncode

    def termios_restored(self):
        current = termios.tcgetattr(self.slave)
        mask = termios.ICANON | termios.ECHO | termios.ISIG
        return (current[3] & mask) == (self.original[3] & mask)

    def resize(self, rows, cols):
        set_winsize(self.master, rows, cols)
        try:
            os.kill(self.proc.pid, signal.SIGWINCH)
        except ProcessLookupError:
            pass

    def signal(self, sig):
        self.proc.send_signal(sig)

    def close(self):
        if self.closed:
            return
        self.closed = True
        for fd in (self.master, self.slave):
            try:
                os.close(fd)
            except OSError:
                pass


def set_winsize(fd, rows, cols):
    fcntl.ioctl(fd, termios.TIOCSWINSZ,
                struct.pack('HHHH', rows, cols, 0, 0))


def launch(binary, base, prefix, protocol='responses', stream=True,
           extra=None, env_extra=None):
    args = ['--base-url', base + prefix, '--api-key', 'secret',
            '--model', 'test-model', '--protocol', protocol]
    if stream:
        args.append('--stream')
    if extra:
        args.extend(extra)
    env = dict(os.environ, TERM='xterm-256color', LC_ALL='C.UTF-8',
               NO_PROXY='127.0.0.1,localhost',
               no_proxy='127.0.0.1,localhost')
    env.pop('NO_COLOR', None)
    if env_extra:
        env.update(env_extra)
    return Pty(binary, args, env)


def send_exit(proc):
    proc.write(b'/exit\r')
    return proc.wait_exit()


def test_editing(binary, base, server):
    prefix = '/edit'
    proc = launch(binary, base, prefix)
    try:
        assert proc.wait_ready()
        proc.clear()
        proc.write('中文ab')
        proc.write(b'\x1bOD')
        proc.write(b'\x7f')
        proc.write('X')
        proc.write(b'\r')
        server.wait_requests(prefix, 1)
        body = server.requests_for(prefix)[0]['body']
        assert last_user_text(body) == '中文Xb', last_user_text(body)
        assert proc.wait_ready_after(b'Agent: done')

        before = server.request_count(prefix)
        proc.clear()
        proc.write(b'\r')
        time.sleep(0.4)
        assert server.request_count(prefix) == before

        proc.write(b'\x04')
        assert proc.wait_exit() == 0
        assert proc.termios_restored()
    finally:
        proc.close()


def test_multiline_and_paste(binary, base, server):
    prefix = '/multiline'
    proc = launch(binary, base, prefix)
    try:
        assert proc.wait_ready()
        proc.clear()
        proc.write('第一行')
        proc.write(b'\n')
        proc.write('第二行')
        proc.write(b'\r')
        server.wait_requests(prefix, 1)
        body = server.requests_for(prefix)[0]['body']
        assert last_user_text(body) == '第一行\n第二行', repr(last_user_text(body))
        assert proc.wait_ready_after(b'Agent: done')

        before = server.request_count(prefix)
        proc.clear()
        proc.write(b'\x1b[200~')
        proc.write('pasted\nline')
        proc.write(b'\x1b[201~')
        time.sleep(0.5)
        assert server.request_count(prefix) == before
        proc.write(b'\r')
        server.wait_requests(prefix, 2)
        body = server.requests_for(prefix)[1]['body']
        assert last_user_text(body) == 'pasted\nline', repr(last_user_text(body))
        assert proc.wait_ready_after(b'Agent: done')
        assert send_exit(proc) == 0
        assert proc.termios_restored()
    finally:
        proc.close()


def test_history_and_help(binary, base, server):
    prefix = '/history'
    proc = launch(binary, base, prefix)
    try:
        assert proc.wait_ready()
        proc.clear()
        proc.write('first\r')
        server.wait_requests(prefix, 1)
        assert proc.wait_ready_after(b'Agent: done')

        proc.clear()
        proc.write('draft')
        proc.write(b'\x1bOA')
        proc.write(b'\x1bOB')
        proc.write(b'\r')
        server.wait_requests(prefix, 2)
        body = server.requests_for(prefix)[1]['body']
        assert last_user_text(body) == 'draft', repr(last_user_text(body))
        users = [item.get('content') for item in body.get('input', [])
                 if isinstance(item, dict) and item.get('role') == 'user']
        assert 'first' in users, users
        assert proc.wait_ready_after(b'Agent: done')

        before = server.request_count(prefix)
        proc.clear()
        proc.write(b'\x03')
        time.sleep(0.2)
        proc.write(b'\r')
        time.sleep(0.4)
        assert server.request_count(prefix) == before

        proc.clear()
        proc.write(b'/help\r')
        assert proc.wait_for(b'Commands:')
        time.sleep(0.2)
        assert server.request_count(prefix) == before
        proc.write(b'\x04')
        assert proc.wait_exit() == 0
        assert proc.termios_restored()
    finally:
        proc.close()


def test_streaming_variants(binary, base, server):
    prefix = '/split'
    proc = launch(binary, base, prefix)
    try:
        assert proc.wait_ready()
        proc.clear()
        proc.write(b'hi\r')
        server.wait_requests(prefix, 1)
        assert proc.wait_for('中文答案')
        assert proc.wait_ready_after('Agent: 中文答案')
        assert send_exit(proc) == 0
    finally:
        proc.close()

    prefix = '/jsonplain'
    proc = launch(binary, base, prefix, stream=False)
    try:
        assert proc.wait_ready()
        proc.clear()
        proc.write(b'hi\r')
        server.wait_requests(prefix, 1)
        assert proc.wait_for(b'hello back')
        assert proc.wait_ready_after(b'Agent: hello back')
        assert send_exit(proc) == 0
    finally:
        proc.close()

    prefix = '/chatstream'
    proc = launch(binary, base, prefix, protocol='chat')
    try:
        assert proc.wait_ready()
        proc.clear()
        proc.write(b'hi\r')
        server.wait_requests(prefix, 1)
        assert proc.wait_for(b'hello back')
        assert proc.wait_ready_after(b'Agent: hello back')
        assert send_exit(proc) == 0
    finally:
        proc.close()

    prefix = '/toolmulti'
    proc = launch(binary, base, prefix)
    try:
        assert proc.wait_ready()
        proc.clear()
        proc.write(b'do it\r')
        server.wait_requests(prefix, 2)
        second = server.requests_for(prefix)[1]['body']
        outputs = function_outputs(second)
        assert [call_id for call_id, _ in outputs] == ['call_1'], outputs
        assert proc.wait_for(b'before')
        assert proc.wait_for(b'after')
        assert proc.wait_ready_after(b'Agent: after')
        assert send_exit(proc) == 0
    finally:
        proc.close()


def test_cancel_and_resize(binary, base, server):
    prefix = '/slow'
    proc = launch(binary, base, prefix)
    try:
        assert proc.wait_ready()
        proc.clear()
        proc.write(b'first\r')
        server.wait_requests(prefix, 1)
        time.sleep(0.3)
        proc.write(b'\x03')
        assert proc.wait_ready_after(b'[cancelled]')
        proc.clear()
        proc.write(b'second\r')
        server.wait_requests(prefix, 2)
        assert proc.wait_for(b'resumed')
        assert proc.wait_ready_after(b'Agent: resumed')
        assert send_exit(proc) == 0
    finally:
        proc.close()

    prefix = '/resize'
    proc = launch(binary, base, prefix)
    try:
        assert proc.wait_ready()
        proc.clear()
        proc.write('resize-draft')
        proc.resize(12, 40)
        time.sleep(0.2)
        assert server.request_count(prefix) == 0
        proc.clear()
        proc.resize(6, 20)
        assert proc.wait_for(b'Terminal too small')
        assert server.request_count(prefix) == 0
        proc.clear()
        proc.resize(28, 100)
        time.sleep(0.3)
        assert server.request_count(prefix) == 0
        proc.write(b'\r')
        server.wait_requests(prefix, 1)
        body = server.requests_for(prefix)[0]['body']
        assert last_user_text(body) == 'resize-draft', repr(last_user_text(body))
        assert proc.wait_ready_after(b'Agent: done')
        assert send_exit(proc) == 0
        assert proc.termios_restored()
    finally:
        proc.close()


def test_exit_and_signals(binary, base, server):
    prefix = '/signals'
    for action in ('exit', 'ctrl-d', 'sigterm', 'sighup'):
        proc = launch(binary, base, prefix)
        try:
            assert proc.wait_ready()
            if action == 'exit':
                proc.write(b'/exit\r')
                code = proc.wait_exit()
                assert code == 0, code
            elif action == 'ctrl-d':
                proc.write(b'\x04')
                code = proc.wait_exit()
                assert code == 0, code
            elif action == 'sigterm':
                proc.signal(signal.SIGTERM)
                code = proc.wait_exit()
                assert code == 143, code
            else:
                proc.signal(signal.SIGHUP)
                code = proc.wait_exit()
                assert code == 129, code
            assert proc.termios_restored()
        finally:
            proc.close()


def test_signal_during_run(binary, base, server, prefix, sig, expected):
    proc = launch(binary, base, prefix)
    try:
        assert proc.wait_ready()
        proc.clear()
        proc.write(b'run\r')
        server.wait_requests(prefix, 1)
        time.sleep(0.3)
        proc.signal(sig)
        code = proc.wait_exit()
        assert code == expected, code
        assert proc.termios_restored()
    finally:
        proc.close()


def test_mode_selection(binary, base, server):
    # --no-tui keeps the line-oriented REPL.
    prefix = '/modes'
    proc = launch(binary, base, prefix, stream=False, extra=['--no-tui'])
    try:
        assert proc.wait_for(b'TinyAgent')
        assert not proc.wait_for(b'\x1b[?2004h', 0.3)
        assert not proc.wait_for(b'\x1b[?1049h', 0.1)
        proc.write(b'/exit\r')
        assert proc.wait_exit() == 0
        assert proc.termios_restored()
    finally:
        proc.close()

    # TERM=dumb disables full-screen mode.
    proc = launch(binary, base, prefix, stream=False,
                  env_extra={'TERM': 'dumb'})
    try:
        assert proc.wait_for(b'TinyAgent')
        assert not proc.wait_for(b'\x1b[?2004h', 0.3)
        assert not proc.wait_for(b'\x1b[?1049h', 0.1)
        proc.write(b'/exit\r')
        assert proc.wait_exit() == 0
    finally:
        proc.close()

    # NO_COLOR does not disable the full-screen layout.
    proc = launch(binary, base, prefix, stream=False,
                  env_extra={'NO_COLOR': '1'})
    try:
        assert proc.wait_ready()
        assert proc.wait_for(b'\x1b[?2004h', 0.5)
        assert send_exit(proc) == 0
        assert proc.termios_restored()
    finally:
        proc.close()

    # An unusable TERM falls back to the line-oriented REPL.
    proc = launch(binary, base, prefix, stream=False,
                  env_extra={'TERM': 'tiny-agent-no-such-term'})
    try:
        assert proc.wait_for(b'full-screen UI unavailable')
        assert proc.wait_for(b'TinyAgent')
        proc.write(b'/exit\r')
        assert proc.wait_exit() == 0
    finally:
        proc.close()

    # A one-shot -p run on a PTY still uses the old output path.
    proc = launch(binary, base, prefix, stream=False,
                  extra=['-p', 'hi'])
    try:
        code = proc.wait_exit()
        assert code == 0, code
        assert not proc.wait_for(b'\x1b[?2004h', 0.1)
        assert not proc.wait_for(b'\x1b[?1049h', 0.1)
        assert b'hello back' in proc.data
    finally:
        proc.close()

    # --json with a prompt stays machine-readable.
    proc = launch(binary, base, prefix, stream=False,
                  extra=['--json', '-p', 'hi'])
    try:
        code = proc.wait_exit()
        assert code == 0, code
        assert not proc.wait_for(b'\x1b[?2004h', 0.1)
        assert not proc.wait_for(b'\x1b[?1049h', 0.1)
    finally:
        proc.close()

    # stdout redirected: no full-screen, stdin still a TTY.
    env = dict(os.environ, TERM='xterm-256color', LC_ALL='C.UTF-8',
               NO_PROXY='127.0.0.1,localhost',
               no_proxy='127.0.0.1,localhost')
    master, slave = pty.openpty()
    set_winsize(master, 28, 100)
    proc = subprocess.Popen(
        [binary, '--base-url', base + prefix, '--api-key', 'secret',
         '--model', 'test-model', '--protocol', 'responses'],
        stdin=slave, stdout=subprocess.PIPE, stderr=slave,
        close_fds=True, env=env)
    try:
        os.write(master, b'/exit\n')
        code = proc.wait(timeout=10)
        assert code == 0, code
        out = proc.stdout.read()
        assert b'\x1b[?2004h' not in out
        assert b'\x1b[?1049h' not in out
        assert out == b''
    finally:
        proc.stdout.close()
        os.close(master)
        os.close(slave)


def main():
    os.environ.update(NO_PROXY='127.0.0.1,localhost',
                      no_proxy='127.0.0.1,localhost')
    server = FakeServer()
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f'http://127.0.0.1:{server.server_port}'
    binary = sys.argv[1]
    try:
        test_editing(binary, base, server)
        test_multiline_and_paste(binary, base, server)
        test_history_and_help(binary, base, server)
        test_streaming_variants(binary, base, server)
        test_cancel_and_resize(binary, base, server)
        test_exit_and_signals(binary, base, server)
        test_signal_during_run(binary, base, server, '/slowterm',
                               signal.SIGTERM, 143)
        test_signal_during_run(binary, base, server, '/slowhup',
                               signal.SIGHUP, 129)
        test_mode_selection(binary, base, server)
    finally:
        server.shutdown()
        server.server_close()


if __name__ == '__main__':
    main()
