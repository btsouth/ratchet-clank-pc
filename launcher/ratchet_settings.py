# SPDX-License-Identifier: GPL-2.0-or-later
"""Settings of the Ratchet & Clank launcher (ratchet_launcher.py), without GTK so they can be tested.

Launcher choices live in ~/.config/ratchet-port-launcher/settings.json and reach run.sh as
environment variables; the upscaler and the FPS counter are bbport.ini keys the game reads. run.sh
adds everything else the game needs by its title ID.
"""

import json
import os
import sys
from pathlib import Path

PORT_DIR = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(PORT_DIR / "scripts"))
import game_check  # noqa: E402
from prepare import sfo  # noqa: E402

PACKAGED = bool(os.environ.get("BB_PREBUILT"))
DATA_DIR = Path(os.environ.get("BB_DATA_DIR", PORT_DIR))
CONFIG_DIR = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "ratchet-port-launcher"
TITLE = "Ratchet & Clank"

# (label, value, note): the first entry is the default.
FPS_MODES = [
    ("60 FPS", "60", None),
    ("120 FPS", "120", "Smooth on a 120 Hz or faster display"),
    ("Unlocked", "uncap", "As fast as the PC allows"),
    ("30 FPS (as on PS4)", "30", None),
]
# "" leaves the choice to run.sh: DLSS on NVIDIA, FSR 3.1 elsewhere. Both stand in for the game's
# own TAA at 1920x1080; "off" keeps the game's TAA.
UPSCALERS = [
    ("Auto", "", "DLSS on NVIDIA RTX, FSR 3.1 on other GPUs"),
    ("DLSS", "dlss", "NVIDIA RTX only; FSR 3.1 is used when it cannot start"),
    ("FSR 3.1", "fsr3", None),
    ("Game's own TAA", "off", None),
]
# The console's system language (BB_LANGUAGE, PS4 language codes). Only English is tested.
LANGUAGES = [
    ("English", "1", None), ("English (UK)", "18", "Untested"), ("French", "2", "Untested"),
    ("German", "4", "Untested"), ("Italian", "5", "Untested"), ("Spanish", "3", "Untested"),
    ("Dutch", "6", "Untested"), ("Portuguese", "7", "Untested"), ("Russian", "8", "Untested"),
    ("Polish", "16", "Untested"),
]

DEFAULTS = {
    "game_dir": "",
    "user_dir": "",
    "fullscreen": False,
    "display": "",
    "gamepad": "",
    "gamepad_name": "",
    "fps_mode": "60",
    "language": "1",
    "save_log": False,
    "extra_env": "",
}

NGX_PREFIX = "libnvidia-ngx-dlss.so."


def load_settings(config_dir=None):
    settings = dict(DEFAULTS)
    try:
        settings.update(json.loads((Path(config_dir or CONFIG_DIR) / "settings.json").read_text(encoding="utf-8")))
    except (OSError, ValueError):
        pass
    return settings


def save_settings(settings, config_dir=None):
    directory = Path(config_dir or CONFIG_DIR)
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "settings.json").write_text(json.dumps(settings, indent=2), encoding="utf-8")


def ini_path():
    return Path(os.environ.get("BB_CONFIG", DATA_DIR / "bbport.ini"))


def read_ini(path=None):
    """(values, lines) of bbport.ini; the game's in-game menu writes the same file."""
    values, lines = {}, []
    try:
        lines = Path(path or ini_path()).read_text(encoding="utf-8").splitlines()
    except OSError:
        pass
    for line in lines:
        if "=" in line and not line.lstrip().startswith("#"):
            key, value = line.split("=", 1)
            values[key.strip()] = value.strip()
    return values, lines


# bbport.ini keys the launcher sets when the file lacks them (a choice made in the launcher or the
# in-game menu stays). The game's motion blur is off: with it the scene flickers while the camera
# turns, shadows coming and going between frames.
INI_DEFAULTS = {"effect_motion_blur": "0"}


