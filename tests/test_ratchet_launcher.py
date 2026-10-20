"""Ratchet & Clank launcher settings (launcher/ratchet_settings.py): environment, bbport.ini, game check."""
from paths import ROOT
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(ROOT / 'launcher'))
import ratchet_settings as rs  # noqa: E402
import game_check  # noqa: E402
from test_game_check import param_sfo  # noqa: E402
from test_prepare import fixture  # noqa: E402


class EnvironmentTests(unittest.TestCase):
    def test_defaults(self):
        env = rs.game_environment(dict(rs.DEFAULTS, game_dir='/games/CUSA01073'), base={})
        self.assertEqual(env, {'BB_GAME_DIR': '/games/CUSA01073', 'BB_MODS_ENABLED': '0',
                               'BB_LANGUAGE': '1', 'BB_MENU_LANGUAGE': 'en', 'BB_FULLSCREEN': '0',
                               'BB_FPS': '60'})

    def test_choices_and_extra_variables(self):
        settings = dict(rs.DEFAULTS, game_dir='/g', fullscreen=True, fps_mode='120', gamepad='0300abcd',
                        display='DP-1', save_log=True, extra_env='BB_READBACK_LINEAR_ASYNC=0 junk')
        env = rs.game_environment(settings, base={'HOME': '/home/x'})
        self.assertEqual((env['BB_FULLSCREEN'], env['BB_FPS'], env['BB_GAMEPAD'], env['BB_DISPLAY'],
                          env['BB_SAVE_LOG'], env['BB_READBACK_LINEAR_ASYNC'], env['HOME']),
                         ('1', '120', '0300abcd', 'DP-1', '1', '0', '/home/x'))
        self.assertNotIn('junk', env)

    def test_presets_are_the_ones_run_sh_knows(self):
        import patches
        self.assertEqual({value for _, value, _ in rs.FPS_MODES}, set(patches.RATCHET_FPS) - {'90'})
        self.assertEqual(rs.FPS_MODES[0][1], '60')


class IniTests(unittest.TestCase):
    def test_auto_removes_the_key_and_keeps_the_rest(self):
        with tempfile.TemporaryDirectory() as directory:
            ini = Path(directory) / 'bbport.ini'
            ini.write_text('# mine\nupscaler=fsr3\nsharpness=0.50\nkey.cross=Space\n')
            rs.write_ini({'upscaler': None, 'show_fps': '1'}, ini)
            self.assertEqual(ini.read_text(), '# mine\nsharpness=0.50\nkey.cross=Space\nshow_fps=1\n')
            rs.write_ini({'upscaler': 'dlss'}, ini)
            self.assertEqual(rs.read_ini(ini)[0]['upscaler'], 'dlss')

    def test_new_file(self):
        with tempfile.TemporaryDirectory() as directory:
            ini = Path(directory) / 'sub' / 'bbport.ini'
            rs.write_ini({'upscaler': None, 'show_fps': '0'}, ini)
            self.assertEqual(rs.read_ini(ini)[0], {'show_fps': '0'})


    def test_defaults_fill_only_missing_keys(self):
        with tempfile.TemporaryDirectory() as directory:
            ini = Path(directory) / 'bbport.ini'
            rs.ensure_ini_defaults(ini)
            self.assertEqual(rs.read_ini(ini)[0], {'effect_motion_blur': '0'})
            ini.write_text('upscaler=dlss\neffect_motion_blur=1\n')
            rs.ensure_ini_defaults(ini)  # chosen in the launcher or the in-game menu: kept
            self.assertEqual(ini.read_text(), 'upscaler=dlss\neffect_motion_blur=1\n')


class GameStatusTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.path = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def game(self, title, version):
        (self.path / 'sce_sys').mkdir(exist_ok=True)
        (self.path / 'sce_sys/param.sfo').write_bytes(param_sfo({'APP_VER': version, 'TITLE_ID': title}))
        (self.path / 'eboot.bin').write_bytes(fixture())
        return self.path

    def test_empty_folder(self):
        self.assertEqual(rs.game_status(self.path)[0], 'error')

    def test_supported(self):
        game = self.game('CUSA01073', '01.00')
        with mock.patch.object(game_check, 'RATCHET_IMAGE', game_check.image_sha256(game)):
            self.assertEqual(rs.game_status(game), ('ok', 'Ratchet & Clank CUSA01073, version 01.00'))

    def test_update_merged(self):
        self.assertEqual(rs.game_status(self.game('CUSA01073', '01.01'))[0], 'error')

    def test_us_release_warns(self):
        level, text = rs.game_status(self.game('CUSA01047', '01.00'))
        self.assertEqual(level, 'warning')
        self.assertIn('not been tested', text)

    def test_bloodborne_is_not_ratchet(self):
        game = self.game('CUSA03173', '01.09')
        with mock.patch.object(game_check, 'SUPPORTED_IMAGE', game_check.image_sha256(game)):
            level, text = rs.game_status(game)
        self.assertEqual(level, 'error')
        self.assertIn('not Ratchet', text)


if __name__ == '__main__':
    unittest.main()
