#!/usr/bin/env bash
# Builds dist/Bloodborne-bbport-x86_64.AppImage: the port's current build (run build.sh first),
# its scripts and the launcher, bundled with their Nix closure by nix-appimage, then repacked so
# it needs no user namespaces (packaging/relocate.py: Ubuntu 23.10 and later restrict them).
# Running it opens the launcher; `--play` starts the game with the launcher's saved settings.
# Data (generated files, saves, bbport.ini): ~/.local/share/bbport (BB_DATA_DIR).
# BB_PACKAGE_GAME=ratchet: dist/Ratchet-Clank-port-x86_64.AppImage, the Ratchet & Clank launcher,
# data in ~/.local/share/ratchet-port.
set -euo pipefail
# LINK: where the bundle's store is found at run time (as long as /nix/store, one per game).
if [[ ${BB_PACKAGE_GAME:-} == ratchet ]]; then name=Ratchet-Clank-port link=/tmp/.racS
else name=Bloodborne-bbport link=/tmp/.bbpS; fi
cd -- "$(dirname -- "$0")/.."
[[ -f out/bb-probe && -f out/gpu/libbbgpu.so ]] || { echo 'Build first: bash build.sh' >&2; exit 1; }
root=$PWD
# The libraries' store paths: for each library a binary needs (NEEDED), the RUNPATH directory it
# is found in (their closures come along). Not every RUNPATH entry: built in nix-shell, the
# binaries list the lib directories of the whole build environment (the full GCC, Vulkan headers,
# SPIRV-Tools, ...), hundreds of MB the game never loads.
needed_dirs() {
    local elf=$1 lib dir
    local -a rpath
    mapfile -t rpath < <(readelf -d "$elf" | sed -n 's/.*R\{0,1\}U\{0,1\}N\{0,1\}PATH.*\[\(.*\)\]/\1/p' | tr ':' '\n')
    for lib in $(readelf -d "$elf" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'); do
        for dir in "${rpath[@]}"; do
            [[ $dir == /nix/store/* && -e $dir/$lib ]] && { echo "$dir"; break; }
        done
    done
}
{
    echo '['
    for elf in out/bb-probe out/gpu/libbbgpu.so $(ls out/libbbport_dlss.so out/gpu/libbbnet.so 2>/dev/null); do
        needed_dirs "$elf"
    done | grep -o '^/nix/store/[^/]*' | sort -u | grep -v -- '-nix-shell$' | sed 's/.*/  "&"/'
    echo ']'
} > packaging/runtime-paths.nix
work=$(mktemp -d)
trap 'chmod -R u+w "$work" 2> /dev/null; rm -rf "$work"' EXIT
(cd "$work" && nix bundle --impure --bundler github:ralismark/nix-appimage \
    --expr "import $root/packaging {}")
# Repacked: the store with its paths rewritten to $link, and packaging/AppRun, behind the same
# AppImage runtime.
bundle=$(echo "$work"/*.AppImage)
offset=$(python3 packaging/relocate.py offset "$bundle")
squashfs=$(nix-build '<nixpkgs>' -A squashfsTools --no-out-link)/bin
"$squashfs/unsquashfs" -q -n -d "$work/root" -o "$offset" "$bundle"
python3 packaging/relocate.py appdir "$work/root" "$work/AppDir" "$link"
# Timestamps from SOURCE_DATE_EPOCH (set by nix-shell).
"$squashfs/mksquashfs" "$work/AppDir" "$work/image" -comp zstd -all-root -no-xattrs -noappend -quiet
mkdir -p dist
head -c "$offset" "$bundle" > "$work/runtime"
cat "$work/runtime" "$work/image" > "dist/$name-x86_64.AppImage"
chmod 755 "dist/$name-x86_64.AppImage"
# A GC root for the package: nix-collect-garbage keeps it, and with it the outputs built here from
# source (the Vulkan-only Mesa, GTK 4 without GStreamer, libadwaita, SDL3), so the next AppImage
# does not build them again (only a nixpkgs update does).
nix-build --impure packaging -o out/nix-roots/bbport > /dev/null
ls -lh "dist/$name-x86_64.AppImage"
