#!/usr/bin/env python3
"""Read REVMHIST files: the full CPU bus history REVM writes with --history.

The format is described in src/revm/src/debug/History.hpp. This module loads
a file into numpy arrays; the load-story report builds on it.

    revm_history.py verify FILE --expect RAM64K.bin
        Replays every RAM write and injected byte onto the recorded start
        RAM and compares the result with RAM64K.bin (for example
        begin/ram64k.bin when the history ends at BEGIN).
"""

import argparse
import json
import struct
import sys

import numpy as np

try:
    from compression import zstd
except ImportError:  # Python < 3.14
    import zstandard

    class zstd:  # noqa: N801 - mimic the stdlib module
        @staticmethod
        def decompress(data):
            return zstandard.ZstdDecompressor().decompressobj().decompress(data)


EXEC, READ_RAM, READ_ROM, READ_IO, WRITE_RAM, WRITE_IO, DMA, TICK = range(8)
END = 0xFF
KIND_NAMES = {EXEC: "exec", READ_RAM: "read", READ_ROM: "read-rom", READ_IO: "read-io",
              WRITE_RAM: "write", WRITE_IO: "write-io", DMA: "dma", TICK: "tick"}

RECORD = np.dtype([("kind", "u1"), ("value", "u1"), ("addr", "<u2"), ("dcycle", "u1")])


class History:
    """A decoded history: start state plus one row per bus record."""

    def __init__(self, path):
        with open(path, "rb") as f:
            raw = zstd.decompress(f.read())
        if raw[:8] != b"REVMHIST":
            raise ValueError(f"{path}: not a REVMHIST file")
        version, _reserved, self.start_cycle = struct.unpack_from("<HHQ", raw, 8)
        if version != 1:
            raise ValueError(f"{path}: unsupported REVMHIST version {version}")
        pos = 20
        (meta_len,) = struct.unpack_from("<I", raw, pos)
        pos += 4
        self.meta = json.loads(raw[pos:pos + meta_len].decode("utf-8"))
        pos += meta_len
        self.start_ram = np.frombuffer(raw, dtype=np.uint8, count=0x10000, offset=pos).copy()
        pos += 0x10000
        self.start_color = np.frombuffer(raw, dtype=np.uint8, count=0x400, offset=pos).copy()
        pos += 0x400
        self.start_ddr, self.start_pr, self.start_pc = struct.unpack_from("<BBH", raw, pos)
        pos += 4

        # Records run up to the End record, which is followed by u64 last cycle.
        body = len(raw) - pos - 8
        if body % RECORD.itemsize != 0:
            raise ValueError(f"{path}: truncated record stream")
        records = np.frombuffer(raw, dtype=RECORD, count=body // RECORD.itemsize, offset=pos)
        if len(records) == 0 or records["kind"][-1] != END:
            raise ValueError(f"{path}: missing End record")
        (self.last_cycle,) = struct.unpack_from("<Q", raw, len(raw) - 8)
        records = records[:-1]

        self.kind = records["kind"]
        self.value = records["value"]
        self.addr = records["addr"]
        self.cycle = self.start_cycle + np.cumsum(records["dcycle"], dtype=np.uint64)

    def __len__(self):
        return len(self.kind)

    def ram_writes(self):
        """Mask of records that change RAM: CPU writes to RAM and DMA bytes.

        A write to $00/$01 sets the CPU port, not RAM, so it is left out."""
        mask = (self.kind == WRITE_RAM) | (self.kind == DMA)
        return mask & (self.addr >= 2)

    def final_ram(self):
        ram = self.start_ram.copy()
        mask = self.ram_writes()
        # Later writes win: numpy assignment with repeated indices keeps the last.
        ram[self.addr[mask]] = self.value[mask]
        return ram


def cmd_verify(args):
    hist = History(args.file)
    ram = hist.final_ram()
    expect = np.fromfile(args.expect, dtype=np.uint8)
    if len(expect) != 0x10000:
        print(f"{args.expect}: expected 65536 bytes, got {len(expect)}", file=sys.stderr)
        return 2
    differ = np.nonzero(ram[2:] != expect[2:])[0] + 2
    print(f"{args.file}: {len(hist)} records, cycles {hist.start_cycle}..{hist.last_cycle}")
    if len(differ) == 0:
        print(f"verify: IDENTICAL to {args.expect} ($0002-$FFFF)")
        return 0
    first = int(differ[0])
    print(f"verify: {len(differ)} bytes differ from {args.expect}; first ${first:04X} "
          f"(history ${ram[first]:02X}, expected ${expect[first]:02X})")
    return 1


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    verify = sub.add_parser("verify", help="rebuild the end RAM and compare it")
    verify.add_argument("file")
    verify.add_argument("--expect", required=True, help="64 KB RAM image to compare with")
    args = parser.parse_args()
    if args.command == "verify":
        return cmd_verify(args)
    return 2


if __name__ == "__main__":
    sys.exit(main())
