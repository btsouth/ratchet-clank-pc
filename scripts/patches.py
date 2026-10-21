"""Compile selected shadPS4/GoldHEN XML patches into out/patches.bin for the loader.

Patch addresses are PS4 virtual addresses (eboot base 0x400000); the loader's
image places eboot vaddr 0 at image offset 0. Only literal writes are supported
(bytes, bytes16/32/64, float32/64, utf8, utf16); pattern ("mask") patches are rejected.
"""
import argparse
import json
import os
import struct
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

EBOOT_BASE=0x400000
# BB_FPS presets: patch names from patches/Bloodborne.xml (app version 01.09).
# Above 60 FPS the sprint fix always goes with the frame rate patch: without it sprinting drops to
# half speed (the game's wall detector measured distance per frame, a 30 FPS rule).
FPS_PRESETS={'30':[],'60':['60 FPS++'],'90':['90 FPS++','Sprint Fix (High FPS)'],
             'uncap':['Uncap FPS++','Sprint Fix (High FPS)']}
# Upscaler presets (bbport.ini "preset", the in-game menu): output / render size ratio. The game
# then renders at 1920x1080 / ratio and the port's temporal upscaler restores the output size.
OUTPUT_SIZE=(1920,1080)
PRESET_SCALES=[1.0,1.5,1.7,2.0,3.0]
# The community patch changes two independent consumers: the game render/window setup
# and the UI movie viewport. Keep the latter at native size so glyph rasterisation and
# vector tessellation do not inherit the scene's FSR resolution.
RESOLUTION_TEMPLATE='Resolution Patch 1280x720 (16:9)'
# Effect switches (bbport.ini, in-game menu, launcher): key -> (patch when the key is 0, patch
# when it is 1). The game reads these at start; a change applies after a restart.
EFFECTS={
    'effect_chromatic_aberration':('Disable Chromatic Aberration',None),
    'effect_dof':('Disable DoF',None),
    'effect_motion_blur':('Disable Motion Blur (perf increase)',None),
    'effect_ssao':('Disable SSAO',None),
    'effect_game_aa':('Disable AA',None),
    'effect_dynamic_shadows':('Disable Dynamic Light Shadows (perf increase)',None),
    'effect_ssr':(None,'Enable Screen Space Reflections (READ NOTE)'),
    'skip_intro':(None,'Skip Intro'),
    'debug_camera':(None,'Restore Debug Camera'),
    'debug_menu':(None,'Restore Debug Menu (READ NOTES)'),
}
# Intel CPUs: the game's tone mapping turns black (DLC areas most; reported as darker than on the
# PS4): the game code runs natively, and Intel's approximate float instructions differ from the
# PS4's AMD CPU, so a block the game gates on a computed flag never runs. The community fix runs
# it always; on by default on Intel. BB_INTEL_TONEMAP_FIX=0/1 forces it off or on.
INTEL_TONEMAP='Intel Black Tonemap Fix'


def intel_cpu(cpuinfo='/proc/cpuinfo'):
    try:
        with open(cpuinfo) as f:
            return any(line.startswith('vendor_id') and 'GenuineIntel' in line for line in f)
    except OSError:
        return False


# The title's PLAY ONLINE / PLAY OFFLINE dialog: the port has no PSN, the game goes straight to the
# main menu offline. On by default (the launcher's switch); BB_SKIP_NETWORK_CHOICE=0 shows it.
SKIP_NETWORK_CHOICE='Skip Online/Offline Choice'


def skip_network_choice(env=os.environ):
    return env.get('BB_SKIP_NETWORK_CHOICE','1')!='0'


def intel_tonemap_fix(env=os.environ, cpuinfo='/proc/cpuinfo'):
    forced=env.get('BB_INTEL_TONEMAP_FIX')
    return forced=='1' if forced in ('0','1') else intel_cpu(cpuinfo)

# model_lod: -2 highest, 0 the game's, 1 lower, 2 lowest.
MODEL_LOD={'-2':'Model LOD -2 (Highest)','1':'Model LOD 1 (Lower)','2':'Model LOD 2 (Lowest)'}


