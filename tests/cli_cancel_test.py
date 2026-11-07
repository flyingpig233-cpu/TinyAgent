"""End-to-end checks for cooperative cancellation and the TTY status line.

The fake server holds a response open; the test sends SIGINT to the real
tiny-agent binary and verifies that the in-flight request is aborted quickly
instead of waiting for the full timeout.
"""

import http.server
import json
import os
import pty
import select
import signal
import subprocess
import sys
import threading
import time

CANCEL_EXIT = 130
AGENT_ERR_CANCELLED = 14
HOLD_SECONDS = 6.0

REASONING = {'type': 'reasoning', 'id': 'rs_1', 'summary': [],
             'encrypted_content': 'opaque'}


def message(text):
    return {'type': 'message', 'id': 'msg_1', 'role': 'assistant',
            'status': 'completed',
            'content': [{'type': 'output_text', 'text': text}]}


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *args):
        pass

    def do_POST(self):
        length = int(self.headers.get('Content-Length', 0))
        self.rfile.read(length)

        if '/slowstream' in self.path:
            self._stream()
        elif '/slowjson' in self.path:
            self._json()
        else:
            self.send_error(404)

    def _stream(self):
        self.send_response(200)
        self.send_header('Content-Type', 'text/event-stream')
        self.send_header('Connection', 'close')
        self.end_headers()
        try:
            event = {'type': 'response.output_text.delta', 'delta': 'hello'}
            self.wfile.write(f'data: {json.dumps(event)}\n\n'.encode())
            self.wfile.flush()
            time.sleep(HOLD_SECONDS)
            done = {'type': 'response.completed',
                    'response': {'status': 'completed',
                                 'output': [REASONING, message('hello back')]}}
            self.wfile.write(f'data: {json.dumps(done)}\n\n'.encode())
        except (BrokenPipeError, ConnectionResetError):
            pass

    def _json(self):
        body = json.dumps({'status': 'completed',
                           'output': [REASONING, message('hello back')]}).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Connection', 'close')
        self.end_headers()
        try:
            self.wfile.flush()
            time.sleep(HOLD_SECONDS)
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass


def wait_for_exit(proc, timeout):
    try:
        out, err = proc.communicate(timeout=timeout)
        return proc.returncode, out, err
    except subprocess.TimeoutExpired:
        proc.kill()
        out, err = proc.communicate()
        raise AssertionError(
            f'process did not exit within {timeout}s (cancellation failed)')


def read_available(fd, seconds=1.0):
    """Reads whatever the pty master has within the window."""
    chunks = []
    deadline = time.time() + seconds
    while time.time() < deadline:
        ready, _, _ = select.select([fd], [], [], 0.1)
        if not ready:
            continue
        try:
            chunk = os.read(fd, 4096)
        except OSError:
            break
        if not chunk:
            break
        chunks.append(chunk)
    return b''.join(chunks)


def main():
    # Keep local fake-server requests off any inherited HTTP proxy.
    os.environ.update(NO_PROXY='127.0.0.1,localhost',
                      no_proxy='127.0.0.1,localhost')
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f'http://127.0.0.1:{server.server_port}'
    binary = sys.argv[1]

    def common(prefix):
        return ['--base-url', f'{base}{prefix}', '--api-key', 'secret',
                '--model', 'test-model', '--protocol', 'responses',
                '--timeout', '30000', '-p', 'hi']

    try:
        # 1) Non-interactive streaming run: SIGINT aborts within the timeout.
        proc = subprocess.Popen([binary] + common('/slowstream') + ['--stream'],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                text=True)
        time.sleep(0.8)
        started = time.time()
        proc.send_signal(signal.SIGINT)
        code, out, err = wait_for_exit(proc, 5)
        elapsed = time.time() - started
        assert code == CANCEL_EXIT, (code, out, err)
        assert elapsed < 5, f'took {elapsed:.1f}s to cancel'
        assert 'hello' in out, repr(out)

        # 2) Non-streaming JSON run: aborting emits an error object and 130.
        proc = subprocess.Popen([binary] + common('/slowjson') + ['--json'],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                text=True)
        time.sleep(0.8)
        proc.send_signal(signal.SIGINT)
        code, out, err = wait_for_exit(proc, 5)
        assert code == CANCEL_EXIT, (code, out, err)
        data = json.loads(out)
        assert data['status'] == 'error', data
        assert data['error_code'] == AGENT_ERR_CANCELLED, data
        assert data['output'] is None, data

        # 3) TTY run: the live status line is visible while waiting, and the
        #    prompt/status live on the terminal rather than polluting stdout.
        env = dict(os.environ, TERM='xterm')
        env.pop('NO_COLOR', None)
        master, slave = pty.openpty()
        proc = subprocess.Popen([binary] + common('/slowjson'), stdin=slave,
                                stdout=slave, stderr=slave, close_fds=True,
                                env=env)
        os.close(slave)
        try:
            screen = read_available(master, 1.0)
            assert b'thinking' in screen, screen
            proc.send_signal(signal.SIGINT)
            code, _, _ = wait_for_exit(proc, 5)
            assert code == CANCEL_EXIT, code
            screen += read_available(master, 0.5)
        finally:
            try:
                os.close(master)
            except OSError:
                pass
    finally:
        server.shutdown()
        server.server_close()


if __name__ == '__main__':
    main()
