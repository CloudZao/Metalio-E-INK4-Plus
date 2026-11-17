from pathlib import Path
root = Path(__file__).resolve().parent
with (root / "assets.bin").open("wb") as output:
    for base in range(0, 16 * 1024 * 1024, 65536):
        output.write(bytes(((i * 37) ^ (i >> 8) ^ (i >> 16) ^ 0x5a) & 255 for i in range(base, base + 65536)))
