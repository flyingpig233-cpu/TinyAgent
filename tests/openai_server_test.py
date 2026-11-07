import http.server
import json
import subprocess
import sys
import threading
import time


REASONING = {'type': 'reasoning', 'id': 'rs_1', 'summary': [],
             'encrypted_content': 'opaque-context'}
CALL = {'type': 'function_call', 'id': 'fc_1', 'call_id': 'call_1',
        'name': 'weather', 'arguments': '{}'}


def response_message(text):
    return {'type': 'message', 'id': 'msg_1', 'role': 'assistant',
            'status': 'completed',
            'content': [{'type': 'output_text', 'text': text}]}


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        body = self.rfile.read(int(self.headers['Content-Length']))
        if self.path.startswith('/slow/'):
            time.sleep(0.2)
        try:
            request = json.loads(body)
            assert self.headers.get('Authorization') == 'Bearer secret'
            assert self.headers.get('Content-Type') == 'application/json'
            assert request.get('model') == 'test-model'
            responses = self.path.endswith('/responses')
            assert responses or self.path.endswith('/chat/completions')
            if responses:
                assert 'messages' not in request
                assert request.get('store') is False
                assert 'reasoning.encrypted_content' in request.get('include', [])
                history = request['input']
            else:
                assert 'input' not in request
                history = request['messages']
            if self.path.startswith('/tools/'):
                tools = request['tools']
                assert tools[0]['type'] == 'function'
                function = tools[0] if responses else tools[0]['function']
                assert function['name'] == 'weather'
                assert function['strict'] is False
                assert isinstance(function['parameters'], dict)
                if len(history) == 1:
                    assert history == [{'role': 'user', 'content': 'tool please'}]
                    if responses:
                        reply = {'status': 'completed', 'output': [REASONING, CALL]}
                    else:
                        reply = {'choices': [{'message': {'content': None,
                            'tool_calls': [{'id': 'call_1', 'type': 'function',
                                            'function': {'name': 'weather', 'arguments': '{}'}}]},
                            'finish_reason': 'tool_calls'}]}
                else:
                    if responses:
                        assert history == [
                            {'role': 'user', 'content': 'tool please'}, REASONING, CALL,
                            {'type': 'function_call_output', 'call_id': 'call_1', 'output': 'sunny'}]
                    else:
                        assert len(history) == 3
                        assert history[1]['tool_calls'][0]['id'] == 'call_1'
                        assert history[2] == {'role': 'tool', 'content': 'sunny', 'tool_call_id': 'call_1'}
                    reply = self.text_reply(responses, 'forecast')
            elif self.path.startswith('/sessions/'):
                system = history[0]['content']
                assert system in ('session-a', 'session-b')
                assert history[1] == {'role': 'user', 'content': 'hello'}
                if len(history) > 2:
                    assert system == 'session-a'
                    if responses:
                        assert history[2:4] == [REASONING, response_message('hello back')]
                        assert history[4:] == [{'role': 'user', 'content': 'followup'}]
                    else:
                        assert history[2:] == [{'role': 'assistant', 'content': 'hello back'},
                                               {'role': 'user', 'content': 'followup'}]
                reply = self.text_reply(responses, 'hello back')
            else:
                assert history == [{'role': 'user', 'content': 'hello'}]
                reply = self.text_reply(responses, 'hello back')
        except (ValueError, KeyError, AssertionError, TypeError) as exc:
            self.send_error(400, str(exc))
            return

        status = 200
        body = json.dumps(reply).encode()
        if self.path.startswith('/http_error/'):
            status = 401
            body = b'{"error":{"code":"invalid_api_key","message":"test unauthorized"}}'
        elif self.path.startswith('/invalid/'):
            body = b'not json'
        elif self.path.startswith('/nul/'):
            body += b'\0junk'
        elif self.path.startswith('/failed/'):
            body = b'{"status":"failed","error":{"code":"server_error","message":"try again"}}'
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    @staticmethod
    def text_reply(responses, text):
        if responses:
            return {'status': 'completed', 'output': [REASONING, response_message(text)]}
        return {'choices': [{'message': {'content': text}, 'finish_reason': 'stop'}]}


server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
try:
    sys.exit(subprocess.call([sys.argv[1], f'http://127.0.0.1:{server.server_port}'], timeout=30))
finally:
    server.shutdown()
    server.server_close()
    thread.join()
