// SPDX-License-Identifier: GPL-2.0-or-later
// FillClearValue: the clear color that leaves an image's memory as a dword fill would.
#include <cassert>
#include <cstdio>
#include "video_core/renderer_vulkan/fill_clear_value.h"

int main() {
    using Vulkan::FillClearValue;
    // Zero is zero in any color format (LBP3's light atlases, RGBA16F).
    const auto zero = FillClearValue(vk::Format::eR16G16B16A16Sfloat, 0);
    assert(zero && zero->uint32[0] == 0 && zero->uint32[3] == 0);
    // LBP3's scene fill: 0xFF000000 in RGBA8 memory is black with alpha 1.
    const auto rgba = FillClearValue(vk::Format::eR8G8B8A8Unorm, 0xff000000u);
    assert(rgba && rgba->float32[0] == 0.0f && rgba->float32[2] == 0.0f &&
           rgba->float32[3] == 1.0f);
    // BGRA keeps byte 0 as blue.
    const auto bgra = FillClearValue(vk::Format::eB8G8R8A8Unorm, 0x000000ffu);
    assert(bgra && bgra->float32[2] == 1.0f && bgra->float32[0] == 0.0f);
    // sRGB only where the encoding is exact (0 and 255).
    assert(FillClearValue(vk::Format::eR8G8B8A8Srgb, 0xff0000ffu));
    assert(!FillClearValue(vk::Format::eR8G8B8A8Srgb, 0x80808080u));
    const auto r32 = FillClearValue(vk::Format::eR32Uint, 0xdeadbeefu);
    assert(r32 && r32->uint32[0] == 0xdeadbeefu);
    // A nonzero value in a format the dword does not map onto: no clear (the shader runs).
    assert(!FillClearValue(vk::Format::eR16G16B16A16Sfloat, 0x3c003c00u));
    std::printf("fill-clear-test: ok\n");
    return 0;
}
