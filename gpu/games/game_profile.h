// SPDX-License-Identifier: GPL-2.0-or-later
// Game profiles: what the translator knows about one game, kept out of its core. The core (the
// command processor, the caches, the shader recompiler) runs any game on its generic paths and
// asks the active profile only for the rest: hooks in the game's own code that announce its
// writes earlier than a write trap would, guest code known to write per-frame data, shaders with
// a known purpose. A game without a profile runs on the generic paths alone. One file per game
// (games/<game>.cpp) defines its profile; game_profile.cpp lists them.
#pragma once
#include <cstdint>
#include <span>

namespace Game {

/// Guest addresses [begin, end).
struct CodeRange {
    std::uint64_t begin = 0, end = 0;
};

/// What the core offers a profile's hooks.
struct CoreServices {
    /// The game handed out GPU memory [address, address + size): what the GPU had there is dead.
    void (*fresh_range)(std::uint64_t address, std::uint64_t size) = nullptr;
};

/// A game's own temporal anti-aliasing that the port's temporal upscaler (DLAA / FSR native AA)
/// can stand in for at the game's render size. Images are named by guest address.
struct SceneAa {
    /// The TAA resolve compute shader (GCN program hash): the upscaler runs just before it.
    std::uint64_t taa_shader = 0;
    /// The compute pass after it that copies the TAA result into the scene color: replaced by
    /// the upscaler's result when it ran.
    std::uint64_t copy_shader = 0;
    /// HDR scene color before TAA (the TAA reads it, the copy writes it).
    std::uint64_t color = 0;
    /// Motion vectors (RG16F), render pixels, current position minus previous, without jitter.
    std::uint64_t motion = 0;
    /// View-space depth (R32F), up to `far` for the sky.
    std::uint64_t linear_depth = 0;
    float far = 0;
    /// The TAA's first constant buffer holds this frame's jitter (x, y, render pixels) here.
    std::uint32_t jitter_dword = 0;
};

struct Profile {
    const char* name = nullptr;
    /// The param.sfo TITLE_IDs it is for.
    std::span<const char* const> serials;
    /// Hooks in the game's code (each checks the bytes it replaces: another build of the game is
    /// left alone). They make the core learn of writes sooner; it does not depend on them for
    /// what it shows (write traps catch what nobody announces).
    void (*install_hooks)(const CoreServices& services) = nullptr;
    /// The hooks also go in with the 0.3 memory model (VRAM copies, write tracking).
    bool hooks_any_memory_model = false;
    /// Guest code known to write per-frame data into GPU memory: the blocks it writes stay in
    /// the game's memory from its first write instead of after a few frames.
    std::span<const CodeRange> dynamic_writers;
    /// A compute shader (GCN program hash) that only copies buffers, into memory the translator
    /// reads on the CPU: its stores go through to the game's memory; 0 = none known.
    std::uint64_t buffer_copy_shader = 0;
    /// The game's own TAA, for the scene upscaler; nullptr = none known.
    const SceneAa* scene_aa = nullptr;
    /// Guest GPU memory the game's CPU code rewrites all the time (a vertex ring): with hot
    /// pages (BB_HOT_PAGES=1) only pages in these ranges stay writable, from their first write
    /// fault. Elsewhere hot pages also took in images the CPU fills and pages the GPU writes back.
    std::span<const CodeRange> streamed_memory;
    /// The pixel shader (GCN program hash) of the full-screen pass that blends the game's motion
    /// blur over the scene: left out when the Motion blur effect is off; 0 = none known.
    std::uint64_t motion_blur_ps = 0;
};

/// Chooses the profile for the game by its TITLE_ID (bbgpu_init). BB_GAME_PROFILE=none runs the
/// game as an unknown one (no profile), for checking the generic paths.
void Select(const char* serial);

/// The active profile, or nullptr (an unknown game, or BB_GAME_PROFILE=none).
const Profile* Active();

/// The active profile's buffer copy shader, or 0.
inline std::uint64_t BufferCopyShader() {
    const Profile* profile = Active();
    return profile ? profile->buffer_copy_shader : 0;
}

/// The active profile's motion blur pass (pixel shader hash), or 0.
inline std::uint64_t MotionBlurShader() {
    const Profile* profile = Active();
    return profile ? profile->motion_blur_ps : 0;
}

/// The active profile's TAA description, or nullptr.
inline const SceneAa* ActiveSceneAa() {
    const Profile* profile = Active();
    return profile ? profile->scene_aa : nullptr;
}

/// Whether guest_rip is in code the active profile knows to write per-frame data.
bool IsDynamicWriter(std::uint64_t guest_rip);

/// Whether the active profile names streamed memory, and whether `address` is in it.
bool HasStreamedMemory();
bool InStreamedMemory(std::uint64_t address);

} // namespace Game
