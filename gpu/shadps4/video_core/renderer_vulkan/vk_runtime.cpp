// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <boost/container/small_vector.hpp>
#include "bbport_toggles.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "video_core/renderer_vulkan/vk_scene_resolution.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/blit_helper.h"
#include "video_core/texture_cache/image.h"

#include <vulkan/vulkan_format_traits.hpp>
#include <vk_mem_alloc.h>

namespace Vulkan {

namespace {
/// bbport: a recording chunk holds 128 KiB with the command: longer region lists (LBP3 syncs a
/// buffer in thousands of pieces while loading a level) are recorded as several commands, in
/// order. Nothing between them needs a barrier: they are the regions of one copy.
template <typename T, typename F>
void ForEachRecordPart(std::span<const T> items, F&& record) {
    constexpr size_t MaxItems = (RecordChunk::Capacity - 4096) / sizeof(T);
    for (size_t at = 0; at < items.size(); at += MaxItems) {
        record(items.subspan(at, std::min(MaxItems, items.size() - at)));
    }
}
} // namespace

static vk::ImageType ConvertImageType(AmdGpu::ImageType type) noexcept {
    switch (type) {
    case AmdGpu::ImageType::Color1D:
    case AmdGpu::ImageType::Color1DArray:
        return vk::ImageType::e1D;
    case AmdGpu::ImageType::Color2D:
    case AmdGpu::ImageType::Color2DMsaa:
    case AmdGpu::ImageType::Color2DArray:
        return vk::ImageType::e2D;
    case AmdGpu::ImageType::Color3D:
        return vk::ImageType::e3D;
    default:
        UNREACHABLE_MSG("Unexpected image type {}", u32(type));
    }
}

/// bbport: the depth slices a range of a volume names as layers (render target views of a
/// volume address its slices that way), clamped to the slices of the range's mip level.
static std::pair<u32, u32> VolumeSlices(const VideoCore::ImageInfo& info,
                                        const VideoCore::SubresourceRange& range) {
    const u32 depth = std::max(info.size.depth >> range.base.level, 1u);
    const u32 first = std::min(range.base.layer, depth - 1);
    return {first, std::clamp(range.extent.layers, 1u, depth - first)};
}

static std::pair<u32, u32> SanitizeCopyLayers(const VideoCore::ImageInfo& src_info,
                                              const VideoCore::ImageInfo& dst_info,
                                              const u32 depth) {
    const auto vk_src_type = ConvertImageType(src_info.type);
    const auto vk_dst_type = ConvertImageType(dst_info.type);

    u32 src_layers = src_info.resources.layers;
    u32 dst_layers = dst_info.resources.layers;

    // 3D images can only use 1 layer.
    if (vk_src_type == vk::ImageType::e3D && src_layers != 1) {
        LOG_WARNING(Render_Vulkan, "Coercing copy 3D source layers {} to 1.", src_layers);
        src_layers = 1;
    }
    if (vk_dst_type == vk::ImageType::e3D && dst_layers != 1) {
        LOG_WARNING(Render_Vulkan, "Coercing copy 3D destination layers {} to 1.", dst_layers);
        dst_layers = 1;
    }

    // If the image type is equal, layer count must match. Take the minimum of both.
    if (vk_src_type == vk_dst_type) {
        if (src_layers != dst_layers) {
            LOG_WARNING(Render_Vulkan,
                        "Coercing copy source layers {} and destination layers {} to minimum.",
                        src_layers, dst_layers);
            src_layers = dst_layers = std::min(src_layers, dst_layers);
        }
    } else {
        // For 2D <-> 3D copies, 2D layer count must equal 3D depth.
        if (vk_src_type == vk::ImageType::e2D && vk_dst_type == vk::ImageType::e3D &&
            src_layers != depth) {
            LOG_WARNING(Render_Vulkan,
                        "Coercing copy 2D source layers {} to 3D destination depth {}", src_layers,
                        depth);
            src_layers = depth;
        }
        if (vk_src_type == vk::ImageType::e3D && vk_dst_type == vk::ImageType::e2D &&
            dst_layers != depth) {
            LOG_WARNING(Render_Vulkan,
                        "Coercing copy 2D destination layers {} to 3D source depth {}", dst_layers,
                        depth);
            dst_layers = depth;
        }
    }

    return std::make_pair(src_layers, dst_layers);
}

static u32 BufferImageCopySize(const vk::BufferImageCopy& copy, const vk::Format pixel_format) {
    const u32 row_length = copy.bufferRowLength ? copy.bufferRowLength : copy.imageExtent.width;
    const u32 height = copy.bufferImageHeight ? copy.bufferImageHeight : copy.imageExtent.height;

    const auto block = vk::blockExtent(pixel_format);
    const u32 block_size = vk::blockSize(pixel_format);
    const u32 row_pitch = (row_length / block[0]) * block_size;
    const u32 slice_pitch = (height / block[1]) * row_pitch;

    const u32 width_in_blocks = (copy.imageExtent.width + block[0] - 1) / block[0];
    const u32 height_in_blocks = (copy.imageExtent.height + block[1] - 1) / block[1];
    const u32 num_slices = copy.imageExtent.depth * copy.imageSubresource.layerCount;

    return (num_slices - 1) * slice_pitch + (height_in_blocks - 1) * row_pitch +
           width_in_blocks * block_size;
}

Runtime::Runtime(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_}, staging_pool{instance_, scheduler_} {
    blit_helper = std::make_unique<VideoCore::BlitHelper>(instance, scheduler);

    memory_barrier.dstStageMask = vk::PipelineStageFlagBits2::eAllCommands;
    memory_barrier.dstAccessMask =
        vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
}

void Runtime::TickFrame() {
    staging_pool.TickFrame();
}

void Runtime::CopyBuffer(const VideoCore::Buffer* src, const VideoCore::Buffer* dst,
                         std::span<const vk::BufferCopy> copies) {
    scheduler.EndRendering();

    // bbport: many regions (HLE copy shaders) are tracked as one bounding range per buffer:
    // conservative for barriers, and two tree lookups instead of two per region.
    const bool bounded = copies.size() > 4;
    u64 src_min = ~0ULL, src_max = 0, dst_min = ~0ULL, dst_max = 0;
    if (bounded) {
        for (const auto& copy : copies) {
            src_min = std::min<u64>(src_min, copy.srcOffset);
            src_max = std::max<u64>(src_max, copy.srcOffset + copy.size);
            dst_min = std::min<u64>(dst_min, copy.dstOffset);
            dst_max = std::max<u64>(dst_max, copy.dstOffset + copy.size);
        }
    }

    bool needs_flush{};
    if (bounded) {
        needs_flush = IsBufferAccessed(src, src_min, src_max - src_min) ||
                      IsBufferAccessed(dst, dst_min, dst_max - dst_min, true);
    } else {
        for (const auto& copy : copies) {
            needs_flush |= IsBufferAccessed(src, copy.srcOffset, copy.size);
            needs_flush |= IsBufferAccessed(dst, copy.dstOffset, copy.size, true);
        }
    }
    if (needs_flush) {
        FlushBarriers();
    }

    ForEachRecordPart(copies, [&](std::span<const vk::BufferCopy> part) {
        scheduler.Record([src_handle = src->Handle(), dst_handle = dst->Handle(),
                          regions = scheduler.RecordData(part)](vk::CommandBuffer cmdbuf) {
            cmdbuf.copyBuffer(src_handle, dst_handle, regions.size(), regions.data());
        });
    });

    if (bounded) {
        AccessBuffer(src, src_min, src_max - src_min, vk::PipelineStageFlagBits2::eCopy,
                     vk::AccessFlagBits2::eTransferRead);
        AccessBuffer(dst, dst_min, dst_max - dst_min, vk::PipelineStageFlagBits2::eCopy,
                     vk::AccessFlagBits2::eTransferWrite);
        return;
    }
    for (const auto& copy : copies) {
        AccessBuffer(src, copy.srcOffset, copy.size, vk::PipelineStageFlagBits2::eCopy,
                     vk::AccessFlagBits2::eTransferRead);
        AccessBuffer(dst, copy.dstOffset, copy.size, vk::PipelineStageFlagBits2::eCopy,
                     vk::AccessFlagBits2::eTransferWrite);
    }
}

void Runtime::FillBuffer(const VideoCore::Buffer* dst, u64 offset, u64 size, u32 value) {
    scheduler.EndRendering();

    if (IsBufferAccessed(dst, offset, size, true)) {
        FlushBarriers();
    }

    scheduler.Record([handle = dst->Handle(), offset, size, value](vk::CommandBuffer cmdbuf) {
        cmdbuf.fillBuffer(handle, offset, size, value);
    });

    AccessBuffer(dst, offset, size, vk::PipelineStageFlagBits2::eClear,
                 vk::AccessFlagBits2::eTransferWrite);
}

void Runtime::CopyFromGuestChunk(vk::Buffer src, const VideoCore::Buffer* dst,
                                 std::span<const vk::BufferCopy> copies) {
    if (copies.empty()) {
        return;
    }
    scheduler.EndRendering();
    bool needs_flush = false;
    for (const auto& copy : copies) {
        needs_flush |= IsBufferAccessed(dst, copy.dstOffset, copy.size, true);
    }
    if (needs_flush) {
        FlushBarriers();
    }
    ForEachRecordPart(copies, [&](std::span<const vk::BufferCopy> part) {
        scheduler.Record([src, dst_handle = dst->Handle(),
                          regions = scheduler.RecordData(part)](vk::CommandBuffer cmdbuf) {
            cmdbuf.copyBuffer(src, dst_handle, regions.size(), regions.data());
        });
    });
    for (const auto& copy : copies) {
        AccessBuffer(dst, copy.dstOffset, copy.size, vk::PipelineStageFlagBits2::eCopy,
                     vk::AccessFlagBits2::eTransferWrite);
    }
}

void Runtime::UpdateBuffer(const VideoCore::Buffer* dst, u64 offset, std::span<const u8> data) {
    if (data.empty()) {
        return;
    }
    scheduler.EndRendering();
    if (IsBufferAccessed(dst, offset, data.size(), true)) {
        FlushBarriers();
    }
    constexpr std::size_t Piece = 32 * 1024; // vkCmdUpdateBuffer takes 64 KiB at most
    for (std::size_t at = 0; at < data.size(); at += Piece) {
        const auto piece = data.subspan(at, std::min(Piece, data.size() - at));
        scheduler.Record([handle = dst->Handle(), to = offset + at,
                          bytes = scheduler.RecordData(piece)](vk::CommandBuffer cmdbuf) {
            cmdbuf.updateBuffer(handle, to, bytes.size_bytes(), bytes.data());
        });
    }
    AccessBuffer(dst, offset, data.size(), vk::PipelineStageFlagBits2::eClear,
                 vk::AccessFlagBits2::eTransferWrite);
}

void Runtime::InlineData(VideoCore::Buffer* dst, u64 offset, u32 value) {
    scheduler.EndRendering();

    if (IsBufferAccessed(dst, offset, sizeof(value), true)) {
        FlushBarriers();
    }

    scheduler.Record([handle = dst->Handle(), offset, value](vk::CommandBuffer cmdbuf) {
        cmdbuf.updateBuffer(handle, offset, sizeof(value), &value);
    });

    AccessBuffer(dst, offset, sizeof(value), vk::PipelineStageFlagBits2::eClear,
                 vk::AccessFlagBits2::eTransferWrite);
}

bool Runtime::Transit(VideoCore::Image* image, vk::ImageLayout dst_layout,
                      vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access,
                      std::optional<VideoCore::SubresourceRange> subres_range) {
    BeforeImageAccess();
    if (scene_targets) scene_targets->NativeAccess(*image, dst_access);
    const size_t prev_num_barriers = static_cast<size_t>(image_barriers.size());
    image->GetBarriers(image_barriers, dst_layout, dst_access, dst_stage, subres_range);
    return image_barriers.size() != prev_num_barriers;
}

Runtime::TransferMark::TransferMark(Runtime& runtime, const char* what,
                                    const VideoCore::Image& image) {
    profiler = GpuProfiler::Get();
    if (!profiler || !profiler->Records(&runtime.scheduler)) {
        profiler = nullptr;
        return;
    }
    resume = profiler->Current();
    const auto& info = image.info;
    const u64 key = (u64(reinterpret_cast<uintptr_t>(what)) << 20) ^
                    (u64(info.pixel_format) << 40) ^ (u64(info.size.width) << 16) ^
                    info.size.height ^ 0x7A5Full;
    profiler->Mark(key, [&] {
        return fmt::format("{} {} {}x{}", what, vk::to_string(info.pixel_format),
                           info.size.width, info.size.height);
    });
}

Runtime::TransferMark::~TransferMark() {
    if (profiler && resume) {
        profiler->Resume(resume);
    }
}

void Runtime::UploadImage(VideoCore::Image* dst, const VideoCore::Buffer* src,
                          std::span<const vk::BufferImageCopy> upload_copies) {
    SetBackingSamples(dst, dst->info.num_samples, false);
    scheduler.EndRendering();
    const TransferMark mark{*this, "image upload", *dst};

    bool needs_flush =
        Transit(dst, vk::ImageLayout::eTransferDstOptimal, vk::PipelineStageFlagBits2::eCopy,
                vk::AccessFlagBits2::eTransferWrite);
    for (const auto& copy : upload_copies) {
        const auto copy_size = BufferImageCopySize(copy, dst->info.pixel_format);
        needs_flush |= IsBufferAccessed(src, copy.bufferOffset, copy_size);
    }
    if (needs_flush) {
        FlushBarriers();
    }

    ForEachRecordPart(upload_copies, [&](std::span<const vk::BufferImageCopy> part) {
        scheduler.Record([src_handle = src->Handle(), image = dst->GetImage(),
                          regions = scheduler.RecordData(part)](vk::CommandBuffer cmdbuf) {
            cmdbuf.copyBufferToImage(src_handle, image, vk::ImageLayout::eTransferDstOptimal,
                                     regions.size(), regions.data());
        });
    });

    for (const auto& copy : upload_copies) {
        const auto copy_size = BufferImageCopySize(copy, dst->info.pixel_format);
        AccessBuffer(src, copy.bufferOffset, copy_size, vk::PipelineStageFlagBits2::eCopy,
                     vk::AccessFlagBits2::eTransferRead);
    }

    dst->flags &= ~VideoCore::ImageFlagBits::Dirty;
}

void Runtime::DownloadImage(VideoCore::Image* src, const VideoCore::Buffer* dst,
                            std::span<const vk::BufferImageCopy> download_copies) {
    SetBackingSamples(src, src->info.num_samples);
    scheduler.EndRendering();
    const TransferMark mark{*this, "image download", *src};

    bool needs_flush =
        Transit(src, vk::ImageLayout::eTransferSrcOptimal, vk::PipelineStageFlagBits2::eCopy,
                vk::AccessFlagBits2::eTransferRead);
    for (const auto& copy : download_copies) {
        const auto copy_size = BufferImageCopySize(copy, src->info.pixel_format);
        needs_flush |= IsBufferAccessed(dst, copy.bufferOffset, copy_size, true);
    }
    if (needs_flush) {
        FlushBarriers();
    }

    ForEachRecordPart(download_copies, [&](std::span<const vk::BufferImageCopy> part) {
        scheduler.Record([image = src->GetImage(), buffer = dst->Handle(),
                          regions = scheduler.RecordData(part)](vk::CommandBuffer cmdbuf) {
            cmdbuf.copyImageToBuffer(image, vk::ImageLayout::eTransferSrcOptimal, buffer,
                                     regions.size(), regions.data());
        });
    });

    for (const auto& copy : download_copies) {
        const auto copy_size = BufferImageCopySize(copy, src->info.pixel_format);
        AccessBuffer(dst, copy.bufferOffset, copy_size, vk::PipelineStageFlagBits2::eCopy,
                     vk::AccessFlagBits2::eTransferWrite);
    }
}

void Runtime::CopyImage(VideoCore::Image* src, VideoCore::Image* dst) {
    const u32 num_mips = std::min(src->info.resources.levels, dst->info.resources.levels);

    // Format mismatch warning (safe but useful)
    if (src->info.pixel_format != dst->info.pixel_format) {
        LOG_DEBUG(Render_Vulkan,
                  "Copy between different formats: src={}, dst={}. "
                  "Result may be undefined.",
                  vk::to_string(src->info.pixel_format), vk::to_string(dst->info.pixel_format));
    }

    const u32 base_width = src->info.size.width;
    const u32 base_height = src->info.size.height;
    const u32 base_depth =
        dst->info.type == AmdGpu::ImageType::Color3D ? dst->info.size.depth : src->info.size.depth;

    // Match sample count before copying
    SetBackingSamples(dst, dst->info.num_samples, false);
    SetBackingSamples(src, src->info.num_samples);

    boost::container::small_vector<vk::ImageCopy, 8> regions;

    const vk::ImageAspectFlags src_aspect = src->aspect_mask & ~vk::ImageAspectFlagBits::eStencil;
    const vk::ImageAspectFlags dst_aspect = dst->aspect_mask & ~vk::ImageAspectFlagBits::eStencil;

    const bool src_is_2d = ConvertImageType(src->info.type) == vk::ImageType::e2D;
    const bool src_is_3d = ConvertImageType(src->info.type) == vk::ImageType::e3D;

    const bool dst_is_2d = ConvertImageType(dst->info.type) == vk::ImageType::e2D;
    const bool dst_is_3d = ConvertImageType(dst->info.type) == vk::ImageType::e3D;

    const bool is_2d_to_3d = src_is_2d && dst_is_3d;
    const bool is_3d_to_2d = src_is_3d && dst_is_2d;
    const bool is_same_type = !is_2d_to_3d && !is_3d_to_2d;

    for (u32 mip = 0; mip < num_mips; ++mip) {
        const u32 mip_w = std::max(base_width >> mip, 1u);
        const u32 mip_h = std::max(base_height >> mip, 1u);
        const u32 mip_d = std::max(base_depth >> mip, 1u);

        const auto [src_layers, dst_layers] = SanitizeCopyLayers(src->info, dst->info, mip_d);

        vk::ImageCopy region{};
        region.srcSubresource.aspectMask = src_aspect;
        region.srcSubresource.mipLevel = mip;
        region.srcSubresource.baseArrayLayer = 0;
        region.dstSubresource.aspectMask = dst_aspect;
        region.dstSubresource.mipLevel = mip;
        region.dstSubresource.baseArrayLayer = 0;

        if (is_same_type) {
            // 2D->2D OR 3D->3D
            if (src_is_3d) {
                // 3D images must use layerCount=1
                region.srcSubresource.layerCount = 1;
                region.dstSubresource.layerCount = 1;
                region.extent = vk::Extent3D(mip_w, mip_h, mip_d);
            } else {
                // Array images
                const u32 copy_layers = std::min(src_layers, dst_layers);
                region.srcSubresource.layerCount = copy_layers;
                region.dstSubresource.layerCount = copy_layers;
                region.extent = vk::Extent3D(mip_w, mip_h, 1);
            }
        } else if (is_2d_to_3d) {
            // 2D array -> 3D volume
            region.srcSubresource.layerCount = src_layers;
            region.dstSubresource.layerCount = 1;
            region.extent = vk::Extent3D(mip_w, mip_h, src_layers);
        } else if (is_3d_to_2d) {
            // 3D volume -> 2D array
            region.srcSubresource.layerCount = 1;
            region.dstSubresource.layerCount = dst_layers;
            region.extent = vk::Extent3D(mip_w, mip_h, dst_layers);
        }

        regions.push_back(region);
    }

    scheduler.EndRendering();

    bool needs_flush =
        Transit(src, vk::ImageLayout::eTransferSrcOptimal, vk::PipelineStageFlagBits2::eCopy,
                vk::AccessFlagBits2::eTransferRead);
    needs_flush |= Transit(dst, vk::ImageLayout::eTransferDstOptimal,
                           vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite);
    if (needs_flush) {
        FlushBarriers();
    }

    scheduler.Record([src_image = src->GetImage(), dst_image = dst->GetImage(),
                      regions](vk::CommandBuffer cmdbuf) {
        cmdbuf.copyImage(src_image, vk::ImageLayout::eTransferSrcOptimal, dst_image,
                         vk::ImageLayout::eTransferDstOptimal, regions);
    });

    dst->flags |= (src->flags & VideoCore::ImageFlagBits::GpuModified);
    dst->flags &= ~VideoCore::ImageFlagBits::Dirty;
}

void Runtime::CopyImageWithBuffer(VideoCore::Image* src, VideoCore::Image* dst,
                                  const VideoCore::Buffer* buffer, u64 offset) {
    const u32 num_mips = std::min(src->info.resources.levels, dst->info.resources.levels);
    const u32 num_layers = std::min(src->info.resources.layers, dst->info.resources.layers);
    ASSERT(src->info.resources.layers == dst->info.resources.layers && num_mips == 1);

    SetBackingSamples(dst, dst->info.num_samples, false);
    SetBackingSamples(src, src->info.num_samples);

    vk::BufferImageCopy buffer_copy = {
        .bufferOffset = offset,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource{
            .aspectMask = src->aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
            .mipLevel = 0u,
            .baseArrayLayer = 0,
            .layerCount = num_layers,
        },
        .imageOffset = {0, 0, 0},
        .imageExtent = {src->info.size.width, src->info.size.height, src->info.size.depth},
    };
    const auto copy_size = BufferImageCopySize(buffer_copy, src->info.pixel_format);

    scheduler.EndRendering();

    bool needs_flush =
        Transit(src, vk::ImageLayout::eTransferSrcOptimal, vk::PipelineStageFlagBits2::eCopy,
                vk::AccessFlagBits2::eTransferRead);
    needs_flush |= Transit(dst, vk::ImageLayout::eTransferDstOptimal,
                           vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite);
    needs_flush |= IsBufferAccessed(buffer, offset, copy_size);
    if (needs_flush) {
        FlushBarriers();
    }

    const auto download_copy = buffer_copy;
    buffer_copy.imageSubresource.aspectMask = dst->aspect_mask & ~vk::ImageAspectFlagBits::eStencil;
    scheduler.Record([src_image = src->GetImage(), dst_image = dst->GetImage(),
                      handle = buffer->Handle(), download_copy,
                      upload_copy = buffer_copy](vk::CommandBuffer cmdbuf) {
        cmdbuf.copyImageToBuffer(src_image, vk::ImageLayout::eTransferSrcOptimal, handle,
                                 download_copy);
        const vk::MemoryBarrier2 post_copy_barrier = {
            .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .dependencyFlags = vk::DependencyFlagBits::eByRegion,
            .memoryBarrierCount = 1u,
            .pMemoryBarriers = &post_copy_barrier,
        });
        cmdbuf.copyBufferToImage(handle, dst_image, vk::ImageLayout::eTransferDstOptimal,
                                 upload_copy);
    });