DEBUG_MENU='Restore Debug Menu (READ NOTES)'
# What the patched game reads for the debug menu (BB_FILE_TRACE=1), under dvdroot_ps4. The game asks
# for lower-case names; the PS4's file system and the port's runtime ignore case.
DEBUG_MENU_FONTS=('adhoc/font/DbgFont14h.ccm','adhoc/font/DbgFont14h.tpf')
DEBUG_MENU_SHADERS=('adhoc/FontShader/debugFont_vs.vpo','adhoc/FontShader/debugFont_ps.ppo')


def find_ignoring_case(root, relative):
    """`root`/`relative`, each component matched ignoring case; None when absent."""
    path=Path(root)
    for part in relative.split('/'):
        exact=path/part
        if exact.exists():
            path=exact
            continue
        try:
            matches=[e for e in path.iterdir() if e.name.casefold()==part.casefold()]
        except OSError:
            return None
        if not matches:
            return None
        path=matches[0]
    return path


def debug_menu_problem(game):
    """Why the debug menu patch cannot work with this game folder, or None."""
    dvdroot=Path(game)/'dvdroot_ps4'
    missing=[]
    for relative in DEBUG_MENU_FONTS:
        found=find_ignoring_case(dvdroot,relative)
        if not found or not found.is_file() or found.stat().st_size==0:
            missing.append(relative)
    if not missing:
        return None
    hint=''
    if all(find_ignoring_case(dvdroot,'font/'+r.rsplit('/',1)[1]) for r in missing):
        hint=' (they are in dvdroot_ps4/font: the game reads them from dvdroot_ps4/adhoc/font)'
    return ('the debug menu needs the font files from https://www.nexusmods.com/bloodborne/mods/253 '
            f'in dvdroot_ps4 of the game folder or of a mod: missing {", ".join(missing)}{hint}')


def validate_patch_requirements(names, game):
    """The patches to apply: raises for conflicts; drops the debug menu (with a message) when its
    font files are missing, as the game crashes opening the menu without them."""
    if 'Restore Debug Camera' in names and 'Enemy Control' in names:
        raise ValueError('Restore Debug Camera conflicts with Enemy Control; enable only one')
    if DEBUG_MENU in names:
        problem=debug_menu_problem(game)
        if problem:
            print(f'Patches: debug menu off: {problem}',file=sys.stderr)
            return [n for n in names if n!=DEBUG_MENU]
        absent=[r for r in DEBUG_MENU_SHADERS if not find_ignoring_case(Path(game)/'dvdroot_ps4',r)]
        if absent:
            print(f'Patches: debug menu: {", ".join(absent)} not found (the mod\'s font shaders); '
                  'the menu may not draw',file=sys.stderr)
    return names


def effect_patches(settings):
    names=[]
    for key,(off,on) in EFFECTS.items():
        if key not in settings: continue
        name=on if settings[key]=='1' else off
        if name: names.append(name)
    lod=MODEL_LOD.get(settings.get('model_lod','0'))
    if lod: names.append(lod)
    return names
SCENE_WIDTH=0x02196A6B-EBOOT_BASE
SCENE_HEIGHT=0x02196A7A-EBOOT_BASE
UI_WIDTH=0x02358554-EBOOT_BASE
UI_HEIGHT=0x0235855D-EBOOT_BASE


def read_settings(path):
    settings={}
    if path.exists():
        for line in path.read_text(encoding='utf-8').splitlines():
            key,sep,value=line.partition('=')
            if sep and not line.startswith('#'): settings[key.strip()]=value.strip()
    return settings


def render_size(settings,override=''):
    """Render resolution for the upscaler preset, or None for native."""
    if override:
        w,h=(int(v) for v in override.lower().split('x'))
        return (w,h)
    if settings.get('upscaler','fsr3')=='off': return None
    preset=int(settings.get('preset','0') or 0)
    scale=PRESET_SCALES[max(0,min(preset,len(PRESET_SCALES)-1))]
    if scale==1.0: return None
    # Even sizes (the game has half-resolution buffers).
    return tuple(max(2,round(v/scale/2)*2) for v in OUTPUT_SIZE)


def output_size(settings):
    """Output (UI) size from bbport.ini output_res, e.g. 3840x2160; 1920x1080 by default."""
    try:
        w,h=(int(v) for v in settings.get('output_res','').lower().split('x'))
        if w>0 and h>0: return (w,h)
    except ValueError:
        pass
    return OUTPUT_SIZE


