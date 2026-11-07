"""End-to-end checks for the CLI --json output and exit codes.

Starts a tiny fake OpenAI Responses server, runs the real tiny-agent binary
against it, and inspects stdout/exit status. No API key or network is needed.
"""

import http.server
import json
import os
import subprocess
import sys
import threading

REASONING = {'type': 'reasoning', 'id': 'rs_1', 'summary': [],
             'encrypted_content': 'opaque'}


def message(text):
    return {'type': 'message', 'id': 'msg_1', 'role': 'assistant',
            'status': 'completed',
            'content': [{'type': 'output_text', 'text': text}]}


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        length = int(self.headers.get('Content-Length', 0))
        self.rfile.read(length)
        if self.path.startswith('/unauthorized'):
            body = b'{"error":{"code":"invalid_api_key","message":"nope"}}'
            self.send_response(401)
        else:
            body = json.dumps({'status': 'completed',
                               'output': [REASONING, message('hello back')]}).encode()
            self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass


def run(binary, args):
    return subprocess.run([binary] + args, capture_output=True, text=True,
                          timeout=30)


def main():
    # Keep local fake-server requests off any inherited HTTP proxy.
    os.environ.update(NO_PROXY='127.0.0.1,localhost',
                      no_proxy='127.0.0.1,localhost')
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    base = f'http://127.0.0.1:{server.server_port}'
    common = ['--base-url', base, '--api-key', 'secret', '--model', 'test-model',
              '--protocol', 'responses']
    binary = sys.argv[1]
    try:
        # 1) Successful run: stdout is exactly one JSON object.
        proc = run(binary, common + ['--json', '-p', 'hello'])
        assert proc.returncode == 0, (proc.returncode, proc.stderr)
        data = json.loads(proc.stdout)
        assert data['status'] == 'ok', data
        assert data['output'] == 'hello back', data
        assert data['error'] is None and data['error_code'] is None, data
        assert isinstance(data['rounds'], int) and data['rounds'] >= 1, data
        assert data['model'] == 'test-model', data
        assert data['protocol'] == 'responses', data
        assert isinstance(data['duration_ms'], int) and data['duration_ms'] >= 0

        # 2) --output-format json is equivalent to --json.
        proc = run(binary, common + ['--output-format', 'json', '-p', 'hello'])
        assert proc.returncode == 0, proc.stderr
        assert json.loads(proc.stdout)['output'] == 'hello back'

        # 3) Default text mode still prints the plain answer.
        proc = run(binary, common + ['-p', 'hello'])
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout.strip() == 'hello back', repr(proc.stdout)

        # 4) HTTP failure: JSON error object, runtime exit code, no stdout noise.
        proc = run(binary, ['--base-url', f'{base}/unauthorized', '--api-key',
                            'secret', '--model', 'test-model', '--json',
                            '-p', 'hello'])
        assert proc.returncode == 1, (proc.returncode, proc.stderr)
        data = json.loads(proc.stdout)
        assert data['status'] == 'error', data
        assert data['output'] is None, data
        assert data['error_code'] == 10, data  # AGENT_ERR_HTTP
        assert data['error'], data

        # 5) --json without a prompt is a usage error (exit 2).
        proc = run(binary, ['--json'])
        assert proc.returncode == 2, (proc.returncode, proc.stderr)
        assert 'requires a prompt' in proc.stderr, proc.stderr

        # 6) Invalid --output-format is a usage error.
        proc = run(binary, ['--output-format', 'xml', '-p', 'hello'])
        assert proc.returncode == 2, (proc.returncode, proc.stderr)
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


if __name__ == '__main__':
    main()