    dst->flags |= (src->flags & VideoCore::ImageFlagBits::GpuModified);
    dst->flags &= ~VideoCore::ImageFlagBits::Dirty;
}

void Runtime::CopyMip(VideoCore::Image* src, VideoCore::Image* dst, u32 mip, u32 slice) {
    const auto dst_dim = dst->info.props.is_block ? 2 : 0;
    const auto mip_block_w = std::max(dst->info.size.width >> (mip + dst_dim), 1u);
    const auto mip_block_h = std::max(dst->info.size.height >> (mip + dst_dim), 1u);
    const auto mip_block_p = std::max(dst->info.mips_layout[mip].pitch >> dst_dim, 1u);

    const auto src_dim = src->info.props.is_block ? 2 : 0;
    ASSERT(mip_block_w == (src->info.size.width >> src_dim));
    ASSERT(mip_block_h == (src->info.size.height >> src_dim));
    ASSERT(mip_block_p == (src->info.pitch >> src_dim));

    const auto [src_layers, dst_layers] =
        SanitizeCopyLayers(src->info, dst->info, src->info.size.depth);

    const vk::ImageCopy image_copy{
        .srcSubresource{
            .aspectMask = src->aspect_mask,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = src_layers,
        },
        .dstSubresource{
            .aspectMask = src->aspect_mask,
            .mipLevel = mip,
            .baseArrayLayer = dst->info.props.is_volume ? 0 : slice,
            .layerCount = dst_layers,
        },
        // bbport: a volume's slice is a depth offset (3D images have one layer).
        .dstOffset = {0, 0, dst->info.props.is_volume ? static_cast<s32>(slice) : 0},
        .extent = {src->info.size.width, src->info.size.height, src->info.size.depth},
    };

    SetBackingSamples(dst, dst->info.num_samples);
    SetBackingSamples(src, src->info.num_samples);

    scheduler.EndRendering();

    bool needs_flush =
        Transit(src, vk::ImageLayout::eTransferSrcOptimal, vk::PipelineStageFlagBits2::eCopy,
                vk::AccessFlagBits2::eTransferRead);
    needs_flush |= Transit(dst, vk::ImageLayout::eTransferDstOptimal,
                           vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite);
    if (needs_flush) {
        FlushBarriers();
    }

    scheduler.Record([src_image = src->GetImage(), dst_image = dst->GetImage(),
                      image_copy](vk::CommandBuffer cmdbuf) {
        cmdbuf.copyImage(src_image, vk::ImageLayout::eTransferSrcOptimal, dst_image,
                         vk::ImageLayout::eTransferDstOptimal, image_copy);
    });

    dst->flags |= (src->flags & VideoCore::ImageFlagBits::GpuModified);
    dst->flags &= ~VideoCore::ImageFlagBits::Dirty;
}

