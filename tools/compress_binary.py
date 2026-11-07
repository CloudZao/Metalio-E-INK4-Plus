"""Store a binary as its little-endian size followed by a zlib stream."""

import argparse
from pathlib import Path
import struct
import zlib

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    data = args.source.read_bytes()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(struct.pack('<I', len(data)) + zlib.compress(data, 9))
