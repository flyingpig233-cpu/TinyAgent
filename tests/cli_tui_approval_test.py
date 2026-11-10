"""End-to-end approval checks for the full-screen CLI.

Uses the PTY/fake-server helpers from cli_tui_test.py. File state and the
function_call_output items sent back to the fake server are the evidence; the
approval prompt text is only used to synchronize with the UI.
"""

import os
import shutil
import sys
import tempfile
import threading
import time

import cli_tui_test as t


def start(binary, base, prefix, extra=None):
    return t.launch(binary, base, prefix, stream=False, extra=extra)


def prompt(proc, server, prefix):
    proc.clear()
    proc.write(b'go\r')
    server.wait_requests(prefix, 1)


def wait_approval(proc):
    assert proc.wait_for(b'approval required', 6.0)


def wait_final(proc):
    assert proc.wait_ready_after(b'Agent: finished')


def path(server, name):
    return os.path.join(server.tempdir, name)


def test_deny(binary, base, server, key, suffix):
    server.tempdir = tempfile.mkdtemp()
    prefix = '/approval/n' + suffix
    proc = start(binary, base, prefix)
    try:
        assert proc.wait_ready()
        prompt(proc, server, prefix)
        wait_approval(proc)
        proc.write(key)
        if key == b'\x03':
            assert proc.wait_ready_after(b'[cancelled]')
            assert server.request_count(prefix) == 1
        else:
            server.wait_requests(prefix, 2)
            wait_final(proc)
        assert not os.path.exists(path(server, 'first.txt'))
        assert t.send_exit(proc) == 0
    finally:
        proc.close()
        shutil.rmtree(server.tempdir)


def test_allow(binary, base, server):
    server.tempdir = tempfile.mkdtemp()
    prefix = '/approval/y'
    proc = start(binary, base, prefix)
    try:
        assert proc.wait_ready()
        prompt(proc, server, prefix)
        wait_approval(proc)
        proc.write(b'y')
        server.wait_requests(prefix, 2)
        with open(path(server, 'first.txt'), encoding='utf-8') as handle:
            assert handle.read() == 'first'
        wait_final(proc)
        assert t.send_exit(proc) == 0
    finally:
        proc.close()
        shutil.rmtree(server.tempdir)


def test_allow_all(binary, base, server):
    server.tempdir = tempfile.mkdtemp()
    prefix = '/approval/a'
    proc = start(binary, base, prefix)
    try:
        assert proc.wait_ready()
        prompt(proc, server, prefix)
        wait_approval(proc)
        proc.write(b'a')
        server.wait_requests(prefix, 2)
        with open(path(server, 'first.txt'), encoding='utf-8') as handle:
            assert handle.read() == 'first'
        with open(path(server, 'second.txt'), encoding='utf-8') as handle:
            assert handle.read() == 'second'
        wait_final(proc)
        assert t.send_exit(proc) == 0
    finally:
        proc.close()
        shutil.rmtree(server.tempdir)


def test_auto_approve(binary, base, server):
    server.tempdir = tempfile.mkdtemp()
    prefix = '/approval/yes'
    proc = start(binary, base, prefix, extra=['--yes'])
    try:
        assert proc.wait_ready()
        prompt(proc, server, prefix)
        server.wait_requests(prefix, 2)
        with open(path(server, 'first.txt'), encoding='utf-8') as handle:
            assert handle.read() == 'first'
        wait_final(proc)
        assert t.send_exit(proc) == 0
    finally:
        proc.close()
        shutil.rmtree(server.tempdir)


def test_read_only(binary, base, server):
    server.tempdir = tempfile.mkdtemp()
    prefix = '/approval/readonly'
    proc = start(binary, base, prefix, extra=['--read-only'])
    try:
        assert proc.wait_ready()
        prompt(proc, server, prefix)
        server.wait_requests(prefix, 2)
        assert not os.path.exists(path(server, 'first.txt'))
        wait_final(proc)
        assert t.send_exit(proc) == 0
    finally:
        proc.close()
        shutil.rmtree(server.tempdir)


def test_read_only_tool(binary, base, server):
    server.tempdir = tempfile.mkdtemp()
    prefix = '/approval/readtool'
    proc = start(binary, base, prefix)
    try:
        assert proc.wait_ready()
        prompt(proc, server, prefix)
        server.wait_requests(prefix, 2)
        assert not proc.wait_for(b'approval required', 0.3)
        second = server.requests_for(prefix)[1]['body']
        outputs = t.function_outputs(second)
        assert [call_id for call_id, _ in outputs] == ['call_1'], outputs
        wait_final(proc)
        assert t.send_exit(proc) == 0
    finally:
        proc.close()
        shutil.rmtree(server.tempdir)


def test_cancel_during_tool(binary, base, server):
    server.tempdir = tempfile.mkdtemp()
    prefix = '/approval/cancel'
    proc = start(binary, base, prefix)
    try:
        assert proc.wait_ready()
        prompt(proc, server, prefix)
        wait_approval(proc)
        proc.write(b'y')
        time.sleep(0.6)
        proc.write(b'\x03')
        assert proc.wait_ready_after(b'[cancelled]')
        assert not os.path.exists(path(server, 'second.txt'))

        proc.clear()
        proc.write(b'continue\r')
        server.wait_requests(prefix, 2)
        second = server.requests_for(prefix)[1]['body']
        outputs = t.function_outputs(second)
        assert sorted(call_id for call_id, _ in outputs) == ['call_1', 'call_2']
        assert not os.path.exists(path(server, 'second.txt'))
        wait_final(proc)
        assert t.send_exit(proc) == 0
    finally:
        proc.close()
        shutil.rmtree(server.tempdir)


def main():
    os.environ.update(NO_PROXY='127.0.0.1,localhost',
                      no_proxy='127.0.0.1,localhost')
    server = t.FakeServer()
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f'http://127.0.0.1:{server.server_port}'
    binary = sys.argv[1]
    try:
        test_deny(binary, base, server, b'n', '1')
        test_deny(binary, base, server, b'\r', '2')
        test_deny(binary, base, server, b'\x03', '3')
        test_allow(binary, base, server)
        test_allow_all(binary, base, server)
        test_auto_approve(binary, base, server)
        test_read_only(binary, base, server)
        test_read_only_tool(binary, base, server)
        test_cancel_during_tool(binary, base, server)
    finally:
        server.shutdown()
        server.server_close()


if __name__ == '__main__':
    main()