void Runtime::CopyColorAndDepth(VideoCore::Image* src, VideoCore::Image* dst) {
    if (src->info.num_samples == 1 && dst->info.num_samples == 1) {
        if (instance.IsMaintenance8Supported() ||
            src->info.props.is_depth == dst->info.props.is_depth) {
            CopyImage(src, dst);
        } else {
            // Perform depth from/to color copy using the intermediate copy buffer.
            static constexpr size_t COPY_BUFFER_SIZE = 128_MB;
            const auto copy_ref =
                staging_pool.Request(COPY_BUFFER_SIZE, VideoCore::MemoryType::DeviceLocal);
            CopyImageWithBuffer(src, dst, copy_ref.buffer, copy_ref.offset);
        }
    } else if (src->info.num_samples == 1 && dst->info.num_samples > 1 &&
               dst->info.props.is_depth) {
        // Perform a rendering pass to transfer the channels of source as samples in dest.
        bool needs_flush =
            Transit(src, vk::ImageLayout::eShaderReadOnlyOptimal,
                    vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderRead);
        needs_flush |= Transit(dst, vk::ImageLayout::eDepthStencilAttachmentOptimal,
                               vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                                   vk::PipelineStageFlagBits2::eLateFragmentTests,
                               vk::AccessFlagBits2::eDepthStencilAttachmentWrite);
        if (needs_flush) {
            FlushBarriers();
        }

        blit_helper->ReinterpretColorAsMsDepth(
            dst->info.size.width, dst->info.size.height, dst->info.num_samples,
            src->info.pixel_format, dst->info.pixel_format, src->GetImage(), dst->GetImage());
    } else {
        LOG_WARNING(Render_Vulkan, "Unimplemented depth overlap copy");
    }
}

