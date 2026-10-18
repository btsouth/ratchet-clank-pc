// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include "common/logging/log.h"
#include "shader_recompiler/resource.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/image_view.h"

#include <magic_enum/magic_enum.hpp>

namespace VideoCore {

vk::ImageViewType ConvertImageViewType(AmdGpu::ImageType type) {
    switch (type) {
    case AmdGpu::ImageType::Color1D:
        return vk::ImageViewType::e1D;
    case AmdGpu::ImageType::Color1DArray:
        return vk::ImageViewType::e1DArray;
    case AmdGpu::ImageType::Color2D:
    case AmdGpu::ImageType::Color2DMsaa:
        return vk::ImageViewType::e2D;
    case AmdGpu::ImageType::Color2DArray:
        return vk::ImageViewType::e2DArray;
    case AmdGpu::ImageType::Color3D:
        return vk::ImageViewType::e3D;
    default:
        UNREACHABLE();
    }
}

bool IsViewTypeCompatible(AmdGpu::ImageType view_type, AmdGpu::ImageType image_type) {
    switch (view_type) {
    case AmdGpu::ImageType::Color1D:
    case AmdGpu::ImageType::Color1DArray:
        return image_type == AmdGpu::ImageType::Color1D;
    case AmdGpu::ImageType::Color2D:
    case AmdGpu::ImageType::Color2DArray:
    case AmdGpu::ImageType::Color2DMsaa:
    case AmdGpu::ImageType::Color2DMsaaArray:
        return image_type == AmdGpu::ImageType::Color2D || image_type == AmdGpu::ImageType::Color3D;
    case AmdGpu::ImageType::Color3D:
        return image_type == AmdGpu::ImageType::Color3D;
    default:
        UNREACHABLE();
    }
}


// bbport BB_LOG_FORMATS=1 (diagnostics): each distinct use of the 11/11/10-bit float formats and
// each color target with a component swap, with the raw format and swizzle (first 200).
static void LogFormatUse(const char* kind, u64 address, u32 raw_format, u32 sel) {
    static const bool enabled = std::getenv("BB_LOG_FORMATS") != nullptr;
    if (!enabled) {
        return;
    }
    static std::mutex lock;
    static std::set<std::tuple<std::string, u64, u32, u32>> seen;
    std::scoped_lock lk{lock};
    if (seen.size() < 200 && seen.emplace(kind, address, raw_format, sel).second) {
        std::printf("Format use: %s %#llx data format %u sel %#x\n", kind,
                    (unsigned long long)address, raw_format, sel);
    }
}

ImageViewInfo::ImageViewInfo(const AmdGpu::Image& image, const Shader::ImageResource& desc) noexcept
    : is_storage{desc.is_written} {
    if (image.data_format == 6 || image.data_format == 7) {
        LogFormatUse(desc.is_written ? "storage" : "texture", image.Address(), image.data_format,
                     image.dst_sel_x | image.dst_sel_y << 3 | image.dst_sel_z << 6 |
                         image.dst_sel_w << 9);
    }
    const auto dfmt = image.GetDataFmt();
    auto nfmt = image.GetNumberFmt();
    if (is_storage && nfmt == AmdGpu::NumberFormat::Srgb) {
        nfmt = AmdGpu::NumberFormat::Unorm;
    }
    format = Vulkan::LiverpoolToVK::SurfaceFormat(dfmt, nfmt);
    if (desc.is_depth) {
        format = Vulkan::LiverpoolToVK::PromoteFormatToDepth(format);
    }

    range.base.level = image.base_level;
    range.base.layer = std::min<u32>(image.base_array, image.NumLayers() - 1);
    range.extent.levels = image.NumViewLevels(desc.is_array);
    range.extent.layers = image.NumViewLayers(desc.is_array);
    type = image.GetViewType(desc.is_array);
    min_lod = static_cast<u32>(image.min_lod);

    if (!is_storage) {
        mapping = Vulkan::LiverpoolToVK::ComponentMapping(image.DstSelect());
    }
}

ImageViewInfo::ImageViewInfo(const AmdGpu::ColorBuffer& col_buffer) noexcept {
    if (const u32 raw = u32(col_buffer.info.format); raw == 6 || raw == 7 ||
                                                     u32(col_buffer.info.comp_swap) != 0) {
        LogFormatUse("target", col_buffer.Address(), raw, u32(col_buffer.info.comp_swap));
    }
    range.base.layer = col_buffer.BaseSlice();
    range.extent.layers = col_buffer.NumSlices() - range.base.layer;
    type = range.extent.layers > 1 ? AmdGpu::ImageType::Color2DArray : AmdGpu::ImageType::Color2D;
    format =
        Vulkan::LiverpoolToVK::SurfaceFormat(col_buffer.GetDataFmt(), col_buffer.GetNumberFmt());
}

ImageViewInfo::ImageViewInfo(const AmdGpu::DepthBuffer& depth_buffer, AmdGpu::DepthView view,
                             AmdGpu::DepthControl ctl) {
    format = Vulkan::LiverpoolToVK::DepthFormat(depth_buffer.z_info.format,
                                                depth_buffer.stencil_info.format);
    is_storage = ctl.depth_write_enable;
    range.base.layer = view.slice_start;
    range.extent.layers = view.NumSlices() - range.base.layer;
    type = range.extent.layers > 1 ? AmdGpu::ImageType::Color2DArray : AmdGpu::ImageType::Color2D;
}

ImageView::ImageView(const Vulkan::Instance& instance, const ImageViewInfo& info_,
                     const Image& image)
    : info{info_} {
    vk::ImageViewUsageCreateInfo usage_ci{.usage = image.usage_flags};
    if (!info.is_storage) {
        usage_ci.usage &= ~vk::ImageUsageFlagBits::eStorage;
    }
    if (image.info.props.is_volume && info.type != AmdGpu::ImageType::Color3D) {
        // bbport: a 2D (array) view of slices of a volume (2D-array compatible) may only be an
        // attachment; sampling one needs VK_EXT_image_2d_view_of_3d (one slice, 2D view).
        auto allowed = vk::ImageUsageFlagBits::eColorAttachment |
                       vk::ImageUsageFlagBits::eDepthStencilAttachment |
                       vk::ImageUsageFlagBits::eInputAttachment;
        if (info.type == AmdGpu::ImageType::Color2D && info.range.extent.layers == 1 &&
            instance.Is2dViewOf3dSupported()) {
            allowed |= vk::ImageUsageFlagBits::eSampled;
            if (info.is_storage) {
                allowed |= vk::ImageUsageFlagBits::eStorage;
            }
        }
        usage_ci.usage &= allowed;
    }
    vk::ImageViewMinLodCreateInfoEXT min_lod_ci{};
    if (info.min_lod != 0 && instance.IsImageViewMinLodSupported()) {
        const float last_level =
            static_cast<float>(info.range.base.level + info.range.extent.levels - 1);
        min_lod_ci.minLod = std::min(static_cast<float>(info.min_lod) / 256.f, last_level);
        usage_ci.pNext = &min_lod_ci;
    }
    // When sampling D32/D16 texture from shader, the T# specifies R32/R16 format so adjust it.
    vk::Format format = info.format;
    vk::ImageAspectFlags aspect = image.aspect_mask;
    if (image.aspect_mask & vk::ImageAspectFlagBits::eDepth &&
        Vulkan::LiverpoolToVK::IsFormatDepthCompatible(format)) {
        format = image.info.pixel_format;
        aspect = vk::ImageAspectFlagBits::eDepth;
    }
    if (image.aspect_mask & vk::ImageAspectFlagBits::eStencil &&
        Vulkan::LiverpoolToVK::IsFormatStencilCompatible(format)) {
        format = image.info.pixel_format;
        aspect = vk::ImageAspectFlagBits::eStencil;
    }

    const vk::ImageViewCreateInfo image_view_ci = {
        .pNext = &usage_ci,
        .image = image.GetImage(),
        .viewType = ConvertImageViewType(info.type),
        .format = instance.GetSupportedFormat(format, image.format_features),
        .components = info.mapping,
        .subresourceRange{
            .aspectMask = aspect,
            .baseMipLevel = info.range.base.level,
            .levelCount = info.range.extent.levels,
            .baseArrayLayer = info.range.base.layer,
            .layerCount = info.range.extent.layers,
        },
    };
    if (!IsViewTypeCompatible(info.type, image.info.type)) {
        LOG_ERROR(Render_Vulkan, "image view type {} is incompatible with image type {}",
                  magic_enum::enum_name(info.type), magic_enum::enum_name(image.info.type));
    }

    auto [view_result, view] = instance.GetDevice().createImageViewUnique(image_view_ci);
    ASSERT_MSG(view_result == vk::Result::eSuccess, "Failed to create image view: {}",
               vk::to_string(view_result));
    image_view = std::move(view);

    const auto view_aspect = aspect & vk::ImageAspectFlagBits::eDepth     ? "Depth"
                             : aspect & vk::ImageAspectFlagBits::eStencil ? "Stencil"
                                                                          : "Color";
    Vulkan::SetObjectName(
        instance.GetDevice(), *image_view, "ImageView {}x{}x{} {:#x}:{:#x} {}:{} {}:{} ({})",
        image.info.size.width, image.info.size.height, image.info.size.depth,
        image.info.guest_address, image.info.guest_size, info.range.base.level,
        info.range.base.level + info.range.extent.levels - 1, info.range.base.layer,
        info.range.base.layer + info.range.extent.layers - 1, view_aspect);
}

ImageView::~ImageView() = default;

} // namespace VideoCore
