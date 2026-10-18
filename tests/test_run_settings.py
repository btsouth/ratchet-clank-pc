"""Exercise run.sh across a re-exec with lightweight preparation/probe stand-ins."""
from paths import ROOT
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest


class RestartResolutionTests(unittest.TestCase):
    def run_restarts(self, explicit=False, live=False, ini_extra='', caps=None, bare_path=False):
        with tempfile.TemporaryDirectory() as directory:
            data = Path(directory)
            # Preparation is unrelated to this test; allow the real patch compiler to
            # validate writes against a single ELF load segment spanning the game image.
            out = data / 'out'
            out.mkdir()
            # Bloodborne: the built-in patches are its addresses (patches.py reads the title).
            (out / 'analysis.json').write_text(json.dumps({'sfo': {'TITLE_ID': 'CUSA03173'}}))
            elf = bytearray(120)
            struct.pack_into('<Q', elf, 0x20, 64)
            struct.pack_into('<HH', elf, 0x36, 56, 1)
            struct.pack_into('<IIQQQQQQ', elf, 64, 1, 0, 0, 0, 0, 0, 0x6000000, 0)
            (out / 'eboot.elf').write_bytes(elf)
            (data / 'eboot.bin').touch()
            config = data / 'bbport.ini'
            config.write_text('upscaler=fsr3\npreset=1\noutput_res=1280x720\n' + ini_extra)
            if caps is not None:  # the GPU check next to the probe (live_resolution=auto)
                tool = data / 'bb-gpu-capabilities'
                tool.write_text(f'#!/bin/sh\necho {caps}\n')
                tool.chmod(0o755)
            python = data / 'python'
            python.write_text(f'#!{sys.executable}\n' +
                'import subprocess, sys\n'
                'if sys.argv[1] in ("scripts/patches.py", "scripts/mods.py"):\n'
                '    sys.exit(subprocess.call([sys.executable, *sys.argv[1:]]))\n')
            python.chmod(0o755)
            probe = data / 'probe'
            probe.write_text(f'#!{sys.executable}\n' +
                'import json, os\n'
                'from pathlib import Path\n'
                'config=Path(os.environ["BB_CONFIG"])\n'
                'stage=int(os.environ.get("BB_TEST_STAGE", "0"))\n'
                'with (config.parent/"environments").open("a") as f:\n'
                '    f.write(json.dumps({key:os.environ.get(key) for key in '
                '("BB_RENDER_RES", "BB_OUTPUT_RES", "BB_AUTO_RENDER_RES")})+"\\n")\n'
                'if stage<2:\n'
                '    config.write_text("upscaler=fsr3\\npreset=4\\noutput_res="+'
                '("1280x720" if stage==0 else "1920x1080")+"\\n")\n'
                '    os.environ["BB_TEST_STAGE"]=str(stage+1)\n'
                '    os.execlp("bash", "bash", "run.sh")\n')
            probe.chmod(0o755)
            env = dict(os.environ, BB_PREBUILT='1', BB_PROBE=str(probe), PYTHON=str(python),
                       BB_DATA_DIR=str(data), BB_CONFIG=str(config), BB_GAME_DIR=str(data))
            for key in ('BB_RENDER_RES', 'BB_OUTPUT_RES', 'BB_AUTO_RENDER_RES', 'BB_TEST_STAGE'):
                env.pop(key, None)
            env.pop('BB_LIVE_RES', None)
            if explicit:
                env['BB_RENDER_RES'] = '800x450'
            if live:
                env['BB_LIVE_RES'] = '1'
            if bare_path:  # the AppImage's PATH: coreutils and bash, no sed/grep
                tools = data / 'bin'
                tools.mkdir()
                for name in ('bash', 'dirname', 'mkdir', 'realpath'):
                    (tools / name).symlink_to(shutil.which(name))
                env['PATH'] = str(tools)
            subprocess.run([shutil.which('bash'), 'run.sh'], cwd=ROOT, env=env,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True, timeout=30)
            return [json.loads(line) for line in (data / 'environments').read_text().splitlines()]

    def test_outputs_other_than_1080p_patch_the_render_size_and_restarts_recompute_it(self):
        rows = self.run_restarts()
        # 720p Quality, 720p Ultra Performance after a restart, then 1080p (live host targets).
        self.assertEqual([row['BB_RENDER_RES'] for row in rows], ['854x480', '426x240', None])
        self.assertEqual([row['BB_OUTPUT_RES'] for row in rows], ['1280x720', '1280x720', None])
        self.assertEqual([row['BB_AUTO_RENDER_RES'] for row in rows], ['1', '1', None])

    def test_live_resolution_keeps_guest_sizes_native(self):
        rows = self.run_restarts(live=True)
        self.assertTrue(all(row['BB_RENDER_RES'] is None for row in rows))
        self.assertTrue(all(row['BB_OUTPUT_RES'] is None for row in rows))
        self.assertTrue(all(row['BB_AUTO_RENDER_RES'] is None for row in rows))

    def test_live_resolution_setting_and_gpu_check(self):
        # Only the first launch matters here: the probe rewrites bbport.ini for the restarts.
        self.assertIsNone(self.run_restarts(ini_extra='live_resolution=1\n')[0]['BB_RENDER_RES'])
        self.assertEqual(self.run_restarts(ini_extra='live_resolution=0\n', caps=1)[0]['BB_RENDER_RES'],
                         '854x480')
        # Unset: off (the startup patch), whatever the GPU; auto asks the GPU check.
        self.assertEqual(self.run_restarts(caps=1)[0]['BB_RENDER_RES'], '854x480')
        self.assertIsNone(self.run_restarts(ini_extra='live_resolution=auto\n', caps=1)[0]['BB_RENDER_RES'])
        self.assertEqual(self.run_restarts(ini_extra='live_resolution=auto\n', caps=0)[0]['BB_RENDER_RES'],
                         '854x480')

    def test_live_resolution_without_sed_or_grep(self):
        # A missing sed ended run.sh (exit 127) before the game in the AppImage on NixOS.
        self.assertEqual(self.run_restarts(ini_extra='live_resolution=0\n', caps=1,
                                           bare_path=True)[0]['BB_RENDER_RES'], '854x480')
        self.assertIsNone(self.run_restarts(ini_extra='live_resolution=auto\n', caps=1,
                                            bare_path=True)[0]['BB_RENDER_RES'])

    def test_explicit_render_override_survives_restart(self):
        rows = self.run_restarts(explicit=True)
        self.assertEqual([row['BB_RENDER_RES'] for row in rows], ['800x450'] * 3)
        self.assertTrue(all(row['BB_AUTO_RENDER_RES'] is None for row in rows))


