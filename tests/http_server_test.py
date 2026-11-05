import http.server
import subprocess
import sys
import threading
import time


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_PUT(self):
        body = self.rfile.read(int(self.headers['Content-Length']))
        if self.path != '/test' or self.headers.get('X-Test') != 'hello':
            self.send_error(400)
            return
        self.send_response(201)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == '/slow':
            time.sleep(0.2)
        self.send_response(404)
        self.end_headers()
        try:
            self.wfile.write(b'missing')
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_POST(self):
        body = self.rfile.read(int(self.headers['Content-Length']))
        if self.path != '/json' or self.headers.get('Content-Type') != 'application/json' or body != b'{}':
            self.send_error(400)
            return
        self.send_response(204)
        self.end_headers()


server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
try:
    sys.exit(subprocess.call([sys.argv[1], f'http://127.0.0.1:{server.server_port}']))
finally:
    server.shutdown()
    server.server_close()
    thread.join()
