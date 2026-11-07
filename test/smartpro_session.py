"""Regenerated public SDL/XTest -> stub core -> resident protocol regression.

Based on the earlier successful session-test transcript, not physical contacts.
Requires Linux/Xvfb/XTest/xdotool. No ROM, target sysroot or hardware.
"""
import ctypes as C
import os
from pathlib import Path
import select
import socket
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
x = C.CDLL('libX11.so.6')
xt = C.CDLL('libXtst.so.6')
x.XOpenDisplay.argtypes = [C.c_char_p]
x.XOpenDisplay.restype = C.c_void_p
x.XStringToKeysym.argtypes = [C.c_char_p]
x.XStringToKeysym.restype = C.c_ulong
x.XKeysymToKeycode.argtypes = [C.c_void_p, C.c_ulong]
x.XKeysymToKeycode.restype = C.c_uint
x.XSync.argtypes = [C.c_void_p, C.c_int]
x.XCloseDisplay.argtypes = [C.c_void_p]
xt.XTestFakeKeyEvent.argtypes = [C.c_void_p, C.c_uint, C.c_int, C.c_ulong]
display = x.XOpenDisplay(None)
assert display, 'X11 display required'


def keys(*events):
    for name, down in events:
        code = x.XKeysymToKeycode(display, x.XStringToKeysym(name.encode()))
        assert code and xt.XTestFakeKeyEvent(display, code, down, 0)
    x.XSync(display, 0)


def tap(name, duration=.012):
    keys((name, True))
    time.sleep(duration)
    keys((name, False))


with tempfile.TemporaryDirectory(prefix='smartpro-session-') as tmp:
    path = tmp + '/socket'
    env = dict(os.environ, SDL_VIDEODRIVER='x11', SDL_AUDIODRIVER='dummy',
               SDL_RENDER_DRIVER='opengl')
    with open(tmp + '/log', 'w') as log:
        process = subprocess.Popen([str(ROOT/'build/smartpro/diatom'), '--socket', path],
                                   env=env, stdout=log, stderr=log)
        try:
            for _ in range(100):
                if os.path.exists(path):
                    break
                assert process.poll() is None, 'startup exit'
                time.sleep(.05)
            connection = socket.socket(socket.AF_UNIX)
            connection.connect(path)
            buffer = b''

            def receive(duration=.08):
                global buffer
                lines = []
                deadline = time.monotonic() + duration
                while True:
                    while b'\n' in buffer:
                        line, buffer = buffer.split(b'\n', 1)
                        lines.append(line.decode())
                    left = deadline - time.monotonic()
                    if left <= 0 or not select.select([connection], [], [], left)[0]:
                        return lines
                    chunk = connection.recv(65536)
                    assert chunk, 'socket closed'
                    buffer += chunk

            def send(line):
                connection.sendall((line + '\n').encode())

            def expect(prefix):
                lines = []
                for _ in range(40):
                    lines += receive(.05)
                    if any(line.startswith(prefix) for line in lines):
                        return lines
                raise AssertionError((prefix, lines))

            def resume():
                send('RESUME')
                lines = expect('RUNNING') + receive(.12)
                assert 'PAUSED' not in lines, 'tap reopened menu after resume'

            expect('READY')
            send('RUN\tcore=' + str(ROOT/'build/smartpro/stubcore.so'))
            expect('RUNNING')
            window = subprocess.check_output(['xdotool', 'search', '--name', '^diatom$'])
            subprocess.run(['xdotool', 'windowfocus', window.decode().splitlines()[0]], check=True)
            # RUNNING precedes warmup. A socket reply from the frame loop is
            # the barrier; a tap during warmup is not the behavior under test.
            send('SPEED')
            assert 'SPEED\tx=1' in expect('SPEED')
            for speed in (1, 2):
                if speed == 2:
                    keys(('Tab', True))
                    time.sleep(.08)
                    tap('w', .60)
                    assert expect('SPEED\tx=2').count('SPEED\tx=2') == 1
                    keys(('Tab', False))
                    assert 'PAUSED' not in receive(.15), 'speed chord paused'
                for i in range(30):
                    time.sleep((i % 7) * .001)
                    tap('Tab')
                    assert receive(.15).count('PAUSED') == 1, f'{speed}x missed tap {i}'
                    resume()
                keys(('Tab', True), ('Tab', False))
                assert receive(.15).count('PAUSED') == 1, 'same-poll MENU tap lost'
                resume()
                keys(('Tab', True))
                time.sleep(.08)
                send('PAUSE')
                expect('PAUSED')
                resume()
                keys(('Tab', False))
                assert 'PAUSED' not in receive(.15), 'held MENU reopened on resume'
                for level in ('equal', 'minus'):
                    keys(('Tab', True))
                    time.sleep(.08)
                    keys((level, True), (level, False), ('Tab', False))
                    assert 'PAUSED' not in receive(.15), 'level chord paused'
                    tap(level)
                    assert 'PAUSED' not in receive(.15), 'plain volume paused'
                tap('w')
                assert 'PAUSED' not in receive(.15), 'plain R1 paused'
                send('SPEED')
                assert f'SPEED\tx={speed}' in expect('SPEED'), 'chord stepped twice'
                print(f'PASS: {speed}x 30 taps, same-poll tap, held resume, level/speed controls', flush=True)
            send('STOP')
            expect('EXIT')
            send('QUIT')
            process.wait(timeout=3)
            assert process.returncode == 0
            connection.close()
        except BaseException:
            log.flush()
            print(Path(tmp + '/log').read_text()[-6000:], flush=True)
            raise
        finally:
            keys(('Tab', False), ('w', False), ('equal', False), ('minus', False))
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=3)
            x.XCloseDisplay(display)
