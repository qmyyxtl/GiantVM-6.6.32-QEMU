#!/usr/bin/env python3
"""Execute a production guest test through an already logged-in root serial console.
No guest script, log, PID or temporary file is created.
"""
import argparse
from pathlib import Path
import re
import shlex
import socket
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("script", type=Path)
parser.add_argument("--host", default="127.0.0.1")
parser.add_argument("--port", type=int, default=5555)
parser.add_argument("--timeout", type=float, default=90)
parser.add_argument("--guest-args", default="", help="arguments passed to the guest script")
parser.add_argument("--preserve-on-timeout", action="store_true", help="leave the guest test running for diagnosis")
args = parser.parse_args()
source = args.script.read_text()
compile(source, str(args.script), "exec")
payload = ("import traceback, sys\nsys.argv = " + repr([args.script.name] + shlex.split(args.guest_args)) + "\ntry:\n exec(compile(" + repr(source) + ", " +
           repr(args.script.name) + ", 'exec'), {'__name__':'__main__'})\n"
           "except SystemExit as error:\n print('\\n__GVM_TEST_PASS__' if error.code in (None, 0) else '\\n__GVM_TEST_FAIL__',flush=True)\n"
           "except BaseException:\n traceback.print_exc(); print('\\n__GVM_TEST_FAIL__',flush=True)\n"
           "else:\n print('\\n__GVM_TEST_PASS__',flush=True)\n"
           "finally:\n print('\\n__GVM_TEST_DONE__',flush=True)\n").encode()
with socket.create_connection((args.host, args.port), timeout=5) as sock:
    sock.settimeout(.25)
    sock.sendall(b"\n")
    buf = bytearray()
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        try:
            chunk = sock.recv(65536)
            if not chunk:
                raise SystemExit("serial connection closed")
            buf.extend(chunk)
        except socket.timeout:
            continue
        text = re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", buf.decode(errors="replace"))
        if re.search(r"root@[^\r\n]*#\s*$", text):
            break
    else:
        raise SystemExit("log into the guest root console before running this tool")
    command = ("unset HISTFILE; stty -echo -icanon min 1 time 0; "
               "python3 -u -c 'import sys; print(\"\\n__GVM_READY__\",flush=True); "
               "exec(sys.stdin.buffer.read(" + str(len(payload)) + "))'; "
               "stty echo icanon\n")
    sock.sendall(command.encode())
    buf = bytearray()
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        try:
            buf.extend(sock.recv(65536))
        except socket.timeout:
            continue
        if b"\r\n__GVM_READY__\r\n" in buf:
            break
    else:
        sock.sendall(b"\x03")
        raise SystemExit("guest Python handshake timed out")
    sock.sendall(payload)
    buf = bytearray()
    printed = 0
    deadline = time.monotonic() + args.timeout
    while time.monotonic() < deadline:
        try:
            chunk = sock.recv(65536)
            if not chunk:
                raise SystemExit("serial connection closed")
            buf.extend(chunk)
        except socket.timeout:
            continue
        end = buf.rfind(b"\n") + 1
        if end > printed:
            print(buf[printed:end].decode(errors="replace"), end="", flush=True)
            printed = end
        if b"\r\n__GVM_TEST_DONE__\r\n" in buf:
            raise SystemExit(0 if b"\r\n__GVM_TEST_PASS__\r\n" in buf else 1)
    if not args.preserve_on_timeout:
        sock.sendall(b"\x03")
    raise SystemExit("guest test timed out; guest state preserved" if args.preserve_on_timeout else "guest test timed out")