void Runtime::CopyDepthStencil(VideoCore::Image* src, VideoCore::Image* dst,
                               const VideoCore::SubresourceRange& sub_range) {
    scheduler.EndRendering();

    bool needs_flush =
        Transit(src, vk::ImageLayout::eTransferSrcOptimal, vk::PipelineStageFlagBits2::eCopy,
                vk::AccessFlagBits2::eTransferRead);
    needs_flush |= Transit(dst, vk::ImageLayout::eTransferDstOptimal,
                           vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite);
    if (needs_flush) {
        FlushBarriers();
    }

    const auto aspect_mask = src->aspect_mask & dst->aspect_mask;

    const vk::ImageCopy region = {
        .srcSubresource{
            .aspectMask = aspect_mask,
            .mipLevel = 0,
            .baseArrayLayer = sub_range.base.layer,
            .layerCount = sub_range.extent.layers,
        },
        .srcOffset = {0, 0, 0},
        .dstSubresource{
            .aspectMask = aspect_mask,
            .mipLevel = 0,
            .baseArrayLayer = sub_range.base.layer,
            .layerCount = sub_range.extent.layers,
        },
        .dstOffset = {0, 0, 0},
        .extent = {dst->info.size.width, dst->info.size.height, 1},
    };

    scheduler.Record([src_image = src->GetImage(), dst_image = dst->GetImage(),
                      region](vk::CommandBuffer cmdbuf) {
        cmdbuf.copyImage(src_image, vk::ImageLayout::eTransferSrcOptimal, dst_image,
                         vk::ImageLayout::eTransferDstOptimal, region);
    });

    dst->flags |= VideoCore::ImageFlagBits::GpuModified;
    dst->flags &= ~VideoCore::ImageFlagBits::Dirty;
}