def scaled_sizes(settings):
    """(render, output) for an output other than 1080p (above it, or 720p for the Steam Deck):
    the game renders at output / preset scale (or at the output size without upscaler) and the
    upscaler fills the output. None at 1080p and for TAA (native, live host targets only)."""
    out=output_size(settings)
    if out==OUTPUT_SIZE or settings.get('upscaler')=='taa': return None
    scale=1.0
    if settings.get('upscaler','fsr3')!='off':
        preset=int(settings.get('preset','0') or 0)
        scale=PRESET_SCALES[max(0,min(preset,len(PRESET_SCALES)-1))]
    render=tuple(max(2,round(v/scale/2)*2) for v in out)
    # A scene of exactly 1920x1080 (4K Performance) is indistinguishable from the game's UI
    # coordinate space, which the port's UI composition recognizes by that size.
    if render==OUTPUT_SIZE: render=(1916,1078)
    return render,out


def resolution_writes(xml,size,app_version,segments,ui=OUTPUT_SIZE):
    writes=compile_patches(xml,[RESOLUTION_TEMPLATE],app_version,segments)
    replacements={SCENE_WIDTH:(0xB8,0x500,size[0]),
                  SCENE_HEIGHT:(0xB8,0x2D0,size[1]),
                  UI_WIDTH:(0xB8,0x500,ui[0]),
                  UI_HEIGHT:(0xB9,0x2D0,ui[1])}
    out=[]
    seen=set()
    for offset,data in writes:
        if offset in replacements:
            opcode,old,value=replacements[offset]
            if data!=bytes([opcode])+old.to_bytes(3,'little') or offset in seen:
                raise ValueError(f'unexpected resolution patch at {offset+EBOOT_BASE:#x}')
            data=bytes([opcode])+value.to_bytes(3,'little')
            seen.add(offset)
        out.append((offset,data))
    if seen!=replacements.keys():
        raise ValueError('resolution patch is missing scene/UI viewport instructions')
    return out


def eboot_segments(elf):
    phoff,=struct.unpack_from('<Q',elf,0x20)
    phentsize,phnum=struct.unpack_from('<HH',elf,0x36)
    segments=[]
    for i in range(phnum):
        kind,_,_,vaddr,_,_,memsz,_=struct.unpack_from('<IIQQQQQQ',elf,phoff+i*phentsize)
        if kind==1: segments.append((vaddr,vaddr+memsz))
    return segments


