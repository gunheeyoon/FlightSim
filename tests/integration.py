"""Black-box checks against the real TCP server; no third-party packages."""
import pathlib
import socket
import subprocess
import sys
import time


def main():
    executable = str(pathlib.Path(sys.argv[1]).resolve())
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        port = probe.getsockname()[1]
    process = subprocess.Popen([executable, '127.0.0.1', str(port)],
                               stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    sockets = []

    def connect(role=None):
        for attempt in range(50):
            try:
                sock = socket.create_connection(('127.0.0.1', port), timeout=2)
                break
            except OSError:
                if process.poll() is not None or attempt == 49:
                    raise
                time.sleep(0.05)
        sockets.append(sock)
        stream = sock.makefile('r', encoding='ascii')
        assert stream.readline().strip() == 'WELCOME aircraft_server_v1'
        if role:
            sock.sendall(f'HELLO {role}\n'.encode())
            assert stream.readline().strip() == f'OK HELLO {role}'
        return sock, stream

    def request(peer, command):
        sock, stream = peer
        sock.sendall((command + '\n').encode())
        return stream.readline().strip()

    def state(peer):
        parts = request(peer, 'GET').split()
        assert parts[0] == 'STATE' and len(parts) == 12
        return [float(value) for value in parts[2:9]], parts[9:]

    try:
        control = connect('CONTROL')
        assert state(control) == ([0.0] * 7, ['0'] * 3)
        for field, value in [('longitude', 129), ('latitude', 35),
                             ('altitude', 1000), ('airspeed', 120)]:
            assert request(control, f'SET {field} {value}').startswith('OK ')
        baseline = state(control)
        for command in ['SET longitude 181', 'SET latitude -91', 'SET airspeed -1',
                        'SET altitude nan', 'SET altitude inf', 'SET roll 1',
                        'SET altitude 1oops', 'SET altitude 1 extra', 'FREEZE roll 2']:
            assert request(control, command).startswith('ERR '), command
            assert state(control) == baseline
        outsider = connect()
        assert request(outsider, 'GET') == 'ERR send_HELLO_first'
        assert request(outsider, 'HELLO CONTROL') == 'ERR role_in_use'
        simulator = connect('SIMULATOR')
        assert request(simulator, 'SET altitude 20').startswith('ERR ')
        assert request(control, 'UPDATE 1 2 3 4 5 6 7').startswith('ERR ')
        assert request(simulator, 'UPDATE 129 35 1000 120 10 20 30').startswith('OK ')
        for axis in ['roll', 'pitch', 'yaw']:
            assert request(control, f'FREEZE {axis} 1').startswith('OK ')
            assert simulator[1].readline().strip() == f'COMMAND FREEZE {axis} 1'
            assert simulator[1].readline().startswith('STATE ')
        assert request(simulator, 'UPDATE 130 36 1100 130 40 50 60').startswith('OK ')
        assert state(control) == ([130, 36, 1100, 130, 10, 20, 30], ['1'] * 3)
        assert request(control, 'FREEZE pitch 0').startswith('OK ')
        assert simulator[1].readline().strip() == 'COMMAND FREEZE pitch 0'
        assert simulator[1].readline().startswith('STATE ')
        assert request(simulator, 'UPDATE 130 36 1100 130 40 50 60').startswith('OK ')
        assert state(control) == ([130, 36, 1100, 130, 10, 50, 30], ['1', '0', '1'])
        assert request(control, 'SET altitude 2000').startswith('OK ')
        assert simulator[1].readline().strip() == 'COMMAND SET altitude 2000'
        assert simulator[1].readline().startswith('STATE ')
        baseline = state(control)
        assert request(simulator, 'UPDATE 0 0 0 0 0 0 360').startswith('ERR ')
        assert state(control) == baseline
        # Split a command across sends, then coalesce two complete commands.
        control[0].sendall(b'GE')
        control[0].sendall(b'T\r\nGET\n')
        assert control[1].readline().startswith('STATE ')
        assert control[1].readline().startswith('STATE ')
        outsider[0].sendall(b'x' * 4097)
        assert outsider[1].readline().strip() == 'ERR line_too_long'
        print('PASS: state, commands, validation, roles, telemetry, freeze, forwarding, framing')
    finally:
        for sock in sockets:
            sock.close()
        process.terminate()
        process.communicate(timeout=5)


if __name__ == '__main__':
    main()