void Runtime::ResolveImage(VideoCore::Image* src, VideoCore::Image* dst,
                           const VideoCore::SubresourceRange& src_range,
                           const VideoCore::SubresourceRange& dst_range) {
    SetBackingSamples(dst, 1, false);
    scheduler.EndRendering();

    const bool needs_resolve = src->backing->num_samples != 1;
    const auto dst_stage =
        needs_resolve ? vk::PipelineStageFlagBits2::eResolve : vk::PipelineStageFlagBits2::eCopy;

    bool needs_flush = Transit(src, vk::ImageLayout::eTransferSrcOptimal, dst_stage,
                               vk::AccessFlagBits2::eTransferRead);
    needs_flush |= Transit(dst, vk::ImageLayout::eTransferDstOptimal, dst_stage,
                           vk::AccessFlagBits2::eTransferWrite);
    if (needs_flush) {
        FlushBarriers();
    }

    // bbport: a volume's slices are its depth (one array layer, offset z), not layers, which
    // Vulkan does not allow on 3D images. Layer counts come from the ranges, not the whole
    // images, so a range starting past layer 0 stays inside them.
    const bool src_3d = src->info.props.is_volume;
    const bool dst_3d = dst->info.props.is_volume;
    const auto [src_first, src_count] =
        src_3d ? VolumeSlices(src->info, src_range)
               : std::pair{src_range.base.layer, src_range.extent.layers};
    const auto [dst_first, dst_count] =
        dst_3d ? VolumeSlices(dst->info, dst_range)
               : std::pair{dst_range.base.layer, dst_range.extent.layers};
    const u32 count = std::min(src_count, dst_count);
    const auto subresource = [](bool is_3d, u32 first, u32 n) {
        return vk::ImageSubresourceLayers{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .mipLevel = 0,
            .baseArrayLayer = is_3d ? 0 : first,
            .layerCount = is_3d ? 1 : n,
        };
    };
    const auto offset = [](bool is_3d, u32 first) {
        return vk::Offset3D{0, 0, is_3d ? static_cast<s32>(first) : 0};
    };
    const u32 width = std::min(src->info.size.width, dst->info.size.width);
    const u32 height = std::min(src->info.size.height, dst->info.size.height);
    if (!needs_resolve) {
        const vk::ImageCopy region = {
            .srcSubresource = subresource(src_3d, src_first, count),
            .srcOffset = offset(src_3d, src_first),
            .dstSubresource = subresource(dst_3d, dst_first, count),
            .dstOffset = offset(dst_3d, dst_first),
            .extent = {width, height, (src_3d || dst_3d) ? count : 1u},
        };
        scheduler.Record([src_image = src->GetImage(), dst_image = dst->GetImage(),
                          region](vk::CommandBuffer cmdbuf) {
            cmdbuf.copyImage(src_image, vk::ImageLayout::eTransferSrcOptimal, dst_image,
                             vk::ImageLayout::eTransferDstOptimal, region);
        });
    } else {
        // A multisampled source is 2D; into a volume, resolve one slice per region.
        boost::container::small_vector<vk::ImageResolve, 8> regions;
        const u32 steps = dst_3d ? count : 1u;
        for (u32 i = 0; i < steps; ++i) {
            const u32 n = dst_3d ? 1u : count;
            regions.push_back(vk::ImageResolve{
                .srcSubresource = subresource(false, src_first + i, n),
                .srcOffset = {0, 0, 0},
                .dstSubresource = subresource(dst_3d, dst_first + i, n),
                .dstOffset = offset(dst_3d, dst_first + i),
                .extent = {width, height, 1},
            });
        }
        scheduler.Record([src_image = src->GetImage(), dst_image = dst->GetImage(),
                          regions](vk::CommandBuffer cmdbuf) {
            cmdbuf.resolveImage(src_image, vk::ImageLayout::eTransferSrcOptimal, dst_image,
                                vk::ImageLayout::eTransferDstOptimal, regions);
        });
    }

    dst->flags |= VideoCore::ImageFlagBits::GpuModified;
    dst->flags &= ~VideoCore::ImageFlagBits::Dirty;
}

