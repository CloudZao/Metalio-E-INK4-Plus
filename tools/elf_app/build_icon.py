"""Convert a 128x128 binary PBM (P4) into an XZIC v1 ELF resource."""
import argparse
import re
import struct
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("input", type=Path)
parser.add_argument("output", type=Path)
args = parser.parse_args()
data = args.input.read_bytes()
header = re.match(rb"P4\s+(?:#[^\r\n]*[\r\n]+\s*)*128\s+128(?:\r\n|[ \t\r\n])", data)
if not header or len(data) - header.end() != 2048:
    parser.error("需要 128x128 二进制 PBM / Expected a 128x128 binary PBM (P4)")
args.output.write_bytes(struct.pack("<4sHHHHI", b"XZIC", 1, 128, 128, 1, 2048) + data[header.end():])