def ensure_ini_defaults(path=None):
    values, _ = read_ini(path)
    missing = {key: value for key, value in INI_DEFAULTS.items() if key not in values}
    if missing:
        write_ini(missing, path)


def write_ini(changes, path=None):
    """Sets the keys in changes (None removes one) and keeps the rest of the file as it is."""
    path = Path(path or ini_path())
    _, lines = read_ini(path)
    written, out = set(), []
    for line in lines:
        if "=" in line and not line.lstrip().startswith("#"):
            key = line.split("=", 1)[0].strip()
            if key in changes:
                if changes[key] is not None and key not in written:
                    out.append(f"{key}={changes[key]}")
                written.add(key)
                continue
        out.append(line)
    if not lines:
        out.append("# Ratchet & Clank port settings")
    out += [f"{key}={value}" for key, value in changes.items() if key not in written and value is not None]
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(out) + "\n", encoding="utf-8")


def user_dir(settings):
    return Path(settings["user_dir"]).expanduser() if settings["user_dir"] else DATA_DIR / "user"


def binaries_dir():
    return PORT_DIR / ("bin" if PACKAGED else "out")


def dlss_status(settings):
    """A line about DLSS: whether the bridge and NVIDIA's library are there."""
    def found(directory):
        try:
            return sorted(p.name for p in Path(directory).iterdir() if p.name.startswith(NGX_PREFIX))
        except OSError:
            return []
    if not (binaries_dir() / "libbbport_dlss.so").is_file():
        return "This build has no DLSS bridge: FSR 3.1 is used"
    libraries = found(user_dir(settings) / "dlss") or found(binaries_dir())
    if not libraries:
        return "NVIDIA's DLSS library is missing: FSR 3.1 is used"
    return f"DLSS {libraries[-1][len(NGX_PREFIX):]} found"


def game_status(path):
    """(level, text) for a game folder: level 'ok', 'warning' (starts anyway) or 'error'."""
    path = Path(path).expanduser()
    if not (path / "eboot.bin").is_file():
        return "error", "No eboot.bin here. Pick the CUSA01073 folder of your own dump."
    try:
        info = sfo((path / "sce_sys/param.sfo").read_bytes())
    except (OSError, ValueError, IndexError):
        info = {}
    title, version = info.get("TITLE_ID", "?"), info.get("APP_VER", "?")
    if title != game_check.RATCHET_TITLE and title not in game_check.RATCHET_UNTESTED_TITLES:
        return "error", (f"This is not Ratchet & Clank (found {title}, sce_sys/param.sfo). The port "
                         "runs the European release CUSA01073, version 01.00.")
    found = game_check.problem(path)
    if found is None:
        return "ok", f"Ratchet & Clank {title}, version {version}"
    return ("warning" if found[0] in game_check.WARNINGS else "error"), game_check.explain(*found)


def game_environment(settings, base=None):
    """Environment for run.sh from the launcher settings."""
    s = settings
    env = dict(os.environ if base is None else base)
    env["BB_GAME_DIR"] = str(Path(s["game_dir"]).expanduser())
    if s["user_dir"]:
        env["BB_USER_DIR"] = str(Path(s["user_dir"]).expanduser())
    env["BB_MODS_ENABLED"] = "0"  # loose-file mods are Bloodborne's
    env["BB_LANGUAGE"] = s["language"]
    env["BB_MENU_LANGUAGE"] = "en"
    env["BB_FULLSCREEN"] = "1" if s["fullscreen"] else "0"
    env["BB_FPS"] = s["fps_mode"]
    if s["gamepad"]:
        env["BB_GAMEPAD"] = s["gamepad"]
    if s["display"]:
        env["BB_DISPLAY"] = s["display"]
    if s["save_log"]:
        env["BB_SAVE_LOG"] = "1"
    for item in s["extra_env"].split():
        if "=" in item:
            key, value = item.split("=", 1)
            env[key] = value
    return env