class RatchetPacingTests(unittest.TestCase):
    """The frame rate preset's pacing at the vblank rate run.sh picks (or is given)."""

    def launch(self, keys=('BB_VBLANK_HZ', 'BB_FPS_LIMIT'), title='CUSA01073', **extra):
        with tempfile.TemporaryDirectory() as directory:
            data = Path(directory)
            out = data / 'out'
            out.mkdir()
            (out / 'analysis.json').write_text(json.dumps({'sfo': {'TITLE_ID': title}}))
            (out / 'eboot.elf').write_bytes(bytes(120))  # not 1.00: no writes, pacing still set
            (data / 'eboot.bin').touch()
            config = data / 'bbport.ini'
            config.write_text('')
            python = data / 'python'
            python.write_text(f'#!{sys.executable}\n' +
                'import subprocess, sys\n'
                'if sys.argv[1] in ("scripts/patches.py", "scripts/mods.py", "-c"):\n'
                '    sys.exit(subprocess.call([sys.executable, *sys.argv[1:]]))\n')
            python.chmod(0o755)
            probe = data / 'probe'
            probe.write_text(f'#!{sys.executable}\n' +
                'import json, os\n'
                'from pathlib import Path\n'
                'Path(os.environ["BB_CONFIG"]).with_name("environment").write_text(json.dumps('
                '{key: os.environ.get(key) for key in ' + repr(tuple(keys)) + '}))\n')
            probe.chmod(0o755)
            env = dict(os.environ, BB_PREBUILT='1', BB_PROBE=str(probe), PYTHON=str(python),
                       BB_DATA_DIR=str(data), BB_CONFIG=str(config), BB_GAME_DIR=str(data))
            for key in ('BB_FPS', 'BB_VBLANK_HZ', 'BB_FPS_LIMIT', 'BB_RENDER_RES', 'BB_LIVE_RES',
                        'BB_FSR1'):
                env.pop(key, None)
            env.update(extra)
            subprocess.run([shutil.which('bash'), 'run.sh'], cwd=ROOT, env=env,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True, timeout=30)
            return json.loads((data / 'environment').read_text())

    def test_default_vblank_per_preset(self):
        self.assertEqual(self.launch(BB_FPS='60'), {'BB_VBLANK_HZ': '60', 'BB_FPS_LIMIT': None})
        self.assertEqual(self.launch(BB_FPS='120'), {'BB_VBLANK_HZ': '240', 'BB_FPS_LIMIT': '120'})
        self.assertEqual(self.launch(BB_FPS='uncap'), {'BB_VBLANK_HZ': '240', 'BB_FPS_LIMIT': None})

    def test_forced_vblank_keeps_the_preset_rate(self):
        # The owner speed test forced 240 Hz: "60" ran at ~68 FPS.
        self.assertEqual(self.launch(BB_FPS='60', BB_VBLANK_HZ='240'),
                         {'BB_VBLANK_HZ': '240', 'BB_FPS_LIMIT': '60'})
        self.assertEqual(self.launch(BB_FPS='120', BB_VBLANK_HZ='480'),
                         {'BB_VBLANK_HZ': '480', 'BB_FPS_LIMIT': '120'})
        self.assertEqual(self.launch(BB_FPS='120', BB_VBLANK_HZ='120'),
                         {'BB_VBLANK_HZ': '120', 'BB_FPS_LIMIT': None})

    def test_explicit_frame_limit_wins(self):
        self.assertEqual(self.launch(BB_FPS='60', BB_VBLANK_HZ='240', BB_FPS_LIMIT='50'),
                         {'BB_VBLANK_HZ': '240', 'BB_FPS_LIMIT': '50'})

    def test_presenter_upscale_is_fsr1_for_ratchet_only(self):
        # Ratchet stays at 1920x1080: a larger screen gets FSR 1 instead of a bilinear stretch.
        self.assertEqual(self.launch(keys=('BB_FSR1',), BB_FPS='60'), {'BB_FSR1': '1'})
        self.assertEqual(self.launch(keys=('BB_FSR1',), BB_FPS='60', BB_FSR1='0'), {'BB_FSR1': '0'})
        # Bloodborne's default patches do not fit this eboot stand-in: none of them.
        self.assertEqual(self.launch(keys=('BB_FSR1',), title='CUSA03173', BB_FPS='30',
                                     BB_INTEL_TONEMAP_FIX='0', BB_SKIP_NETWORK_CHOICE='0'),
                         {'BB_FSR1': None})


