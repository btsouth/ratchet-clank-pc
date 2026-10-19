// SPDX-License-Identifier: GPL-2.0-or-later
// Ratchet & Clank (2016), 1.00: what the translator knows about its frame. The shaders and
// images below were read from frame captures (BB_CAPTURE_TRIGGER) of the EU release, whose
// targets sit at the same addresses every session.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <x86intrin.h>
#include "game_profile.h"
#include "bbport_guest_hooks.h"
#include "bbport_toggles.h"

namespace Game {
namespace {
constexpr const char* Serials[] = {"CUSA01073"};

/// The TAA resolve (compute 4595ddd5, 240x135 groups of 8x8) reads the scene color, the motion
/// vectors at the nearest-depth pixel, linear depth and its history, and writes new history;
/// compute 17f7e64a then copies that into the scene color before bloom and tonemapping.
/// Its constants: [24..27] render size and its reciprocal (1920x1080), [38..39] this frame's
/// jitter in pixels (Halton 2/3 minus 0.5). Its history lookup, (pixel + 0.5) - motion, shows the
/// motion vectors are pixels, current minus previous; on still geometry they are about 0.01 px,
/// so they carry no jitter. The linear depth is 1/w from the motion vector pass (sky: 1000).
constexpr SceneAa RatchetTaa = {
    .taa_shader = 0x4595ddd5,
    .copy_shader = 0x17f7e64a,
    .color = 0x1084e88000,
    .motion = 0x1086e60000,
    .linear_depth = 0x1084690000,
    .far = 1000.0f,
    .jitter_dword = 38,
};
/// The geometry ring in the 188 MB GPU-visible block at 0x1100000000: job threads (+0x1792fff,
/// +0x179186a, +0x17915b7, +0x1791dc0) fill it every frame, ~85% of the write faults; the GPU
/// only reads it.
constexpr CodeRange StreamedMemory[] = {{0x1101000000, 0x1104000000}};

/// Those job threads: two parallel-for loops call the writer 0x1792b00 per work item (rdi the
/// item, rsi, rdx, rcx), from 0x1792aa3 and 0x1789179. They fill the frame's ring segment while
/// the draws that read it are already submitted (the PS4's GPU reaches them later). With async
/// write tracking the calls go through WriterItem, which counts the items running and stamps
/// their end (BbStats::writers_active, writers_end_tsc; MemoryTracker::SyncAsyncWrites).
/// BB_WRITER_HOOK=0: not hooked.
constexpr std::uint64_t ImageBase = 0x800000000ull;
constexpr std::uint64_t WriterEntry = 0x1792b00;
constexpr unsigned char WriterCallA[5] = {0xe8, 0x58, 0x00, 0x00, 0x00}; // at 0x1792aa3
constexpr unsigned char WriterCallB[5] = {0xe8, 0x82, 0x99, 0x00, 0x00}; // at 0x1789179
using WriterFn = void (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);

void WriterItem(std::uint64_t item, std::uint64_t b, std::uint64_t c, std::uint64_t d) {
    BbStats::writers_active.fetch_add(1, std::memory_order_seq_cst);
    reinterpret_cast<WriterFn>(ImageBase + WriterEntry)(item, b, c, d);
    BbStats::writers_end_tsc.store(__rdtsc(), std::memory_order_seq_cst);
    BbStats::writers_active.fetch_sub(1, std::memory_order_seq_cst);
    BbStats::writer_items.fetch_add(1, std::memory_order_relaxed);
}

void InstallHooks(const CoreServices&) {
    const char* env = std::getenv("BB_WRITER_HOOK");
    const char* async = std::getenv("BB_ASYNC_WP");
    const bool on = env ? env[0] == '1' : !(async && async[0] == '0');
    if (!on) {
        return;
    }
    const void* target = reinterpret_cast<const void*>(&WriterItem);
    const int hooked = BbGuestHooks::HookCall(0x1792aa3, WriterCallA, target) +
                       BbGuestHooks::HookCall(0x1789179, WriterCallB, target);
    std::printf("Guest hooks: %d of 2 geometry ring writer call sites counted (BB_WRITER_HOOK)\n",
                hooked);
}
} // namespace

extern const Profile RatchetProfile = {
    .name = "Ratchet & Clank",
    .serials = Serials,
    .install_hooks = InstallHooks,
    .hooks_any_memory_model = true,
    .scene_aa = &RatchetTaa,
    .streamed_memory = StreamedMemory,
    // The motion blur before the TAA: tile and neighbour maximum velocity (cs 233341ad,
    // bfbd37b0), a half-size gather (cs 2655cbd4, 1fbf981c, 0354eebe), then this full-screen
    // pass blends the result over the scene color by its coverage. Without it the scene is left
    // as drawn; the TAA still reads the gather's mask (unchanged).
    .motion_blur_ps = 0xe9bef76c,
};

} // namespace Game
