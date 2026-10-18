// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: a dword fill as an image clear (Rasterizer::IsComputeMaskedFill).
#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <optional>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

// bbport: the clear color that gives an image of this format the memory bytes of `value`
// repeated (one texel per dword), or nullopt where that is not exact.
inline std::optional<vk::ClearColorValue> FillClearValue(vk::Format format, u32 value) {
    if (value == 0) {
        return vk::ClearColorValue{.uint32 = std::array<u32, 4>{}}; // zero in any color format
    }
    const std::array<u32, 4> bytes = {value & 0xff, (value >> 8) & 0xff, (value >> 16) & 0xff,
                                      value >> 24};
    const auto unorm = [](u32 b) { return float(b) / 255.0f; };
    const auto uints = [](std::array<u32, 4> v) { return vk::ClearColorValue{.uint32 = v}; };
    const auto floats = [](std::array<float, 4> v) { return vk::ClearColorValue{.float32 = v}; };
    switch (format) {
    case vk::Format::eR32Uint:
    case vk::Format::eR32Sint:
        return uints({value, 0, 0, 0});
    case vk::Format::eR32Sfloat:
        return floats({std::bit_cast<float>(value), 0, 0, 0});
    case vk::Format::eR8G8B8A8Uint:
        return uints(bytes);
    case vk::Format::eR8G8B8A8Unorm:
    case vk::Format::eA8B8G8R8UnormPack32:
        return floats({unorm(bytes[0]), unorm(bytes[1]), unorm(bytes[2]), unorm(bytes[3])});
    case vk::Format::eB8G8R8A8Unorm:
        return floats({unorm(bytes[2]), unorm(bytes[1]), unorm(bytes[0]), unorm(bytes[3])});
    case vk::Format::eR8G8B8A8Srgb:
    case vk::Format::eB8G8R8A8Srgb: {
        // sRGB encodes the clear color; only 0 and 255 come back exactly.
        if (std::ranges::any_of(bytes, [](u32 b) { return b != 0 && b != 0xff; })) {
            return std::nullopt;
        }
        const bool bgra = format == vk::Format::eB8G8R8A8Srgb;
        return floats({unorm(bytes[bgra ? 2 : 0]), unorm(bytes[1]), unorm(bytes[bgra ? 0 : 2]),
                       unorm(bytes[3])});
    }
    default:
        return std::nullopt;
    }
}

} // namespace Vulkan
