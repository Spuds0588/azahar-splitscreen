#!/usr/bin/env python3
"""Set the emulated 3DS console nickname (CFG username block) in an Azahar slot.

The Qt room dialog takes the nickname from the emulated console's config save
(CFG block 0xA0000), not from any Azahar setting, and a fresh slot has it
empty — which makes "Connect To Room" fail validation. This writes the block
directly so a scripted slot has a valid nickname.

Usage:
    tools/diagrom/set-slot-name.py <slot-dir> <name>

<slot-dir> is the directory holding `user/` (the instance's cwd). The name is
limited to 10 characters, matching the console's own limit.
"""
import glob
import os
import struct
import sys

BLOCK_ID = 0xA0000
BLOCK_SIZE = 0x1C


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__.strip().splitlines()[-1])
        return 2
    slot, name = sys.argv[1], sys.argv[2]
    if not 1 <= len(name) <= 10:
        print("name must be 1-10 characters")
        return 2

    paths = glob.glob(os.path.join(slot, "user", "nand", "data", "*", "sysdata",
                                  "00010017", "00000000", "config"))
    if not paths:
        print(f"no config save under {slot}/user/nand; boot the instance once first")
        return 1

    path = paths[0]
    with open(path, "r+b") as f:
        data = bytearray(f.read())
        total, data_off = struct.unpack_from("<HH", data, 0)
        for i in range(total):
            bid, offset, size, _flags = struct.unpack_from("<IIHH", data, 4 + i * 12)
            if bid != BLOCK_ID:
                continue
            if size != BLOCK_SIZE:
                print(f"username block is {size} bytes, expected {BLOCK_SIZE}")
                return 1
            start = offset if size > 4 else 4 + i * 12 + 4
            block = bytearray(BLOCK_SIZE)
            block[: len(name) * 2] = name.encode("utf-16-le")
            data[start : start + BLOCK_SIZE] = block
            f.seek(0)
            f.write(data)
            f.truncate()
            print(f"{os.path.basename(os.path.dirname(os.path.dirname(os.path.dirname(path))))}: "
                  f"username={name!r} (block at {start:#x})")
            return 0

    print(f"username block 0x{BLOCK_ID:X} not present in {path}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
