#!/usr/bin/env python3
"""
Writes a recording of made-up inputs for the determinism harness (host/src/netplay/harness.cpp), to
replay without anyone playing: buttons pressed and held for a while, and the stick pushed about, from a
seed, so the same seed always gives the same recording.

    make_inputs.py <out.pads> [--reads 3000] [--seed 1] [--ports 1] [--start-every 90]

--start-every presses Start every so many reads (0: never) on the first port, to get through the title
and menus. The recording's format is the harness's (see its header comment), with its ROM and settings
profile hashes 0, which the harness doesn't check (they aren't known here).
"""
import argparse
import random
import struct

MAGIC = b"CBFDPADS"
VERSION = 2
START = 0x1000
BUTTONS = [0x8000, 0x4000, 0x2000, 0x0020, 0x0010, 0x0008, 0x0004, 0x0002, 0x0001]  # A B Z L R C-pad
NO_RESPONSE = 8  # err_no for a controller that isn't there (CONT_NO_RESPONSE_ERROR)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("out")
    parser.add_argument("--reads", type=int, default=3000)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--ports", type=int, default=1, choices=range(1, 5))
    parser.add_argument("--start-every", type=int, default=90)
    args = parser.parse_args()

    rng = random.Random(args.seed)
    held = [[0, 0, 0, 0] for _ in range(4)]  # buttons, stick x, stick y, reads left
    with open(args.out, "wb") as f:
        f.write(MAGIC + struct.pack("<IIQQ", VERSION, args.ports, 0, 0))
        for read in range(args.reads):
            f.write(bytes([4]))
            for port in range(4):
                if port >= args.ports:
                    f.write(struct.pack("<Hbbb", 0, 0, 0, NO_RESPONSE))
                    continue
                pad = held[port]
                if pad[3] == 0:
                    pad[0] = sum(b for b in BUTTONS if rng.random() < 0.15)
                    pad[1] = rng.randint(-80, 80) if rng.random() < 0.6 else 0
                    pad[2] = rng.randint(-80, 80) if rng.random() < 0.6 else 0
                    pad[3] = rng.randint(3, 45)
                pad[3] -= 1
                buttons = pad[0]
                if port == 0 and args.start_every and read % args.start_every < 3:
                    buttons |= START
                f.write(struct.pack("<Hbbb", buttons, pad[1], pad[2], 0))
    print(f"{args.out}: {args.reads} reads, {args.ports} ports, seed {args.seed}")


main()