void Runtime::ClearImage(VideoCore::Image* dst, const VideoCore::SubresourceRange& range,
                         const vk::ClearValue& clear_value) {
    scheduler.EndRendering();

    if (dst->info.props.is_volume) {
        ClearVolumeSlices(dst, range, clear_value);
        return;
    }

    const bool needs_flush =
        Transit(dst, vk::ImageLayout::eTransferDstOptimal, vk::PipelineStageFlagBits2::eClear,
                vk::AccessFlagBits2::eTransferWrite, range);
    if (needs_flush) {
        FlushBarriers();
    }

    // bbport: a depth (and stencil) image takes clear_value.depthStencil.
    const bool is_depth = dst->info.props.is_depth;
    const vk::ImageSubresourceRange vk_range = {
        .aspectMask = is_depth ? dst->aspect_mask : vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = range.base.level,
        .levelCount = range.extent.levels,
        .baseArrayLayer = range.base.layer,
        .layerCount = range.extent.layers,
    };
    if (is_depth) {
        scheduler.Record([image = dst->GetImage(), depth_stencil = clear_value.depthStencil,
                          vk_range](vk::CommandBuffer cmdbuf) {
            cmdbuf.clearDepthStencilImage(image, vk::ImageLayout::eTransferDstOptimal,
                                          depth_stencil, vk_range);
        });
    } else {
        scheduler.Record([image = dst->GetImage(), color = clear_value.color,
                          vk_range](vk::CommandBuffer cmdbuf) {
            cmdbuf.clearColorImage(image, vk::ImageLayout::eTransferDstOptimal, color, vk_range);
        });
    }

    dst->flags |= VideoCore::ImageFlagBits::GpuModified;
    dst->flags &= ~VideoCore::ImageFlagBits::Dirty;
}

// bbport: a range of a volume names depth slices as layers (render targets of one slice). A
// 3D image has one array layer, so vkCmdClearColorImage can only clear all of its slices; a
// part is cleared by a render pass over a 2D array view of those slices (the image is
// 2D-array compatible).
void Runtime::ClearVolumeSlices(VideoCore::Image* dst, const VideoCore::SubresourceRange& range,
                                const vk::ClearValue& clear_value) {
    const auto [first, count] = VolumeSlices(dst->info, range);
    const u32 level = range.base.level;
    const u32 depth = std::max(dst->info.size.depth >> level, 1u);
    if (first == 0 && count >= depth) {
        const bool needs_flush = Transit(dst, vk::ImageLayout::eTransferDstOptimal,
                                         vk::PipelineStageFlagBits2::eClear,
                                         vk::AccessFlagBits2::eTransferWrite, range);
        if (needs_flush) {
            FlushBarriers();
        }
        const vk::ImageSubresourceRange vk_range = {
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = level,
            .levelCount = range.extent.levels,
            .baseArrayLayer = 0,
            .layerCount = 1,
        };
        scheduler.Record([image = dst->GetImage(), color = clear_value.color,
                          vk_range](vk::CommandBuffer cmdbuf) {
            cmdbuf.clearColorImage(image, vk::ImageLayout::eTransferDstOptimal, color, vk_range);
        });
    } else {
        VideoCore::ImageViewInfo view_info;
        view_info.type =
            count > 1 ? AmdGpu::ImageType::Color2DArray : AmdGpu::ImageType::Color2D;
        view_info.format = dst->info.pixel_format;
        view_info.range.base = {.level = level, .layer = first};
        view_info.range.extent = {.levels = 1, .layers = count};
        const vk::ImageView view = *dst->FindView(view_info).image_view;
        const bool needs_flush =
            Transit(dst, vk::ImageLayout::eColorAttachmentOptimal,
                    vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                    vk::AccessFlagBits2::eColorAttachmentWrite, range);
        if (needs_flush) {
            FlushBarriers();
        }
        const vk::Extent2D extent{std::max(dst->info.size.width >> level, 1u),
                                  std::max(dst->info.size.height >> level, 1u)};
        scheduler.Record([view, extent, count, color = clear_value.color](vk::CommandBuffer cmdbuf) {
            const vk::RenderingAttachmentInfo attachment = {
                .imageView = view,
                .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .loadOp = vk::AttachmentLoadOp::eClear,
                .storeOp = vk::AttachmentStoreOp::eStore,
                .clearValue = vk::ClearValue{.color = color},
            };
            cmdbuf.beginRendering(vk::RenderingInfo{
                .renderArea = {{0, 0}, extent},
                .layerCount = count,
                .colorAttachmentCount = 1,
                .pColorAttachments = &attachment,
            });
            cmdbuf.endRendering();
        });
    }

    dst->flags |= VideoCore::ImageFlagBits::GpuModified;
    dst->flags &= ~VideoCore::ImageFlagBits::Dirty;
}

