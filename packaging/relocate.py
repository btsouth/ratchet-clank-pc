#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Repacks nix-appimage's AppImage so it runs without user namespaces.

nix-appimage's AppRun mounts the bundle's store at /nix inside a user namespace, which Ubuntu
23.10 and later restrict by default ("cannot write uid_map: Operation not permitted"). Here the
store is copied with every "/nix/store/" in file contents and symlink targets rewritten to LINK
(a path of the same length in /tmp, so binaries keep their layout), and packaging/AppRun points
LINK at the mounted AppImage before it starts the entry point.

  relocate.py offset APPIMAGE         byte offset of the squashfs image (the runtime's ELF size)
  relocate.py appdir ROOT APPDIR LINK  ROOT: the extracted bundle (nix/store, entrypoint)
"""

import os
from pathlib import Path
import stat
import struct
import sys

OLD = b"/nix/store/"


def squashfs_offset(path):
    """The AppImage runtime is an ELF file; the squashfs image follows its section headers."""
    with open(path, "rb") as stream:
        header = stream.read(64)
    if header[:5] != b"\x7fELF\x02":
        raise ValueError(f"{path}: not a 64-bit ELF AppImage")
    shoff, = struct.unpack_from("<Q", header, 0x28)
    shentsize, shnum = struct.unpack_from("<HH", header, 0x3A)
    return shoff + shentsize * shnum


def copy_store(src, dst, link):
    new = link.encode() + b"/"
    if len(new) != len(OLD):
        raise ValueError(f"{link}: must be {len(OLD) - 1} bytes long, as /nix/store is")
    counts = {"files": 0, "rewritten": 0, "symlinks": 0}
    for root, dirs, names in os.walk(src):
        out = Path(dst) / Path(root).relative_to(src)
        out.mkdir(parents=True, exist_ok=True)
        for name in dirs + names:
            source, target = Path(root) / name, out / name
            mode = source.lstat().st_mode
            if stat.S_ISLNK(mode):
                os.symlink(os.fsdecode(os.fsencode(os.readlink(source)).replace(OLD, new)), target)
                counts["symlinks"] += 1
            elif stat.S_ISREG(mode):
                data = source.read_bytes()
                if OLD in data:
                    data = data.replace(OLD, new)
                    counts["rewritten"] += 1
                target.write_bytes(data)
                target.chmod(stat.S_IMODE(mode) | stat.S_IRUSR | stat.S_IWUSR)
                counts["files"] += 1
    return counts


def make_appdir(root, appdir, link):
    root, appdir = Path(root), Path(appdir)
    entry = os.readlink(root / "entrypoint")
    if not entry.startswith("/nix/store/"):
        raise ValueError(f"entry point {entry}: not in the store")
    counts = copy_store(root / "nix" / "store", appdir / "store", link)
    template = (Path(__file__).with_name("AppRun")).read_text(encoding="utf-8")
    apprun = appdir / "AppRun"
    apprun.write_text(template.replace("@LINK@", link)
                      .replace("@ENTRY@", entry.removeprefix("/nix/store/")), encoding="utf-8")
    apprun.chmod(0o755)
    return counts


def main(argv):
    if len(argv) == 3 and argv[1] == "offset":
        print(squashfs_offset(argv[2]))
    elif len(argv) == 5 and argv[1] == "appdir":
        counts = make_appdir(argv[2], argv[3], argv[4])
        print("{files} files ({rewritten} rewritten), {symlinks} symlinks".format(**counts))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv)
