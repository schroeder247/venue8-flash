#!/usr/bin/env python3
"""Controller for dnx_flash.py — sends commands via Unix socket."""
import socket, json, sys

SOCK_PATH = '/tmp/dnx_flash.sock'

cmd = ' '.join(sys.argv[1:]) if len(sys.argv) > 1 else 'STATUS'

try:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(SOCK_PATH)
    s.sendall(cmd.encode())
    s.settimeout(2.0)
    data = s.recv(65536).decode()
    s.close()
    resp = json.loads(data)
    if 'state' in resp:
        print(f"[{resp['state']}]")
    if 'lines' in resp:
        for line in resp['lines']:
            print(line)
    elif 'error' in resp:
        print(f"ERROR: {resp['error']}")
    elif 'pong' in resp:
        print("pong")
    else:
        print(json.dumps(resp, indent=2))
except ConnectionRefusedError:
    print("Flash server not running")
except FileNotFoundError:
    print(f"Socket {SOCK_PATH} not found — server not running")
