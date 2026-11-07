"""End-to-end checks for CLI streaming output.

Serves Server-Sent Events for both protocols, runs the real tiny-agent binary
with --stream, and verifies streamed text and multi-round tool handling.
"""

import http.server
import json
import subprocess
import sys
import threading

REASONING = {'type': 'reasoning', 'id': 'rs_1', 'summary': [],
             'encrypted_content': 'opaque'}
CALL = {'type': 'function_call', 'id': 'fc_1', 'call_id': 'call_1',
        'name': 'list_dir', 'arguments': '{"path":"."}'}


def message(text):
    return {'type': 'message', 'id': 'msg_1', 'role': 'assistant',
            'status': 'completed',
            'content': [{'type': 'output_text', 'text': text}]}


def responses_text(text):
    events = []
    for chunk in (text[:6], text[6:]):
        events.append({'type': 'response.output_text.delta', 'delta': chunk})
    events.append({'type': 'response.completed',
                   'response': {'status': 'completed',
                                'output': [REASONING, message(text)]}})
    return events


def responses_tool():
    return [{'type': 'response.completed',
             'response': {'status': 'completed',
                          'output': [REASONING, CALL]}}]


def chat_text(text):
    return [
        {'choices': [{'delta': {'content': text[:6]}, 'finish_reason': None}]},
        {'choices': [{'delta': {'content': text[6:]}, 'finish_reason': None}]},
        {'choices': [{'delta': {}, 'finish_reason': 'stop'}]},
    ]


def chat_tool():
    return [
        {'choices': [{'delta': {'tool_calls': [
            {'index': 0, 'id': 'call_1', 'type': 'function',
             'function': {'name': 'list_dir', 'arguments': ''}}]},
            'finish_reason': None}]},
        {'choices': [{'delta': {'tool_calls': [
            {'index': 0, 'function': {'arguments': '{"path":"."}'}}]},
            'finish_reason': None}]},
        {'choices': [{'delta': {}, 'finish_reason': 'tool_calls'}]},
    ]


class Handler(http.server.BaseHTTPRequestHandler):
    counts = {}

    def log_message(self, *args):
        pass

    def do_POST(self):
        length = int(self.headers.get('Content-Length', 0))
        self.rfile.read(length)
        key = self.path
        self.counts[key] = self.counts.get(key, 0) + 1
        n = self.counts[key]

        if self.path.endswith('/chat/completions'):
            if '/chattools/' in self.path:
                events, done = (chat_tool() if n == 1
                                else chat_text('done')), True
            else:
                events, done = chat_text('hello back'), True
            sse = True
        elif '/tools/' in self.path:
            events, done = (responses_tool() if n == 1
                            else responses_text('done')), False
            sse = True
        elif '/jsonplain/' in self.path:
            body = json.dumps({'status': 'completed',
                               'output': [REASONING, message('hello back')]}).encode()
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        else:
            events, done = responses_text('hello back'), False
            sse = True

        self.send_response(200)
        self.send_header('Content-Type', 'text/event-stream')
        self.end_headers()
        try:
            for event in events:
                self.wfile.write(f'data: {json.dumps(event)}\n\n'.encode())
            if done:
                self.wfile.write(b'data: [DONE]\n\n')
        except (BrokenPipeError, ConnectionResetError):
            pass


def run(binary, args):
    return subprocess.run([binary] + args, capture_output=True, text=True,
                          timeout=30)


def main():
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f'http://127.0.0.1:{server.server_port}'
    binary = sys.argv[1]

    def common(prefix, protocol):
        return ['--base-url', f'{base}{prefix}', '--api-key', 'secret',
                '--model', 'test-model', '--protocol', protocol]

    try:
        # 1) Responses streaming prints the assembled text exactly once.
        proc = run(binary, common('/plain', 'responses') + ['--stream', '-p', 'hi'])
        assert proc.returncode == 0, (proc.returncode, proc.stderr)
        assert proc.stdout.strip() == 'hello back', repr(proc.stdout)
        assert proc.stdout.count('hello back') == 1, repr(proc.stdout)

        # 2) Chat Completions streaming.
        proc = run(binary, common('/chatstream', 'chat') + ['--stream', '-p', 'hi'])
        assert proc.returncode == 0, (proc.returncode, proc.stderr)
        assert proc.stdout.strip() == 'hello back', repr(proc.stdout)

        # 3) A streamed function call is executed and the run continues.
        proc = run(binary, common('/tools', 'responses') + ['--stream', '-p', 'do it'])
        assert proc.returncode == 0, (proc.returncode, proc.stderr)
        assert proc.stdout.strip() == 'done', repr(proc.stdout)

        # 3b) Same, over the Chat Completions streaming tool-call deltas.
        proc = run(binary, common('/chattools', 'chat') + ['--stream', '-p', 'do it'])
        assert proc.returncode == 0, (proc.returncode, proc.stderr)
        assert proc.stdout.strip() == 'done', repr(proc.stdout)

        # 4) --json takes precedence over --stream: one clean JSON object.
        proc = run(binary, common('/jsonplain', 'responses') +
                   ['--stream', '--json', '-p', 'hi'])
        assert proc.returncode == 0, (proc.returncode, proc.stderr)
        data = json.loads(proc.stdout)
        assert data['status'] == 'ok' and data['output'] == 'hello back', data
    finally:
        server.shutdown()
        server.server_close()


if __name__ == '__main__':
    main()
