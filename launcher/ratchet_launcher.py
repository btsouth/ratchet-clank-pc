#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Ratchet & Clank launcher for the native port (GTK4 / libadwaita).

Picks the game folder and checks it, sets the frame rate, upscaler, display and controller, then
starts run.sh and shows its output. --play starts the game with the saved settings, no window.
"""

import os
import signal
import sys
import time
from pathlib import Path

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Adw", "1")
from gi.repository import Adw, Gio, GLib, Gtk  # noqa: E402

import ratchet_settings as rs  # noqa: E402
from bbport_i18n import set_language  # noqa: E402

set_language("en")  # the shared helpers below label their rows in English
from bbport_launcher import connected_displays, connected_gamepads, flat_button, open_folder  # noqa: E402

MAX_LOG_LINES = 5000


def combo_row(title, choices, current):
    """A combo row over (label, value, note) choices; the note of the selected one is its subtitle."""
    row = Adw.ComboRow(title=title, model=Gtk.StringList.new([label for label, _, _ in choices]))
    row.values = [value for _, value, _ in choices]
    notes = [note for _, _, note in choices]
    row.connect("notify::selected", lambda *_: row.set_subtitle(notes[row.get_selected()] or ""))
    row.set_selected(row.values.index(current) if current in row.values else 0)
    row.set_subtitle(notes[row.get_selected()] or "")
    return row


class LauncherWindow(Adw.ApplicationWindow):
    def __init__(self, app):
        super().__init__(application=app, title=rs.TITLE)
        self.set_default_size(640, 720)
        self.settings = rs.load_settings()
        self.process = None
        self.stream = None
        self.build()
        self.connect("close-request", self.on_close)

    def build(self):
        toolbar = Adw.ToolbarView()
        header = Adw.HeaderBar()
        self.stack = Adw.ViewStack()
        header.set_title_widget(Adw.ViewSwitcher(stack=self.stack, policy=Adw.ViewSwitcherPolicy.WIDE))
        self.launch_button = Gtk.Button()
        self.launch_button.connect("clicked", self.on_launch)
        header.pack_end(self.launch_button)
        toolbar.add_top_bar(header)
        self.toasts = Adw.ToastOverlay(child=self.stack)
        toolbar.set_content(self.toasts)
        self.set_content(toolbar)
        self.stack.add_titled_with_icon(self.build_settings_page(), "settings", "Settings",
                                        "preferences-system-symbolic")
        self.stack.add_titled_with_icon(self.build_log_page(), "log", "Log",
                                        "utilities-terminal-symbolic")
        self.update_game_status()
        self.update_upscaler_status()

    # --- settings ------------------------------------------------------------------------

    def build_settings_page(self):
        s = self.settings
        ini, _ = rs.read_ini()
        page = Adw.PreferencesPage()

        game = Adw.PreferencesGroup(title="Game", description=(
            "Your own dump of the European release, CUSA01073 version 01.00"))
        self.game_row = Adw.ActionRow(title="Game folder")
        self.game_status = Gtk.Image()
        self.game_row.add_suffix(self.game_status)
        self.game_row.add_suffix(flat_button("folder-open-symbolic", "Choose the folder with eboot.bin",
                                             self.on_choose_game))
        game.add(self.game_row)
        self.user_row = Adw.ActionRow(title="Saves and caches", subtitle=str(rs.user_dir(s)))
        self.user_row.add_suffix(flat_button("system-file-manager-symbolic", "Open in the file manager",
                                             lambda _b: open_folder(self, rs.user_dir(self.settings))))
        game.add(self.user_row)
        self.language_row = combo_row("Language", rs.LANGUAGES, s["language"])
        game.add(self.language_row)
        page.add(game)

        video = Adw.PreferencesGroup(title="Video")
        self.fps_row = combo_row("Frame rate", rs.FPS_MODES, s["fps_mode"])
        video.add(self.fps_row)
        self.upscaler_row = combo_row("Anti-aliasing", rs.UPSCALERS, ini.get("upscaler", ""))
        self.upscaler_row.connect("notify::selected", lambda *_: self.update_upscaler_status())
        video.add(self.upscaler_row)
        self.show_fps_row = Adw.SwitchRow(title="Show FPS counter", active=ini.get("show_fps") == "1")
        video.add(self.show_fps_row)
        self.motion_blur_row = Adw.SwitchRow(
            title="Motion blur", subtitle="Makes shadows flicker while the camera turns",
            active=ini.get("effect_motion_blur", rs.INI_DEFAULTS["effect_motion_blur"]) == "1")
        video.add(self.motion_blur_row)
        page.add(video)

        display = Adw.PreferencesGroup(title="Display")
        self.fullscreen_row = Adw.SwitchRow(title="Fullscreen", active=s["fullscreen"])
        display.add(self.fullscreen_row)
        self.display_row = Adw.ComboRow(title="Monitor")
        display.add(self.display_row)
        page.add(display)

        controls = Adw.PreferencesGroup(title="Controls", description=(
            "Any controller SDL knows works (DualShock 4, DualSense, Xbox). The keyboard works too."))
        self.gamepad_row = Adw.ComboRow(title="Controller")
        refresh = flat_button("view-refresh-symbolic", "Look for controllers again",
                              lambda _b: self.fill_gamepads())
        self.gamepad_row.add_suffix(refresh)
        controls.add(self.gamepad_row)
        page.add(controls)

        advanced = Adw.PreferencesGroup(title="Troubleshooting")
        self.save_log_row = Adw.SwitchRow(title="Save the log to a file", subtitle=str(rs.DATA_DIR / "logs"),
                                          active=s["save_log"])
        advanced.add(self.save_log_row)
        self.extra_row = Adw.EntryRow(title="Extra environment variables (NAME=value ...)")
        self.extra_row.set_text(s["extra_env"])
        advanced.add(self.extra_row)
        page.add(advanced)

        self.fill_displays()
        self.fill_gamepads()
        return page

    def fill_gamepads(self):
        current = self.settings.get("gamepad", "")
        choices = [("Auto (first connected)", "", "")]
        choices += [(name, guid, name) for guid, name in connected_gamepads()]
        if current and current not in [guid for _, guid, _ in choices]:
            name = self.settings.get("gamepad_name") or current
            choices.append((f"{name} (not connected)", current, name))
        self.gamepad_row.set_model(Gtk.StringList.new([label for label, _, _ in choices]))
        self.gamepad_row.values = [guid for _, guid, _ in choices]
        self.gamepad_row.names = [name for _, _, name in choices]
        values = self.gamepad_row.values
        self.gamepad_row.set_selected(values.index(current) if current in values else 0)

    def fill_displays(self):
        current = self.settings.get("display", "")
        choices = [("Primary", "")] + [(label, value) for value, label in connected_displays()]
        if current and current not in [value for _, value in choices]:
            choices.append((f"{current} (not connected)", current))
        self.display_row.set_model(Gtk.StringList.new([label for label, _ in choices]))
        self.display_row.values = [value for _, value in choices]
        values = self.display_row.values
        self.display_row.set_selected(values.index(current) if current in values else 0)

    def update_upscaler_status(self):
        value = self.upscaler_row.values[self.upscaler_row.get_selected()]
        note = dict((v, n) for _, v, n in rs.UPSCALERS)[value]
        lines = [note] if note else []
        if value in ("", "dlss"):
            lines.append(rs.dlss_status(self.settings))
        self.upscaler_row.set_subtitle("\n".join(lines))

    def update_game_status(self):
        if not self.settings["game_dir"]:
            level, text = "error", "Not chosen yet"
        else:
            level, text = rs.game_status(self.settings["game_dir"])
            text = f"{Path(self.settings['game_dir']).expanduser()}\n{text}"
        self.game_row.set_subtitle(GLib.markup_escape_text(text))
        self.game_status.set_from_icon_name({"ok": "object-select-symbolic",
                                             "warning": "dialog-warning-symbolic"}.get(level, "dialog-error-symbolic"))
        self.can_start = level != "error"
        self.update_launch_button()

    def on_choose_game(self, _button):
        dialog = Gtk.FileDialog(title="Game folder (with eboot.bin)")
        current = Path(self.settings["game_dir"]).expanduser()
        if self.settings["game_dir"] and current.is_dir():
            dialog.set_initial_folder(Gio.File.new_for_path(str(current)))

        def finish(dialog, result):
            try:
                folder = dialog.select_folder_finish(result)
            except GLib.Error:
                return
            if folder and folder.get_path():
                self.settings["game_dir"] = folder.get_path()
                self.store()
                self.update_game_status()
        dialog.select_folder(self, None, finish)

    def store(self):
        s = self.settings
        s["language"] = self.language_row.values[self.language_row.get_selected()]
        s["fps_mode"] = self.fps_row.values[self.fps_row.get_selected()]
        s["fullscreen"] = self.fullscreen_row.get_active()
        s["display"] = self.display_row.values[self.display_row.get_selected()]
        s["gamepad"] = self.gamepad_row.values[self.gamepad_row.get_selected()]
        s["gamepad_name"] = self.gamepad_row.names[self.gamepad_row.get_selected()]
        s["save_log"] = self.save_log_row.get_active()
        s["extra_env"] = self.extra_row.get_text().strip()
        rs.save_settings(s)
        upscaler = self.upscaler_row.values[self.upscaler_row.get_selected()]
        rs.write_ini({"upscaler": upscaler or None,
                      "show_fps": "1" if self.show_fps_row.get_active() else "0",
                      "effect_motion_blur": "1" if self.motion_blur_row.get_active() else "0"})

    # --- log -----------------------------------------------------------------------------

    def build_log_page(self):
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        self.log_view = Gtk.TextView(editable=False, monospace=True, cursor_visible=False,
                                     wrap_mode=Gtk.WrapMode.WORD_CHAR, top_margin=8,
                                     left_margin=8, right_margin=8)
        self.log_scroller = Gtk.ScrolledWindow(vexpand=True, child=self.log_view)
        box.append(self.log_scroller)
        bar = Gtk.ActionBar()
        export = Gtk.Button(label="Save log…")
        export.connect("clicked", self.on_export_log)
        bar.pack_end(export)
        copy = Gtk.Button(label="Copy")
        copy.connect("clicked", lambda _b: (self.get_clipboard().set(self.log_text()),
                                            self.toasts.add_toast(Adw.Toast(title="Log copied"))))
        bar.pack_end(copy)
        box.append(bar)
        return box

    def log_text(self):
        buffer = self.log_view.get_buffer()
        return buffer.get_text(*buffer.get_bounds(), False)

    def on_export_log(self, _button):
        text = self.log_text()
        dialog = Gtk.FileDialog(title="Save log")
        dialog.set_initial_name(f"ratchet-port-{time.strftime('%Y%m%d_%H%M%S')}.log")

        def finish(dialog, result):
            try:
                chosen = dialog.save_finish(result)
            except GLib.Error:
                return
            if chosen and chosen.get_path():
                try:
                    Path(chosen.get_path()).write_text(text, encoding="utf-8")
                except OSError as error:
                    self.toasts.add_toast(Adw.Toast(title=f"Could not save the log: {error.strerror or error}"))
        dialog.save(self, None, finish)

    def append_log(self, text):
        buffer = self.log_view.get_buffer()
        buffer.insert(buffer.get_end_iter(), text)
        extra = buffer.get_line_count() - MAX_LOG_LINES
        if extra > 0:
            buffer.delete(buffer.get_start_iter(), buffer.get_iter_at_line(extra)[1])
        adj = self.log_scroller.get_vadjustment()
        GLib.idle_add(lambda: adj.set_value(adj.get_upper()) and False)

    # --- the game ------------------------------------------------------------------------

    def update_launch_button(self):
        running = self.process is not None
        self.launch_button.set_label("Stop" if running else "Play")
        self.launch_button.remove_css_class("suggested-action" if running else "destructive-action")
        self.launch_button.add_css_class("destructive-action" if running else "suggested-action")
        self.launch_button.set_sensitive(running or getattr(self, "can_start", False))

    def on_launch(self, _button):
        if self.process:
            self.stop_game()
            return
        self.store()
        self.log_view.get_buffer().set_text("")
        launcher = Gio.SubprocessLauncher.new(Gio.SubprocessFlags.STDOUT_PIPE | Gio.SubprocessFlags.STDERR_MERGE)
        launcher.set_environ([f"{k}={v}" for k, v in rs.game_environment(self.settings).items()])
        launcher.set_cwd(str(rs.PORT_DIR))
        try:
            # setsid: the game and its helpers form one process group, stopped together.
            self.process = launcher.spawnv(["setsid", "bash", str(rs.PORT_DIR / "run.sh")])
        except GLib.Error as error:
            self.toasts.add_toast(Adw.Toast(title=f"Could not start: {error.message}"))
            return
        self.stream = Gio.DataInputStream.new(self.process.get_stdout_pipe())
        self.read_line()
        self.process.wait_async(None, self.on_exit)
        self.update_launch_button()
        self.stack.set_visible_child_name("log")

    def read_line(self):
        self.stream.read_line_async(GLib.PRIORITY_DEFAULT, None, self.on_line)

    def on_line(self, stream, result):
        try:
            line, _length = stream.read_line_finish_utf8(result)
        except GLib.Error:
            return
        if line is not None:
            self.append_log(line + "\n")
            self.read_line()

    def stop_game(self):
        if not self.process:
            return
        pid = int(self.process.get_identifier())
        try:
            os.killpg(pid, signal.SIGTERM)
        except OSError:
            self.process.force_exit()
        GLib.timeout_add_seconds(3, self.kill_if_running, pid)

    def kill_if_running(self, pid):
        if self.process:
            try:
                os.killpg(pid, signal.SIGKILL)
            except OSError:
                pass
        return False

    def on_exit(self, process, result):
        try:
            process.wait_finish(result)
        except GLib.Error:
            pass
        status = process.get_exit_status() if process.get_if_exited() else -1
        self.process = None
        self.update_launch_button()
        self.append_log(f"\n(the game exited with code {status})\n")

    def on_close(self, _window):
        self.store()
        if self.process:
            self.stop_game()
        return False


class LauncherApp(Adw.Application):
    def __init__(self):
        super().__init__(application_id="io.github.ratchet_port.Launcher", flags=Gio.ApplicationFlags.DEFAULT_FLAGS)

    def do_activate(self):
        (self.get_active_window() or LauncherWindow(self)).present()


def play():
    """--play: the game with the saved settings, no window."""
    settings = rs.load_settings()
    level, text = rs.game_status(settings["game_dir"]) if settings["game_dir"] else ("error", "")
    if level == "error":
        print(f"ratchet-port: choose the game folder in the launcher first. {text}", file=sys.stderr)
        return 1
    rs.ensure_ini_defaults()
    os.chdir(rs.PORT_DIR)
    os.execvpe("bash", ["bash", str(rs.PORT_DIR / "run.sh")], rs.game_environment(settings))


if __name__ == "__main__":
    if "--play" in sys.argv[1:]:
        sys.exit(play())
    sys.exit(LauncherApp().run([arg for arg in sys.argv if arg != "--play"]))
