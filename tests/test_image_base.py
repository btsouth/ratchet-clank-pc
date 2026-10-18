"""CPU-only synthetic image: explicit relocation base and collision rejection."""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

probe = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory() as tmp:
    (Path(tmp) / ".local/share").mkdir(parents=True)
    image = Path(tmp) / "return.bin"
    code = bytes.fromhex("b8 e7 00 00 00 31 ff 0f 05") + bytes(16384 - 9)
    image.write_bytes(struct.pack("<8s6Q", b"BBPROBE2", len(code), 0, 1, 0, 0, 0)
                     + struct.pack("<3Q", 0, len(code), 5) + code)
    for base, expected in [("0x80000000", "Image base: 0x80000000"),
                           ("0x80000001", "invalid BB_IMAGE_BASE"),
                           ("0x400000", "BB_IMAGE_BASE is unavailable")]:
        env = dict(os.environ, BB_IMAGE_BASE=base, HOME=tmp, XDG_DATA_HOME=str(Path(tmp) / ".local/share"))
        result = subprocess.run([probe, str(image), "--cpu-only", "--timeout", "5"],
                                env=env, text=True, capture_output=True, timeout=10)
        assert expected in result.stdout + result.stderr, result
        assert (result.returncode == 0) == (base == "0x80000000"), result
print("image base: low image executes; unaligned and occupied addresses rejected")
