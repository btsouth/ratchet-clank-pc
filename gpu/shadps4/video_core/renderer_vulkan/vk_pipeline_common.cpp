// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <cstdlib>
#include <span>

#include <boost/container/small_vector.hpp>
#include <boost/container/static_vector.hpp>

#include "shader_recompiler/resource.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

#include "bbport_toggles.h"

namespace Vulkan {

Pipeline::Pipeline(const Instance& instance_, Scheduler& scheduler_, DescriptorHeap& desc_heap_,
                   const Shader::Profile& profile_, vk::PipelineCache pipeline_cache,
                   bool is_compute_ /*= false*/)
    : instance{instance_}, scheduler{scheduler_}, desc_heap{desc_heap_}, profile{profile_},
      is_compute{is_compute_} {}

Pipeline::~Pipeline() = default;

std::atomic<u64> g_bb_pipeline_wait_ns;
std::atomic<u64> g_bb_compile_stall_ns;
std::atomic<u64> g_bb_pipeline_skipped_draws;

namespace {
u64 EnvNumber(const char* name, u64 fallback) {
    const char* env = std::getenv(name);
    return env && *env ? std::strtoull(env, nullptr, 10) : fallback;
}

/// The time draws of the current shown frame (BbStats::frame_number) spent waiting for
/// pipelines, on all recording threads together.
struct DrawWaitBudget {
    std::atomic<u32> frame{~0u};
    std::atomic<u64> used_ns{0};

    u64 Used(u32 shown) {
        u32 seen = frame.load(std::memory_order_relaxed);
        if (seen != shown && frame.compare_exchange_strong(seen, shown)) {
            used_ns.store(0, std::memory_order_relaxed);
        }
        return used_ns.load(std::memory_order_relaxed);
    }
};
DrawWaitBudget draw_wait_budget;
} // namespace

vk::Pipeline Pipeline::HandleForDrawSlow(bool recurring) const {
    // Geometry of a pass drawn every frame shows a frame later; a one-time render or a
    // full-screen pass that goes without leaves a hole or a grey frame, so those wait longer.
    // The longer budget keeps the frame's flip well inside LBP3's watchdog (5 waits of 100 ms
    // for the previous frame's flip), which a cold cache missed: ~100 pipelines in one frame
    // at 50-140 ms each on NVIDIA.
    static const u64 recurring_ns = EnvNumber("BB_PIPELINE_WAIT_MS", 8) * 1'000'000;
    static const u64 other_ns = EnvNumber("BB_PIPELINE_STALL_MS", 200) * 1'000'000;
    static const u64 max_frames = EnvNumber("BB_PIPELINE_SKIP_FRAMES", 60);
    const u32 shown = BbStats::frame_number.load(std::memory_order_relaxed);
    bool overdue;
    {
        std::scoped_lock lk{handle_mutex};
        if (!handle_pending.load(std::memory_order_acquire)) {
            return *pipeline;
        }
        if (missing_frame != shown) {
            missing_frame = shown;
            ++missing_frames;
        }
        overdue = missing_frames > max_frames;
    }
    if (overdue) {
        // Missing for too long: drawn now, built here if its job has not started.
        if (!BuildNow()) {
            WaitHandle();
        }
        return *pipeline;
    }
    const u64 budget = recurring ? recurring_ns : other_ns;
    const u64 used = draw_wait_budget.Used(shown);
    if (used >= budget) {
        g_bb_pipeline_skipped_draws.fetch_add(1, std::memory_order_relaxed);
        return {};
    }
    const auto start = std::chrono::steady_clock::now();
    const auto charge = [&] {
        const u64 ns = u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now() - start)
                               .count());
        draw_wait_budget.used_ns.fetch_add(ns, std::memory_order_relaxed);
        g_bb_pipeline_wait_ns += ns;
        g_bb_compile_stall_ns += ns;
    };
    // It may be queued behind a hundred others: the next free worker takes it.
    Expedite();
    std::unique_lock lk{handle_mutex};
    const bool ready =
        handle_cv.wait_for(lk, std::chrono::nanoseconds(budget - used),
                           [&] { return !handle_pending.load(std::memory_order_acquire); });
    lk.unlock();
    charge();
    if (!ready) {
        g_bb_pipeline_skipped_draws.fetch_add(1, std::memory_order_relaxed);
        return {};
    }
    return *pipeline;
}

void Pipeline::WaitHandle() const {
    const auto start = std::chrono::steady_clock::now();
    std::unique_lock lk{handle_mutex};
    if (!handle_pending.load(std::memory_order_acquire)) {
        return; // the destructor, after the build (the lock orders it after PublishHandle)
    }
    handle_cv.wait(lk, [&] { return !handle_pending.load(std::memory_order_acquire); });
    const u64 ns = u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count());
    g_bb_pipeline_wait_ns += ns;
    g_bb_compile_stall_ns += ns;
}

void Pipeline::PublishHandle() {
    // Notified under the lock: a waiter (or the destructor) may free this object once it sees
    // the handle published.
    std::scoped_lock lk{handle_mutex};
    handle_pending.store(false, std::memory_order_release);
    handle_cv.notify_all();
}


