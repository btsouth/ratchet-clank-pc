"""The AppImage repack (packaging/relocate.py, packaging/AppRun): store paths rewritten, no /nix."""
from paths import ROOT
import importlib.util
import os
from pathlib import Path
import secrets
import shutil
import struct
import subprocess
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("relocate", ROOT / "packaging/relocate.py")
relocate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(relocate)


class RelocateTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)
        # A bundle as nix-appimage extracts it: the store and an entrypoint link into it.
        store = self.dir / "root/nix/store"
        (store / "aaa-data/share").mkdir(parents=True)
        (store / "aaa-data/share/greeting").write_text("hello\n")
        (store / "bbb-game/bin").mkdir(parents=True)
        entry = store / "bbb-game/bin/game"
        entry.write_text("#!/bin/sh\ncat /nix/store/aaa-data/share/greeting\necho \"$@\"\n")
        entry.chmod(0o555)
        (store / "bbb-game/data").symlink_to("/nix/store/aaa-data/share")
        (store / "bbb-game/elf").write_bytes(b"\0/nix/store/aaa-data\0/nix/store\0")
        (self.dir / "root/entrypoint").symlink_to("/nix/store/bbb-game/bin/game")
        # A link name of /nix/store's length, unique to this test.
        self.link = "/tmp/.t" + secrets.token_hex(2)[:3]
        self.addCleanup(lambda: os.path.lexists(self.link) and os.unlink(self.link))

    def test_store_paths_rewritten(self):
        appdir = self.dir / "AppDir"
        counts = relocate.make_appdir(self.dir / "root", appdir, self.link)
        self.assertEqual(counts, {"files": 3, "rewritten": 2, "symlinks": 1})
        store = appdir / "store"
        self.assertEqual((store / "bbb-game/elf").read_bytes(),
                         f"\0{self.link}/aaa-data\0/nix/store\0".encode())
        self.assertEqual(os.readlink(store / "bbb-game/data"), f"{self.link}/aaa-data/share")
        self.assertTrue(os.access(store / "bbb-game/bin/game", os.X_OK))

    def test_apprun_links_and_starts(self):
        appdir = self.dir / "AppDir"
        relocate.make_appdir(self.dir / "root", appdir, self.link)
        env = dict(os.environ, APPDIR=str(appdir))
        run = subprocess.run([appdir / "AppRun", "--play"], env=env, capture_output=True, text=True)
        self.assertEqual((run.returncode, run.stdout), (0, "hello\n--play\n"), run.stderr)
        self.assertEqual(os.readlink(self.link), f"{appdir}/store")
        # Started again from another mount point: the link follows.
        moved = self.dir / "moved"
        appdir.rename(moved)
        env["APPDIR"] = str(moved)
        run = subprocess.run([moved / "AppRun"], env=env, capture_output=True, text=True)
        self.assertEqual(run.stdout, "hello\n\n", run.stderr)
        self.assertEqual(os.readlink(self.link), f"{moved}/store")

    def test_apprun_refuses_while_another_copy_runs(self):
        first, second = self.dir / "first", self.dir / "second"
        relocate.make_appdir(self.dir / "root", first, self.link)
        relocate.make_appdir(self.dir / "root", second, self.link)
        # Only the tools AppRun and the entry use: no notify-send reaching a real desktop.
        tools = self.dir / "tools"
        tools.mkdir()
        for tool in ("dirname", "readlink", "stat", "id", "ln", "cat"):
            (tools / tool).symlink_to(shutil.which(tool))
        env = dict(os.environ, APPDIR=str(first), PATH=str(tools))
        self.assertEqual(subprocess.run([first / "AppRun"], env=env, capture_output=True).returncode, 0)
        env["APPDIR"] = str(second)
        run = subprocess.run([second / "AppRun"], env=env, capture_output=True, text=True)
        self.assertEqual(run.returncode, 1)
        self.assertIn("already running", run.stderr)
        self.assertEqual(os.readlink(self.link), f"{first}/store")

    def test_apprun_refuses_what_is_not_its_link(self):
        appdir = self.dir / "AppDir"
        relocate.make_appdir(self.dir / "root", appdir, self.link)
        os.mkdir(self.link)
        self.addCleanup(lambda: os.path.isdir(self.link) and os.rmdir(self.link))
        run = subprocess.run([appdir / "AppRun"], env=dict(os.environ, APPDIR=str(appdir)),
                             capture_output=True, text=True)
        self.assertEqual(run.returncode, 1)
        self.assertIn("remove it", run.stderr)

    def test_link_must_match_store_length(self):
        with self.assertRaises(ValueError):
            relocate.make_appdir(self.dir / "root", self.dir / "AppDir", "/tmp/x")

    def test_offset_is_end_of_section_headers(self):
        image = self.dir / "runtime"
        header = bytearray(64)
        header[:5] = b"\x7fELF\x02"
        struct.pack_into("<Q", header, 0x28, 1000)
        struct.pack_into("<HH", header, 0x3A, 64, 3)
        image.write_bytes(bytes(header))
        self.assertEqual(relocate.squashfs_offset(image), 1000 + 64 * 3)


if __name__ == "__main__":
    unittest.main()
