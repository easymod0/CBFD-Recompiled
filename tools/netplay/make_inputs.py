#!/usr/bin/env python3
"""
Writes a recording of made-up inputs for the determinism harness (host/src/netplay/harness.cpp), to
replay without anyone playing: buttons pressed and held for a while, and the stick pushed about, from a
seed, so the same seed always gives the same recording.

    make_inputs.py <out.pads> [--reads 3000] [--seed 1] [--ports 1] [--start-every 90] [--mouse]

--start-every presses Start every so many reads (0: never) on the first port, to get through the title
and menus. --mouse moves each player's mouse, gyro and scroll wheel about too, and gives each player
settings of their own (Controls) from the seed, changed now and then; otherwise there's no movement and
every player has the defaults. The recording's format is the harness's (version 3, see its header comment), with its ROM and
settings profile hashes 0, which the harness doesn't check (they aren't known here).
"""
import argparse
import random
import struct

MAGIC = b"CBFDPADS"
VERSION = 3
DEFAULT_CONTROLS = 0x0200  # the stick's Invert Y, everything else off (motion.cpp)
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
    parser.add_argument("--mouse", action="store_true")
    args = parser.parse_args()

    rng = random.Random(args.seed)
    held = [[0, 0, 0, 0] for _ in range(4)]  # buttons, stick x, stick y, reads left
    moving = [Movement(rng) for _ in range(4)]
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
            for port in range(4):
                f.write(moving[port].next() if args.mouse and port < args.ports else NO_MOTION)
    print(f"{args.out}: {args.reads} reads, {args.ports} ports, seed {args.seed}" + (", with mouse and gyro" if args.mouse else ""))


NO_MOTION = struct.pack("<hhhhbH", 0, 0, 0, 0, 0, DEFAULT_CONTROLS)


def random_controls(rng):
    """A player's Controls: each response, Mouse: Turn the Camera, each invert and the field of view (0, the
    game's own, or 35 to 80 degrees) at random."""
    return (rng.getrandbits(4) | (rng.randrange(4) << 4) | (rng.randrange(4) << 6) | (rng.randrange(4) << 8) |
            (rng.randrange(47) << 10))


class Movement:
    """A player's mouse, gyro and wheel, read by read: still for a while, then swept at a steady rate (with
    some shake) for a while; now and then a notch of the wheel; and their Controls, changed about every
    500 reads. Each read's 11 bytes (netplay::Motion: the mouse in 1/8 pixel, gyro in halves of
    recompinput's unit)."""

    def __init__(self, rng):
        self.rng = rng
        self.left = 0
        self.rate = (0, 0, 0, 0)
        self.controls = random_controls(rng)

    def next(self):
        rng = self.rng
        if self.left == 0:
            self.left = rng.randint(5, 60)
            if rng.random() < 0.4:
                self.rate = (0, 0, 0, 0)
            else:
                self.rate = (rng.randint(-400, 400), rng.randint(-250, 250), rng.randint(-300, 300), rng.randint(-300, 300))
        self.left -= 1
        values = [max(-32767, min(32767, r + rng.randint(-20, 20))) if r else 0 for r in self.rate]
        wheel = rng.choice((-1, 1)) if rng.random() < 0.01 else 0
        if rng.random() < 1 / 500:
            self.controls = random_controls(rng)
        return struct.pack("<hhhhbH", *values, wheel, self.controls)


if __name__ == "__main__":
    main()