void Pipeline::BindResources(DescriptorWrites& set_writes, const Shader::PushData& push_data,
                             std::span<const vk::DescriptorImageInfo> image_infos,
                             std::span<const vk::DescriptorBufferInfo> buffer_infos) const {
    const auto bind_point =
        IsCompute() ? vk::PipelineBindPoint::eCompute : vk::PipelineBindPoint::eGraphics;
    const auto stage_flags = IsCompute() ? vk::ShaderStageFlagBits::eCompute : AllGraphicsStageBits;
    const vk::PipelineLayout layout = *pipeline_layout;
    scheduler.Record([layout, stage_flags, push_data](vk::CommandBuffer cmdbuf) {
        cmdbuf.pushConstants(layout, stage_flags, 0u, sizeof(push_data), &push_data);
    });

    // Bind descriptor set.
    if (set_writes.empty()) {
        return;
    }

    if (uses_push_descriptors) {
        // bbport: a set too large for one recording chunk (Ratchet & Clank binds sets of
        // thousands of descriptors) is pushed on this thread's command buffer, recording directly.
        size_t descriptors = 0;
        for (const auto& write : set_writes) {
            descriptors += write.descriptorCount;
        }
        if (scheduler.IsRecordingDeferred() &&
            set_writes.size() * sizeof(vk::WriteDescriptorSet) +
                    descriptors * (sizeof(vk::DescriptorBufferInfo) + sizeof(vk::DescriptorImageInfo) +
                                   sizeof(vk::BufferView) + 32) +
                    2048 >
                64 * 1024) {
            scheduler.CommandBuffer().pushDescriptorSetKHR(bind_point, layout, 0, set_writes);
            return;
        }
        if (!scheduler.IsRecordingDeferred()) {
            scheduler.Record([&](vk::CommandBuffer cmdbuf) {
                cmdbuf.pushDescriptorSetKHR(bind_point, layout, 0, set_writes);
            });
            return;
        }
        // Writes and the infos they point to are laid out in the recording chunk, with the
        // pointers already aimed at those copies: the command only carries a span.
        // bbport: when every write points into the caller's info arrays (draws and
        // dispatches), each array is copied once and the pointers are rebased: a copy per
        // descriptor (two per write) cost the draw recording thread ~3% of its time.
        const auto within = [](const auto* p, u32 count, const auto& array) {
            return p >= array.data() && p + count <= array.data() + array.size();
        };
        bool bulk = !image_infos.empty() || !buffer_infos.empty();
        for (const auto& write : set_writes) {
            if (!bulk) {
                break;
            }
            bulk = !write.pTexelBufferView &&
                   (!write.pImageInfo || within(write.pImageInfo, write.descriptorCount,
                                                image_infos)) &&
                   (!write.pBufferInfo || within(write.pBufferInfo, write.descriptorCount,
                                                 buffer_infos));
        }
        if (bulk) {
            scheduler.ReserveRecordData(set_writes.size() * sizeof(vk::WriteDescriptorSet) +
                                        image_infos.size_bytes() + buffer_infos.size_bytes() +
                                        256);
            const auto writes =
                scheduler.RecordData(std::span<const vk::WriteDescriptorSet>{set_writes});
            const auto images = scheduler.RecordData(image_infos);
            const auto buffers = scheduler.RecordData(buffer_infos);
            auto* patched = const_cast<vk::WriteDescriptorSet*>(writes.data());
            for (size_t i = 0; i < writes.size(); ++i) {
                auto& write = patched[i];
                if (write.pImageInfo) {
                    write.pImageInfo = images.data() + (write.pImageInfo - image_infos.data());
                }
                if (write.pBufferInfo) {
                    write.pBufferInfo = buffers.data() + (write.pBufferInfo - buffer_infos.data());
                }
            }
            scheduler.Record([bind_point, layout, writes](vk::CommandBuffer cmdbuf) {
                cmdbuf.pushDescriptorSetKHR(bind_point, layout, 0, writes);
            });
            return;
        }
        size_t bytes = set_writes.size() * sizeof(vk::WriteDescriptorSet) + 64;
        for (const auto& write : set_writes) {
            bytes += write.descriptorCount *
                         (sizeof(vk::DescriptorBufferInfo) + sizeof(vk::DescriptorImageInfo) +
                          sizeof(vk::BufferView)) +
                     32;
        }
        scheduler.ReserveRecordData(bytes);
        const auto writes = scheduler.RecordData(std::span<const vk::WriteDescriptorSet>{set_writes});
        auto* patched = const_cast<vk::WriteDescriptorSet*>(writes.data());
        for (size_t i = 0; i < writes.size(); ++i) {
            auto& write = patched[i];
            if (write.pBufferInfo) {
                write.pBufferInfo = scheduler.RecordData(std::span{write.pBufferInfo, write.descriptorCount}).data();
            }
            if (write.pImageInfo) {
                write.pImageInfo = scheduler.RecordData(std::span{write.pImageInfo, write.descriptorCount}).data();
            }
            if (write.pTexelBufferView) {
                write.pTexelBufferView =
                    scheduler.RecordData(std::span{write.pTexelBufferView, write.descriptorCount}).data();
            }
        }
        scheduler.Record([bind_point, layout, writes](vk::CommandBuffer cmdbuf) {
            cmdbuf.pushDescriptorSetKHR(bind_point, layout, 0, writes);
        });
        return;
    }

    // Descriptor set updates are device calls: they stay on this thread.
    const auto desc_set = desc_heap.Commit(*desc_layout);
    for (auto& set_write : set_writes) {
        set_write.dstSet = desc_set;
    }
    instance.GetDevice().updateDescriptorSets(set_writes, {});
    scheduler.Record([bind_point, layout, desc_set](vk::CommandBuffer cmdbuf) {
        cmdbuf.bindDescriptorSets(bind_point, layout, 0, desc_set, {});
    });
}

std::string Pipeline::GetDebugString() const {
    std::string stage_desc;
    for (const auto& stage : stages) {
        if (stage) {
            const auto shader_name = PipelineCache::GetShaderName(stage->hw_stage, stage->pgm_hash);
            if (stage_desc.empty()) {
                stage_desc = shader_name;
            } else {
                stage_desc = fmt::format("{},{}", stage_desc, shader_name);
            }
        }
    }
    return stage_desc;
}

} // namespace Vulkan