RATCHET_KEYS = ('BB_FPS_LIMIT', 'BB_VBLANK_HZ', 'BB_HINTLESS_BASE', 'BB_STUB_IMPORTS',
                'BB_CREATE_YIELD_US', 'BB_CREATE_YIELD_ALL', 'BB_READBACK_LINEAR',
                'BB_READBACK_LINEAR_ASYNC', 'BB_GAME_AA', 'BB_PC_MODEL', 'BB_UPSCALER',
                'BB_RENDER_RES', 'BB_OUTPUT_RES')


class RatchetDefaultTests(unittest.TestCase):
    """Ratchet & Clank runs with no variables set: run.sh picks its settings by TITLE_ID."""

    def launch(self, title='CUSA01073', ini='', vendor=None, **extra):
        with tempfile.TemporaryDirectory() as directory:
            data = Path(directory)
            out = data / 'out'
            out.mkdir()
            (out / 'analysis.json').write_text(json.dumps({'sfo': {'TITLE_ID': title}}))
            elf = bytearray(120)
            struct.pack_into('<Q', elf, 0x20, 64)
            struct.pack_into('<HH', elf, 0x36, 56, 1)
            struct.pack_into('<IIQQQQQQ', elf, 64, 1, 0, 0, 0, 0, 0, 0x6000000, 0)
            (out / 'eboot.elf').write_bytes(elf)
            (data / 'eboot.bin').touch()
            config = data / 'bbport.ini'
            config.write_text(ini)
            caps = data / 'bb-gpu-capabilities'  # next to the probe, as run.sh looks for it
            caps.write_text('#!/bin/sh\n' + (f'[ "$1" = --device ] && printf "{vendor}\\tTest GPU\\n"\n'
                                              if vendor else 'exit 1\n'))
            caps.chmod(0o755)
            python = data / 'python'
            python.write_text(f'#!{sys.executable}\n' +
                'import subprocess, sys\n'
                'if sys.argv[1] in ("scripts/patches.py", "scripts/mods.py", "-c"):\n'
                '    sys.exit(subprocess.call([sys.executable, *sys.argv[1:]]))\n')
            python.chmod(0o755)
            probe = data / 'probe'
            probe.write_text(f'#!{sys.executable}\n' +
                'import json, os, sys\n'
                'from pathlib import Path\n'
                f'keys = {RATCHET_KEYS!r}\n'
                'Path(os.environ["BB_CONFIG"]).with_name("environment").write_text(json.dumps('
                '{key: os.environ.get(key) for key in keys}))\n')
            probe.chmod(0o755)
            env = dict(os.environ, BB_PREBUILT='1', BB_PROBE=str(probe), PYTHON=str(python),
                       BB_DATA_DIR=str(data), BB_CONFIG=str(config), BB_GAME_DIR=str(data))
            for key in RATCHET_KEYS + ('BB_FPS', 'BB_LIVE_RES', 'BB_AUTO_RENDER_RES'):
                env.pop(key, None)
            env.update(extra)
            subprocess.run([shutil.which('bash'), 'run.sh'], cwd=ROOT, env=env,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True, timeout=30)
            return json.loads((data / 'environment').read_text())

    def test_required_settings_without_variables(self):
        env = self.launch()
        self.assertEqual({key: env[key] for key in RATCHET_KEYS[2:10]}, {
            'BB_HINTLESS_BASE': '0x7000000000', 'BB_STUB_IMPORTS': '1',
            'BB_CREATE_YIELD_US': '2000', 'BB_CREATE_YIELD_ALL': '1', 'BB_READBACK_LINEAR': '1',
            'BB_READBACK_LINEAR_ASYNC': '1', 'BB_GAME_AA': '1', 'BB_PC_MODEL': '0'})

    def test_default_preset_is_60(self):
        env = self.launch()
        self.assertEqual((env['BB_VBLANK_HZ'], env['BB_FPS_LIMIT']), ('60', None))

    def test_presets(self):
        env = self.launch(BB_FPS='120')
        self.assertEqual((env['BB_VBLANK_HZ'], env['BB_FPS_LIMIT']), ('240', '120'))
        env = self.launch(BB_FPS='uncap')
        self.assertEqual((env['BB_VBLANK_HZ'], env['BB_FPS_LIMIT']), ('240', None))

    def test_explicit_values_win(self):
        env = self.launch(BB_READBACK_LINEAR_ASYNC='0', BB_GAME_AA='0', BB_PC_MODEL='1',
                          BB_HINTLESS_BASE='0x6000000000')
        self.assertEqual((env['BB_READBACK_LINEAR_ASYNC'], env['BB_GAME_AA'], env['BB_PC_MODEL'],
                          env['BB_HINTLESS_BASE']), ('0', '0', '1', '0x6000000000'))

    def test_upscaler_by_gpu(self):
        self.assertEqual(self.launch(vendor='0x10de')['BB_UPSCALER'], 'dlss')
        self.assertEqual(self.launch(vendor='0x1002')['BB_UPSCALER'], 'fsr3')
        self.assertEqual(self.launch(vendor='0x8086')['BB_UPSCALER'], 'fsr3')
        self.assertEqual(self.launch()['BB_UPSCALER'], 'fsr3')  # GPU unknown

    def test_upscaler_from_settings_or_variable_wins(self):
        self.assertIsNone(self.launch(vendor='0x10de', ini='upscaler=off\n')['BB_UPSCALER'])
        self.assertEqual(self.launch(vendor='0x10de', BB_UPSCALER='fsr3')['BB_UPSCALER'], 'fsr3')

    def test_output_resolution_is_not_patched(self):
        env = self.launch(ini='upscaler=fsr3\npreset=1\noutput_res=2560x1440\n')
        self.assertEqual((env['BB_RENDER_RES'], env['BB_OUTPUT_RES']), (None, None))

    def test_bloodborne_gets_none_of_them(self):
        env = self.launch(title='CUSA03173', vendor='0x10de', BB_FPS='60')
        for key in RATCHET_KEYS[2:9] + ('BB_UPSCALER',):
            self.assertIsNone(env[key], key)
        self.assertEqual(env['BB_PC_MODEL'], '0')  # its own choice by GPU: the 0.3 model off AMD
        self.assertEqual(env['BB_VBLANK_HZ'], '60')
        env = self.launch(title='CUSA03173', vendor='0x1002')
        self.assertEqual((env['BB_PC_MODEL'], env['BB_VBLANK_HZ']), ('1', '480'))