u64 Runtime::DumpImage(VideoCore::Image* image, u32 layer, const std::string& path) {
    const auto& info = image->info;
    if (!image->backing || image->backing->num_samples != 1 ||
        info.pixel_format == vk::Format::eUndefined) {
        return 0;
    }
    const bool depth = static_cast<bool>(image->aspect_mask & vk::ImageAspectFlagBits::eDepth);
    const vk::Format format = info.pixel_format;
    u32 texel = 0;
    if (depth) {
        texel = format == vk::Format::eD16Unorm || format == vk::Format::eD16UnormS8Uint ? 2 : 4;
    } else if (vk::blockExtent(format)[0] == 1) {
        texel = vk::blockSize(format);
    }
    if (texel == 0) {
        return 0; // compressed
    }
    const u32 width = info.size.width, height = info.size.height;
    const u64 size = u64(width) * height * texel;
    const VkBufferCreateInfo buffer_ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    };
    const VmaAllocationCreateInfo alloc_ci{
        .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
    };
    VkBuffer buffer{};
    VmaAllocation allocation{};
    VmaAllocationInfo alloc_info{};
    if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &buffer, &allocation,
                        &alloc_info) != VK_SUCCESS) {
        return 0;
    }
    scheduler.EndRendering();
    if (Transit(image, vk::ImageLayout::eTransferSrcOptimal, vk::PipelineStageFlagBits2::eCopy,
                vk::AccessFlagBits2::eTransferRead)) {
        FlushBarriers();
    }
    const bool volume = info.props.is_volume;
    const u32 slice = std::min(layer, (volume ? info.size.depth : info.resources.layers) - 1);
    const vk::BufferImageCopy region{
        .imageSubresource = {depth ? vk::ImageAspectFlagBits::eDepth
                                   : vk::ImageAspectFlagBits::eColor,
                             0, volume ? 0 : slice, 1},
        .imageOffset = {0, 0, volume ? s32(slice) : 0},
        .imageExtent = {width, height, 1},
    };
    scheduler.Record([src = image->GetImage(), dst = vk::Buffer{buffer},
                      region](vk::CommandBuffer cmdbuf) {
        cmdbuf.copyImageToBuffer(src, vk::ImageLayout::eTransferSrcOptimal, dst, region);
    });
    scheduler.DeferPriorityOperation(
        [allocator = instance.GetAllocator(), buffer, allocation, alloc_info, size, path] {
            vmaInvalidateAllocation(allocator, allocation, 0, VK_WHOLE_SIZE);
            if (FILE* f = std::fopen(path.c_str(), "wb")) {
                std::fwrite(alloc_info.pMappedData, 1, size, f);
                std::fclose(f);
            }
            vmaDestroyBuffer(allocator, buffer, allocation);
        });
    return size;
}

void Runtime::SetBackingSamples(VideoCore::Image* image, u32 num_samples, bool copy_backing) {
    BeforeImageAccess();
    auto& backing = image->backing;
    auto& backing_images = image->backing_images;
    const auto& info = image->info;
    if (!backing || backing->num_samples == num_samples) {
        return;
    }
    ASSERT_MSG(!image->info.props.is_depth, "Swapping samples is only valid for color images");
    VideoCore::Image::BackingImage* new_backing;
    auto it = std::ranges::find(backing_images, num_samples,
                                &VideoCore::Image::BackingImage::num_samples);
    if (it == backing_images.end()) {
        auto new_image_ci = backing->image.image_ci;
        new_image_ci.samples = LiverpoolToVK::NumSamples(num_samples, image->supported_samples);

        new_backing = &backing_images.emplace_back();
        new_backing->num_samples = num_samples;
        new_backing->image = VideoCore::UniqueImage{instance.GetDevice(), instance.GetAllocator()};
        new_backing->image.Create(new_image_ci);

        Vulkan::SetObjectName(instance.GetDevice(), new_backing->image.image,
                              "Image {}x{}x{} {} {} {:#x}:{:#x} L:{} M:{} S:{} (backing)",
                              info.size.width, info.size.height, info.size.depth,
                              AmdGpu::NameOf(info.tile_mode), vk::to_string(info.pixel_format),
                              info.guest_address, info.guest_size, info.resources.layers,
                              info.resources.levels, num_samples);
    } else {
        new_backing = std::addressof(*it);
    }

    if (copy_backing) {
        scheduler.EndRendering();
        ASSERT(image->info.resources.levels == 1 && image->info.resources.layers == 1);

        // Transition current backing to shader read layout
        Transit(image, vk::ImageLayout::eShaderReadOnlyOptimal,
                vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderRead);

        // Transition dest backing to color attachment layout, not caring of previous contents
        constexpr auto dst_stage = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
        constexpr auto dst_access =
            vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite;
        constexpr auto dst_layout = vk::ImageLayout::eColorAttachmentOptimal;
        image_barriers.push_back(vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_access,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = dst_layout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = new_backing->image,
            .subresourceRange{
                .aspectMask = image->aspect_mask,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = image->info.resources.layers,
            },
        });
        FlushBarriers();

        // Copy between ms and non ms backing images
        blit_helper->CopyBetweenMsImages(
            info.size.width, info.size.height, new_backing->num_samples, info.pixel_format,
            backing->num_samples > 1, backing->image, new_backing->image);

        // Update current layout in tracker to new backings layout
        new_backing->state.layout = dst_layout;
        new_backing->state.access_mask = dst_access;
        new_backing->state.pl_stage = dst_stage;
    }

    backing = new_backing;
}

