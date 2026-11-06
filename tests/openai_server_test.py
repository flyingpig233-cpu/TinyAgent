import http.server
import json
import subprocess
import sys
import threading


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        body = self.rfile.read(int(self.headers['Content-Length']))
        try:
            request = json.loads(body)
        except ValueError:
            self.send_error(400)
            return
        if (self.headers.get('Authorization') != 'Bearer secret' or
                self.headers.get('Content-Type') != 'application/json' or
                request.get('model') != 'test-model' or
                request.get('messages') != [{'role': 'user', 'content': 'hello'}]):
            self.send_error(400)
            return
        if self.path == '/http_error':
            self.send_response(401)
            body = b'{"error":"unauthorized"}'
        elif self.path == '/invalid':
            self.send_response(200)
            body = b'not json'
        else:
            self.send_response(200)
            body = b'{"choices":[{"message":{"content":"hello back"},"finish_reason":"stop"}]}'
            if self.path == '/nul':
                body += b'\0junk'
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)


server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
try:
    sys.exit(subprocess.call([sys.argv[1], f'http://127.0.0.1:{server.server_port}']))
finally:
    server.shutdown()
    server.server_close()
    thread.join()