LBP3_KEYS = ('BB_IMAGE_BASE', 'BB_STUB_IMPORTS', 'BB_NP_SIGNED_IN', 'BB_LOCAL_UDP', 'BB_VBLANK_HZ')


class Lbp3DefaultTests(unittest.TestCase):
    """LittleBigPlanet 3 runs with no variables set: run.sh picks its settings by TITLE_ID."""

    def launch(self, title='CUSA00063', **extra):
        with tempfile.TemporaryDirectory() as directory:
            data = Path(directory)
            out = data / 'out'
            out.mkdir()
            (out / 'analysis.json').write_text(json.dumps({'sfo': {'TITLE_ID': title}}))
            elf = bytearray(120)
            struct.pack_into('<Q', elf, 0x20, 64)
            struct.pack_into('<HH', elf, 0x36, 56, 1)
            struct.pack_into('<IIQQQQQQ', elf, 64, 1, 0, 0, 0, 0, 0, 0x6000000, 0)
            (out / 'eboot.elf').write_bytes(elf)
            (data / 'eboot.bin').touch()
            config = data / 'bbport.ini'
            config.write_text('')
            caps = data / 'bb-gpu-capabilities'  # next to the probe, as run.sh looks for it
            caps.write_text('#!/bin/sh\nexit 1\n')
            caps.chmod(0o755)
            python = data / 'python'
            python.write_text(f'#!{sys.executable}\n' +
                'import subprocess, sys\n'
                'if sys.argv[1] in ("scripts/patches.py", "scripts/mods.py", "-c"):\n'
                '    sys.exit(subprocess.call([sys.executable, *sys.argv[1:]]))\n')
            python.chmod(0o755)
            probe = data / 'probe'
            probe.write_text(f'#!{sys.executable}\n' +
                'import json, os, sys\n'
                'from pathlib import Path\n'
                f'keys = {LBP3_KEYS!r}\n'
                'Path(os.environ["BB_CONFIG"]).with_name("environment").write_text(json.dumps('
                '{key: os.environ.get(key) for key in keys}))\n')
            probe.chmod(0o755)
            env = dict(os.environ, BB_PREBUILT='1', BB_PROBE=str(probe), PYTHON=str(python),
                       BB_DATA_DIR=str(data), BB_CONFIG=str(config), BB_GAME_DIR=str(data))
            for key in LBP3_KEYS + ('BB_FPS', 'BB_LIVE_RES', 'BB_AUTO_RENDER_RES'):
                env.pop(key, None)
            env.update(extra)
            subprocess.run([shutil.which('bash'), 'run.sh'], cwd=ROOT, env=env,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True, timeout=30)
            return json.loads((data / 'environment').read_text())

    def test_required_settings_without_variables(self):
        env = self.launch()
        self.assertEqual({key: env[key] for key in LBP3_KEYS[:4]}, {
            'BB_IMAGE_BASE': '0x80000000', 'BB_STUB_IMPORTS': '1', 'BB_NP_SIGNED_IN': '1',
            'BB_LOCAL_UDP': '1'})

    def test_explicit_values_win(self):
        env = self.launch(BB_NP_SIGNED_IN='0', BB_IMAGE_BASE='0x800000000')
        self.assertEqual((env['BB_NP_SIGNED_IN'], env['BB_IMAGE_BASE']), ('0', '0x800000000'))

    def test_default_rate_is_30_as_set_by_hand(self):
        self.assertEqual(self.launch()['BB_VBLANK_HZ'], self.launch(BB_FPS='30')['BB_VBLANK_HZ'])

    def test_bloodborne_gets_none_of_them(self):
        env = self.launch(title='CUSA03173')
        for key in LBP3_KEYS[:4]:
            self.assertIsNone(env[key], key)
