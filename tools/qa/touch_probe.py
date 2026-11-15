#!/usr/bin/env python3
"""Bind seat input sockets and print every datagram a viewer sends.

The viewer forwards input as 12-byte datagrams (see src/citra_qt/input_socket.h)
to one unix socket per seat. Running this instead of the emulator makes the
contract visible: which seat got a touch, and at what console coordinates.

    python3 tools/qa/touch_probe.py /tmp/probe-p1.sock /tmp/probe-p2.sock

then point a viewer at those paths:

    azahar-viewer --windowed --view top \
        --seat azsh-a:/tmp/probe-p1.sock --seat azsh-b:/tmp/probe-p2.sock

Each line is `<seconds> <socket> type=<t> key=<k> x=<x> y=<y>`, where type 3 is
touch down, 4 touch move, 5 touch up, 1/2 a forwarded key, and 6 the circle pad
(whose x/y are signed, y up, so a half-deflected stick reads about 16384).
"""

import os
import select
import socket
import struct
import sys
import time

TYPES = {1: "key-down", 2: "key-up", 3: "touch-down", 4: "touch-move", 5: "touch-up",
         6: "circle-pad"}


def main() -> int:
    paths = sys.argv[1:]
    if not paths:
        print(__doc__, file=sys.stderr)
        return 2

    sockets = {}
    for path in paths:
        try:
            os.unlink(path)
        except FileNotFoundError:
            pass
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        sock.bind(path)
        sock.setblocking(False)
        sockets[sock] = os.path.basename(path)

    print(f"probe: listening on {', '.join(paths)}", flush=True)
    started = time.monotonic()
    try:
        while True:
            ready, _, _ = select.select(list(sockets), [], [], 0.2)
            for sock in ready:
                data, _ = sock.recvfrom(64)
                if len(data) < 12:
                    continue
                # type, pad[3], key, x, y - see src/citra_qt/input_socket.h
                kind, _p0, _p1, _p2, key, x, y = struct.unpack("<BBBBIHH", data[:12])
                if kind == 6:  # circle pad: x/y are the bits of signed int16s
                    x = struct.unpack("<h", struct.pack("<H", x))[0]
                    y = struct.unpack("<h", struct.pack("<H", y))[0]
                print(
                    f"{time.monotonic() - started:8.3f} {sockets[sock]:14s} "
                    f"type={kind}({TYPES.get(kind, '?')}) key={key} x={x} y={y}",
                    flush=True,
                )
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