bool Runtime::IsBufferAccessed(const VideoCore::Buffer* handle, u64 offset, u64 size,
                               bool check_read_access) {
    const AddressRange range = {
        .resource = reinterpret_cast<u64>(handle),
        .range_start = offset,
        .range_end = offset + size - 1,
    };
    if (global_write) {
        return true; // bbport: a paged write may have touched it (AccessGlobal)
    }
    bool has_access = barrier_tracker.FindRange(range, Access::Write);
    if (check_read_access && !has_access) {
        has_access |= barrier_tracker.FindRange(range, Access::Read);
    }
    return has_access;
}

void Runtime::AccessBuffer(const VideoCore::Buffer* handle, u64 offset, u64 size,
                           vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access) {
    AddressRange range = {
        .resource = reinterpret_cast<u64>(handle),
        .range_start = offset,
        .range_end = offset + size - 1,
    };

    constexpr static vk::AccessFlags2 READ_MASK =
        vk::AccessFlagBits2::eIndexRead | vk::AccessFlagBits2::eVertexAttributeRead |
        vk::AccessFlagBits2::eUniformRead | vk::AccessFlagBits2::eShaderRead |
        vk::AccessFlagBits2::eColorAttachmentRead |
        vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eTransferRead |
        vk::AccessFlagBits2::eMemoryRead;

    constexpr static vk::AccessFlags2 WRITE_MASK =
        vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eColorAttachmentWrite |
        vk::AccessFlagBits2::eDepthStencilAttachmentWrite | vk::AccessFlagBits2::eTransferWrite |
        vk::AccessFlagBits2::eMemoryWrite | vk::AccessFlagBits2::eTransformFeedbackWriteEXT;

    // bbport: reads are tracked at 4 KiB granularity. Constant data comes from ring allocations
    // at a new offset every draw; rounded, they land in ranges already present and the
    // insert returns early instead of growing the tree until the next barrier flush.
    // Wider read ranges only add barriers, never drop one.
    if (!(src_access & WRITE_MASK) && !BbToggle::Disabled(BbToggle::CoarseReadTracking)) {
        range.range_start &= ~u64{0xFFF};
        range.range_end |= 0xFFF;
    }
    const auto insert = [&](Access access) {
        if (BbToggle::Disabled(BbToggle::AccessMemo)) {
            barrier_tracker.InsertRange(range, access);
            return;
        }
        u64 hash = range.resource ^ range.range_start * 0x9E3779B97F4A7C15ull ^
                   range.range_end << 13 ^ u64(access) << 61;
        hash = (hash ^ hash >> 31) * 0xFF51AFD7ED558CCDull;
        auto& memo = access_memo[(hash ^ hash >> 29) % access_memo.size()];
        if (memo.epoch == access_epoch && memo.resource == range.resource &&
            memo.start == range.range_start && memo.end == range.range_end &&
            memo.access == u32(access)) {
            return;
        }
        barrier_tracker.InsertRange(range, access);
        memo = {range.resource, range.range_start, range.range_end, access_epoch, u32(access)};
    };
    if (src_access & WRITE_MASK) {
        insert(Access::Write);
    }
    if (src_access & READ_MASK) {
        insert(Access::Read);
    }

    memory_barrier.srcStageMask |= src_stage;
    memory_barrier.srcAccessMask |= src_access & WRITE_MASK;
}

void Runtime::AccessGlobal(vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access) {
    memory_barrier.srcStageMask |= src_stage;
    if (src_access & vk::AccessFlagBits2::eShaderWrite) {
        memory_barrier.srcAccessMask |= vk::AccessFlagBits2::eShaderWrite;
        global_write = true;
    }
}

void Runtime::FlushBarriers() {
    BeforeImageAccess();
    vk::DependencyInfo dep_info{};

    if (memory_barrier.srcStageMask) {
        dep_info.pMemoryBarriers = &memory_barrier;
        dep_info.memoryBarrierCount = 1U;
    }
    if (!image_barriers.empty()) {
        dep_info.pImageMemoryBarriers = image_barriers.data();
        dep_info.imageMemoryBarrierCount = static_cast<u32>(image_barriers.size());
    }

    if (!dep_info.memoryBarrierCount && !dep_info.imageMemoryBarrierCount) {
        return;
    }

    scheduler.EndRendering();
    // bbport: thousands of image barriers at once (Ratchet & Clank's large texture sets) do not
    // fit a recording chunk: recorded on this thread's command buffer instead.
    if (scheduler.IsRecordingDeferred() &&
        image_barriers.size() * sizeof(vk::ImageMemoryBarrier2) > 64 * 1024) {
        scheduler.CommandBuffer().pipelineBarrier2(dep_info);
    } else
    scheduler.Record([memory = memory_barrier, has_memory = dep_info.memoryBarrierCount != 0,
                      images = scheduler.RecordData(std::span<const vk::ImageMemoryBarrier2>(
                          image_barriers.data(), image_barriers.size()))](vk::CommandBuffer cmdbuf) {
        const vk::DependencyInfo info = {
            .memoryBarrierCount = has_memory ? 1U : 0U,
            .pMemoryBarriers = has_memory ? &memory : nullptr,
            .imageMemoryBarrierCount = static_cast<u32>(images.size()),
            .pImageMemoryBarriers = images.data(),
        };
        cmdbuf.pipelineBarrier2(info);
    });

    memory_barrier.srcStageMask = vk::PipelineStageFlagBits2::eNone;
    memory_barrier.srcAccessMask = vk::AccessFlagBits2::eNone;

    image_barriers.clear();
    barrier_tracker.Clear();
    global_write = false;
    ++access_epoch;
}

} // namespace Vulkan
