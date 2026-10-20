# Ratchet & Clank (PS4) on Linux

A native Linux port of *Ratchet & Clank* (2016) for PlayStation 4. It runs the game's original
PS4 executable directly on a Linux PC, the way Wine plus DXVK runs a Windows game:

- the game's x86-64 code runs on your CPU as it is, with no emulation;
- a small runtime stands in for the PS4 system libraries the game calls;
- the game's graphics are translated to Vulkan by a renderer derived from
  [shadPS4](https://github.com/shadps4-emu/shadPS4).

It is built on [bbport](https://github.com/deadinside28/bloodborne_pc), the native Bloodborne port
by deadinside28, and keeps its loader, runtime and renderer. Only this one game is supported.

**No game files are included, and none are linked here.** You need your own copy of the game,
dumped from your own PS4. This project is not affiliated with Sony Interactive Entertainment or
Insomniac Games.

**Status: experimental, release 0.1.** The game boots, plays its cutscenes, saves and loads, and
plays with sound and a controller. Every planet loads and plays at the 60 FPS preset, but no one
has finished the game on it yet. It has been tested on one desktop (Intel Core i7-14700F, NVIDIA
RTX 4070 SUPER) and, for correctness only, on an Intel iGPU.

## Requirements

- Linux on an x86-64 PC and a GPU with Vulkan 1.3. NVIDIA RTX is the tested setup. AMD and Intel
  GPUs should work but have had little or no testing.
- A dump of **Ratchet & Clank, European release CUSA01073, version 01.00**, with no update merged
  into it. The game folder holds `eboot.bin`, `sce_module/` and `sce_sys/`.
  - The US release (CUSA01047) has not been tried. The launcher lets you start it with a warning.
  - Updates (1.01 and later) are not supported: the port's patches use the 1.00 addresses.
- About 30 GB of disk space for the game, plus a few GB for the shader cache.

## Getting the game files

You need a jailbroken PS4 and your own copy of the game, on disc or bought digitally.

1. Dump the game with a PS4 dumper of your choice, or back up its package file.
2. If you have a `.pkg`, extract it with a PKG extractor (the shadPS4 docs list a few). Use a
   recent tool: some old extractors leave parts of compressed files stale.
3. You should end up with a folder named `CUSA01073` that contains `eboot.bin`. Point the launcher
   at that folder.

Do not ask for game files in this project's issues. They will be closed.

## Install and run

### AppImage (recommended)

1. Download `Ratchet-Clank-port-x86_64.AppImage` from the
   [releases page](https://github.com/btsouth/ratchet-clank-pc/releases). The `.sha256` file next
   to it lets you check the download with `sha256sum -c`.
2. Make it executable and start it:

   ```bash
   chmod +x Ratchet-Clank-port-x86_64.AppImage
   ./Ratchet-Clank-port-x86_64.AppImage
   ```

3. In the launcher, choose your game folder. A green tick means it found CUSA01073 1.00.
4. Press **Play**. The first start prepares the game image and compiles shaders, so expect a slow
   first minute and some stutter the first time you visit each area. Both are cached afterwards.

`./Ratchet-Clank-port-x86_64.AppImage --play` starts the game with the saved settings and no
launcher window (handy for Steam as a non-Steam game).

Saves, caches and settings live in `~/.local/share/ratchet-port`. Launcher settings live in
`~/.config/ratchet-port-launcher`.

The AppImage needs unprivileged user namespaces, which most desktop distributions have on.

### From source

You need GCC, CMake, Ninja, Python 3, glslang, SDL3, the Vulkan headers and the libraries listed
in `shell.nix`. With [Nix](https://nixos.org) they all come from `shell.nix`.

```bash
git clone --recursive <this repository> ratchet-port && cd ratchet-port
bash build.sh
bash launcher/ratchet-launcher.sh      # needs PyGObject, GTK 4 and libadwaita
```

Or without the launcher:

```bash
BB_GAME_DIR=/path/to/CUSA01073 bash run.sh
```

`run.sh` recognises the game by its title ID and applies everything it needs. You don't have to
set any variables.

## Settings

The launcher covers the common ones:

| Setting | Choices |
|---|---|
| Frame rate | **60 FPS** (default), 120 FPS, Unlocked, 30 FPS (as on PS4) |
| Anti-aliasing | **Auto**: DLSS on NVIDIA RTX, FSR 3.1 on other GPUs. Or DLSS, FSR 3.1, or the game's own TAA |
| Show FPS counter | Off by default |
| Fullscreen and monitor | Windowed on the primary monitor by default |
| Controller | The first one connected, or a specific one |
| Language | The console language the game sees. Only English is tested |

DLSS and FSR 3.1 replace the game's own temporal anti-aliasing at the game's native 1920x1080, then
the image is scaled to your window. If DLSS can't start (no NVIDIA RTX GPU or no DLSS library), FSR
3.1 is used.

The game's 120 FPS and Unlocked modes are patched in by the port. The game's own clock is patched
too, so it does not run faster at higher frame rates. On the tested RTX 4070 SUPER the 120 FPS mode
runs at about 80 FPS in busy scenes, because the CPU side is the limit.

Settings are stored in `bbport.ini` in the data folder. The in-game overlay (Insert key, or L3+R3
on a controller) can change the upscaler, sharpening and motion blur while you play.

### Controls

Any controller SDL supports works: DualShock 4, DualSense, Xbox and others. The keyboard works
too. Its default layout is bbport's and can be changed with `key.<button>=` lines in `bbport.ini`
(for example `key.cross=Space`).

### Environment variables

You don't need these for normal play. They are for testing and bug reports.

| Variable | Default for this game | What it does |
|---|---|---|
| `BB_GAME_DIR` | | The game folder |
| `BB_FPS` | `60` | `30`, `60`, `120` or `uncap` |
| `BB_UPSCALER` | `dlss` on NVIDIA, else `fsr3` | `dlss`, `fsr3` or `off` (the game's TAA) |
| `BB_GAME_AA` | `1` | `0` turns off the DLSS/FSR stand-in for the game's TAA |
| `BB_READBACK_LINEAR_ASYNC` | `0` | `1` copies GPU readbacks without waiting (faster, but planet loads can crash) |
| `BB_VBLANK_HZ` | `60`, or `240` for 120 and Unlocked | The port's vblank rate |
| `BB_FPS_LIMIT` | set by the preset | Frame limit in FPS, `0` for none |
| `BB_SAVE_LOG` | | `1` writes the log to `logs/` in the data folder |
| `BB_SKIP_GAME_CHECK` | | `1` starts a game folder the check rejects |

The port also sets `BB_HINTLESS_BASE`, `BB_STUB_IMPORTS`, `BB_CREATE_YIELD_US`,
`BB_CREATE_YIELD_ALL`, `BB_READBACK_LINEAR` and `BB_PC_MODEL=0` for this game. The game does not
start without them. Each one can still be overridden. `run.sh` explains each.

## Known issues

- Only CUSA01073 version 01.00 is tested. The US release and updated versions are not.
- No one has played the game from start to finish on the port yet.
- The first visit to each area stutters while shaders compile. Later visits use the cache.
- 120 FPS is not steady yet: busy scenes run at about 80 FPS on the tested PC. 60 FPS is the
  recommended preset. Unlocked is less tested still.
- Don't set `BB_VBLANK_HZ=480`. With very high frame rates at 480 Hz the game has frozen. The
  port uses 240 Hz instead.
- On CPUs without AMD's SSE4a instructions (all Intel CPUs) a few game instructions are emulated.
  This is automatic and costs almost nothing.
- AMD GPUs run with the same memory model as NVIDIA (`BB_PC_MODEL=0`). bbport's newer model is not
  tested with this game.

## Reporting a problem

Turn on **Save the log to a file** in the launcher, reproduce the problem, and attach the log from
`~/.local/share/ratchet-port/logs` with your GPU, driver version and distribution.

## Tools

`scripts/ratchet_save.py` shows a save file and can write a copy with every planet unlocked and the
checkpoint moved. Run it with `--help`. Keep a backup of your saves first.

## Credits and licenses

This port is licensed under the **GNU GPL v2 or later** ([LICENSE](LICENSE)).

- [bbport](https://github.com/deadinside28/bloodborne_pc) by deadinside28 and its contributors:
  the loader, runtime, renderer, launcher and packaging this port is built on. bbport's own
  documentation is in [docs/bloodborne](docs/bloodborne/README.md).
- [shadPS4](https://github.com/shadps4-emu/shadPS4): the video core and shader recompiler the
  renderer is derived from (GPL-2.0-or-later), with sirit and half.
- AMD FidelityFX Super Resolution 3.1 through [FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan)
  by FireBurn (MIT) and the AMD FidelityFX SDK (MIT).
- NVIDIA DLSS. The DLSS bridge (`gpu/dlss_bridge`, MIT) comes from
  [Supermedo's port](https://github.com/Supermedo/bloodborne_pc) and
  [IFreemz/shadPS4-Bloodborne-DLSS-FSR](https://github.com/IFreemz/shadPS4-Bloodborne-DLSS-FSR).
  The AppImage includes NVIDIA's DLSS runtime library under the NVIDIA RTX SDK license, which is
  included next to it as `NVIDIA-DLSS-LICENSE.txt`. The DLSS SDK is not part of this repository.
- [LibAtrac9](https://github.com/Thealexbarney/LibAtrac9) (MIT) and
  [Dear ImGui](https://github.com/ocornut/imgui) (MIT).

Ratchet & Clank is a trademark of Sony Interactive Entertainment. NVIDIA, RTX and DLSS are
trademarks of NVIDIA Corporation. AMD and FidelityFX are trademarks of Advanced Micro Devices.