def encode(line):
    kind,value=line.get('Type'),line.get('Value')
    if kind=='bytes': return bytes.fromhex(value.replace(' ',''))
    if kind in ('bytes16','bytes32','bytes64'):
        return int(value,0).to_bytes(int(kind[5:])//8,'little')
    if kind=='float32': return struct.pack('<f',float(value))
    if kind=='float64': return struct.pack('<d',float(value))
    if kind=='utf8': return value.encode()+b'\0'
    if kind=='utf16': return value.encode('utf-16-le')+b'\0\0'
    raise ValueError(f'unsupported patch type {kind!r}')


def compile_patches(xml, names, app_version, segments):
    found={}
    for meta in ET.parse(xml).getroot().iter('Metadata'):
        if meta.get('Name') in names and meta.get('AppVer')==app_version and meta.get('AppElf','eboot.bin')=='eboot.bin':
            found[meta.get('Name')]=meta
    missing=[n for n in names if n not in found]
    if missing: raise ValueError(f'patches not found for app version {app_version}: {missing}')
    writes=[]
    for name in names:
        for line in found[name].iter('Line'):
            offset=int(line.get('Address'),0)-EBOOT_BASE
            data=encode(line)
            if not any(start<=offset and offset+len(data)<=end for start,end in segments):
                raise ValueError(f'{name}: address {line.get("Address")} is outside the eboot')
            writes.append((offset,data))
    return writes


# Third-party patch files (shadPS4/GoldHEN XML) in the data directory's patches/ folder.
# The retail releases (scripts/game_check.py SUPPORTED_TITLES): one 1.09 executable.
BLOODBORNE_IDS={'CUSA00900','CUSA00207','CUSA00208','CUSA00299','CUSA01363',
                'CUSA03179','CUSA03173','CUSA03014','CUSA03023'}


def external_patches(directory, app_version='01.09', exclude=Path(__file__).resolve().parent.parent/'patches/Bloodborne.xml'):
    """[(key, file, metadata)] of eboot patches for this version in directory/*.xml.
    key is "<file name>/<patch name>" (the launcher's selection, patches.json)."""
    found=[]
    directory=Path(directory)
    for path in sorted(directory.glob('*.xml')) if directory.is_dir() else []:
        if path.resolve()==Path(exclude).resolve(): continue  # the built-in file (patches/ in a checkout)
        try:
            root=ET.parse(path).getroot()
        except ET.ParseError as error:
            print(f'Patches: {path.name}: {error}',file=sys.stderr)
            continue
        ids={e.text.strip() for e in root.iter('ID') if e.text}
        if ids and not ids&BLOODBORNE_IDS: continue
        for meta in root.iter('Metadata'):
            if meta.get('AppVer')==app_version and meta.get('AppElf','eboot.bin')=='eboot.bin':
                found.append((f'{path.name}/{meta.get("Name")}',path,meta))
    return found


def external_selection(found, config):
    """Selected external patches: patches.json {"enabled": [...], "disabled": [...]} overrides
    each file's isEnabled."""
    settings={}
    if config and Path(config).is_file():
        settings=json.loads(Path(config).read_text(encoding='utf-8'))
    enabled,disabled=set(settings.get('enabled',[])),set(settings.get('disabled',[]))
    return [(key,path,meta) for key,path,meta in found
            if key in enabled or (key not in disabled and meta.get('isEnabled','false').lower()=='true')]


def compile_external(selected, segments):
    """Writes of the selected external patches; a patch with unsupported lines is skipped whole."""
    writes=[]
    for key,_,meta in selected:
        try:
            ours=[]
            for line in meta.iter('Line'):
                offset=int(line.get('Address') or '',0)-EBOOT_BASE
                data=encode(line)
                if not any(start<=offset and offset+len(data)<=end for start,end in segments):
                    raise ValueError(f'address {line.get("Address")} is outside the eboot')
                ours.append((offset,data))
        except ValueError as error:
            print(f'Patches: skipped {key}: {error}',file=sys.stderr)
            continue
        writes+=ours
        print(f'Patches: external {key} ({len(ours)} writes)')
    return writes


# Ratchet & Clank (2016), CUSA01073 1.00: the game paces its frames itself, every swap interval x
# vblank period (2 x 1/59.94 s: 30 FPS) of its display object, a value set once at start
# (vaddr 0x1722f6e: mov [r14+0x1c],eax from its configuration). The patch stores the preset's
# interval there instead. The simulation clock also has a 1/60 s floor (below). "uncap" (interval 0)
# leaves pacing to the port (BB_FPS_LIMIT). The neighbouring store of the unused fallback
# interval (only read when the flag set right after is clear) makes room for the constant.
# The period is the 59.94 Hz the port reports, whatever BB_VBLANK_HZ is, and the game waits until
# (interval - 0.5) periods after the last flip: it counts on flips landing on vblank ticks of the
# preset's rate. At another vblank rate (240 Hz flips at once) interval 1 waited half a frame and
# "60" ran at ~68 FPS; there the preset is interval 0 and the port's limiter (ratchet_pacing).
RATCHET_IDS={'CUSA01073','CUSA01047'} # EU, US (untested: patched only where its code matches 1.00)
RATCHET_FPS={'30':None,'60':1.0,'90':59.94/90,'120':59.94/120,'uncap':0.0}
RATCHET_INTERVAL_SITE=0x1722f6e
RATCHET_INTERVAL_CODE=bytes.fromhex('4189461c8b4628be080000004c89ff41894620')
# Clock constructor: vmaxsd xmm1,xmm0,[1/60]. At 120 FPS this forces every update to
# advance at least 16.67 ms. Keep the configured/pacing minimum in xmm0, without that
# extra 60 Hz floor. The maximum-delta clamp and the clock's time scale stay intact.
RATCHET_CLOCK_SITE=0x16991f8
RATCHET_CLOCK_CODE=bytes.fromhex('c5fb5f0dd05b3c01')
RATCHET_CLOCK_PATCH=bytes.fromhex('c5f928c890909090') # vmovapd xmm1,xmm0; nop x4


def ratchet_pacing(fps,vblank_hz):
    """(interval, frame limit) for a preset at a vblank rate: the game's own pacing where flips
    land on ticks of the preset's rate (30 at 60 Hz: the original interval, None), else the same
    interval plus BB_FPS_LIMIT=fps. Interval 0 there froze level loads (black screen after Continue
    on the RTX 4070S, 2026-10-17), so the game keeps its interval and the port's limiter caps it."""
    if fps=='uncap':
        return 0.0,0
    if vblank_hz==(60 if fps=='30' else int(fps)):
        return RATCHET_FPS[fps],0
    return RATCHET_FPS[fps],int(fps)


def ratchet_fps_writes(fps,eboot,vblank_hz=None):
    interval=RATCHET_FPS[fps] if vblank_hz is None else ratchet_pacing(fps,vblank_hz)[0]
    if interval is None:
        return []
    phoff,=struct.unpack_from('<Q',eboot,0x20)
    phentsize,phnum=struct.unpack_from('<HH',eboot,0x36)
    file_offset=None
    clock_offset=None
    for i in range(phnum):
        kind,_,off,vaddr,_,filesz,_,_=struct.unpack_from('<IIQQQQQQ',eboot,phoff+i*phentsize)
        if kind==1 and vaddr<=RATCHET_INTERVAL_SITE and RATCHET_INTERVAL_SITE+len(RATCHET_INTERVAL_CODE)<=vaddr+filesz:
            file_offset=off+RATCHET_INTERVAL_SITE-vaddr
        if kind==1 and vaddr<=RATCHET_CLOCK_SITE and RATCHET_CLOCK_SITE+len(RATCHET_CLOCK_CODE)<=vaddr+filesz:
            clock_offset=off+RATCHET_CLOCK_SITE-vaddr
    if file_offset is None or eboot[file_offset:file_offset+len(RATCHET_INTERVAL_CODE)]!=RATCHET_INTERVAL_CODE:
        print('Patches: this Ratchet & Clank executable differs from 1.00; frame rate left at 30')
        return []
    if clock_offset is None or eboot[clock_offset:clock_offset+len(RATCHET_CLOCK_CODE)]!=RATCHET_CLOCK_CODE:
        print('Patches: this Ratchet & Clank clock differs from 1.00; frame rate left at 30')
        return []
    code=(bytes.fromhex('41c7461c')+struct.pack('<f',interval)  # mov dword [r14+0x1c],interval
          +bytes.fromhex('6a085e')                               # push 8; pop rsi (mov esi,8)
          +bytes.fromhex('4c89ff'))                              # mov rdi,r15
    code+=b'\x90'*(len(RATCHET_INTERVAL_CODE)-len(code))
    return [(RATCHET_INTERVAL_SITE,code),(RATCHET_CLOCK_SITE,RATCHET_CLOCK_PATCH)]


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--patches-dir',type=Path,help='third-party patch XML files (shadPS4 format)')
    p.add_argument('--patches-config',type=Path,help='patches.json: enabled/disabled external patches')
    p.add_argument('--xml',type=Path,default=Path(__file__).resolve().parent.parent/'patches/Bloodborne.xml')
    p.add_argument('--fps',choices=sorted(set(FPS_PRESETS)|set(RATCHET_FPS)),default='uncap')
    p.add_argument('--extra',default='',help='additional patch names, separated by ";"')
    p.add_argument('--vblank-hz',type=int,default=None,help='BB_VBLANK_HZ (Ratchet & Clank pacing)')
    p.add_argument('--print-frame-limit',action='store_true',
                   help='print the BB_FPS_LIMIT the preset needs at --vblank-hz (0: none)')
    p.add_argument('--app-version',default='01.09')
    p.add_argument('--out',type=Path,default=Path(__file__).resolve().parent.parent/'out')
    p.add_argument('--settings',type=Path,default=Path(__file__).resolve().parent.parent/'bbport.ini')
    p.add_argument('--game-dir',type=Path,default=Path(os.environ.get('BB_GAME_DIR','../CUSA03173')))
    p.add_argument('--render-res',default='',help='render resolution WxH (overrides the preset)')
    p.add_argument('--print-preset-size',action='store_true',help='print the selected preset size, if reduced')
    p.add_argument('--output-res',default='',help='output resolution WxH (the upscaler\'s; the UI stays 1920x1080)')
    p.add_argument('--print-scaled',action='store_true',
                   help='print "RENDER OUTPUT" (WxH) when bbport.ini selects an output other than 1080p')
    a=p.parse_args()
    if a.print_scaled:
        sizes=scaled_sizes(read_settings(a.settings))
        if sizes: print(f'{sizes[0][0]}x{sizes[0][1]} {sizes[1][0]}x{sizes[1][1]}')
        return
    if a.print_preset_size:
        settings=read_settings(a.settings)
        if 'BB_UPSCALER' in os.environ:
            settings['upscaler']='fsr3' if os.environ['BB_UPSCALER']=='fsr3' else 'off'
        if 'BB_UPSCALE_PRESET' in os.environ:
            settings['preset']=os.environ['BB_UPSCALE_PRESET']
        size=render_size(settings)
        if size: print(f'{size[0]}x{size[1]}')
        return
    # The built-in patches are Bloodborne 1.09 addresses: another game gets none of them.
    title=json.loads((a.out/'analysis.json').read_text(encoding='utf-8'))['sfo'].get('TITLE_ID','')
    if a.print_frame_limit:
        limit=0
        if title in RATCHET_IDS and a.fps in RATCHET_FPS and a.vblank_hz is not None:
            limit=ratchet_pacing(a.fps,a.vblank_hz)[1]
        print(limit)
        return
    if title in RATCHET_IDS:
        writes=ratchet_fps_writes(a.fps,(a.out/'eboot.elf').read_bytes(),a.vblank_hz)
        blob=struct.pack('<8sQQ',b'BBPATCH2',EBOOT_BASE,len(writes))
        for offset,data in writes: blob+=struct.pack('<QQ',offset,len(data))+data
        (a.out/'patches.bin').write_bytes(blob)
        print(f'Patches: Ratchet & Clank frame rate {a.fps}; {len(writes)} writes')
        return
    if a.fps not in FPS_PRESETS:
        sys.exit(f'Patches: frame rate preset {a.fps} is not available for {title}')
    if title not in BLOODBORNE_IDS:
        (a.out/'patches.bin').write_bytes(struct.pack('<8sQQ',b'BBPATCH2',EBOOT_BASE,0))
        print(f'Patches: none ({title} is not Bloodborne)')
        return
    names=FPS_PRESETS[a.fps]+[n.strip() for n in a.extra.split(';') if n.strip()]
    names+=[n for n in effect_patches(read_settings(a.settings)) if n not in names]
    if intel_tonemap_fix() and INTEL_TONEMAP not in names:
        names.append(INTEL_TONEMAP)
    if skip_network_choice() and SKIP_NETWORK_CHOICE not in names:
        names.append(SKIP_NETWORK_CHOICE)
    names=validate_patch_requirements(names,a.game_dir)
    segments=eboot_segments((a.out/'eboot.elf').read_bytes())
    writes=compile_patches(a.xml,names,a.app_version,segments)
    size=render_size(read_settings(a.settings),a.render_res) if a.render_res else None
    # The UI keeps the game's 1920x1080 coordinates even for a larger output: the port draws
    # it into the output-size image with a viewport scaled by output / 1920
    # (UiComposition::NativeViewport), so it is rasterized at the output resolution.
    ui=OUTPUT_SIZE
    if size:
        writes+=resolution_writes(a.xml,size,a.app_version,segments,ui)
        if size[0]*size[1]>OUTPUT_SIZE[0]*OUTPUT_SIZE[1]:
            heap='Increased Graphics Heap Sizes'
            writes+=compile_patches(a.xml,[heap],a.app_version,segments)
            names.append(heap)
        print(f'Patches: scene {size[0]}x{size[1]}; UI {ui[0]}x{ui[1]}')
    if a.patches_dir:
        # After the built-in ones: an external patch of the same bytes wins.
        writes+=compile_external(external_selection(external_patches(a.patches_dir,a.app_version,a.xml),
                                                    a.patches_config),segments)
    # BBPATCH2: the patch base, so the loader can rebase pointers the patches write into
    # relocated slots (60/90 FPS++ replace function pointers).
    blob=struct.pack('<8sQQ',b'BBPATCH2',EBOOT_BASE,len(writes))
    for offset,data in writes: blob+=struct.pack('<QQ',offset,len(data))+data
    (a.out/'patches.bin').write_bytes(blob)
    print(f'Patches: FPS preset {a.fps}; {len(writes)} writes from {names or "none"}')


if __name__=='__main__':
    main()
