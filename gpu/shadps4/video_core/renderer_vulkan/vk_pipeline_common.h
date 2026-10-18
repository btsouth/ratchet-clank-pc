// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/renderer_vulkan/vk_common.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <boost/container/small_vector.hpp>

namespace Shader {
struct Info;
struct PushData;
} // namespace Shader

namespace Vulkan {

/// bbport: time a thread that makes the game's frames waited for a shader or pipeline compile
/// (on the GPU thread, or a recorder waiting for a pipeline built on a worker); never reset.
extern std::atomic<u64> g_bb_compile_stall_ns;
/// bbport: draws recorded without their pipeline because it was still being built
/// (Pipeline::HandleForDraw); never reset.
extern std::atomic<u64> g_bb_pipeline_skipped_draws;


static constexpr auto AllGraphicsStageBits =
    vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eTessellationControl |
    vk::ShaderStageFlagBits::eTessellationEvaluation | vk::ShaderStageFlagBits::eGeometry |
    vk::ShaderStageFlagBits::eFragment;

class Instance;
class Scheduler;
class DescriptorHeap;

class Pipeline {
public:
    Pipeline(const Instance& instance, Scheduler& scheduler, DescriptorHeap& desc_heap,
             const Shader::Profile& profile, vk::PipelineCache pipeline_cache,
             bool is_compute = false);
    virtual ~Pipeline();

    /// bbport: a pipeline whose driver compile was deferred to a worker (graphics pipelines
    /// made on the GPU thread) is waited for here, by whoever binds it first: the recording
    /// thread, so the GPU thread goes on to the next draws and their pipelines meanwhile.
    vk::Pipeline Handle() const {
        if (handle_pending.load(std::memory_order_acquire)) {
            WaitHandle();
        }
        return *pipeline;
    }

    /// bbport: Handle() for a draw, which may go without its pipeline: a first-visit compile
    /// must not hold back the frame (and the flip the game's watchdog waits for). Null when
    /// the pipeline is still being built and the wait budget of this shown frame is spent: a
    /// `recurring` draw (geometry into a target drawn in each of the last frames) gets
    /// BB_PIPELINE_WAIT_MS of it, any other BB_PIPELINE_STALL_MS. A pipeline missing in
    /// BB_PIPELINE_SKIP_FRAMES shown frames is waited for.
    vk::Pipeline HandleForDraw(bool recurring) const {
        if (handle_pending.load(std::memory_order_acquire)) {
            return HandleForDrawSlow(recurring);
        }
        return *pipeline;
    }

    bool IsHandlePending() const noexcept {
        return handle_pending.load(std::memory_order_acquire);
    }

    vk::PipelineLayout GetLayout() const noexcept {
        return *pipeline_layout;
    }

    auto GetStages() const {
        static_assert(static_cast<u32>(Shader::SwStage::Compute) == Shader::MaxStageTypes - 1);
        if (is_compute) {
            return std::span{stages.cend() - 1, stages.cend()};
        } else {
            return std::span{stages.cbegin(), stages.cend() - 1};
        }
    }

    const Shader::Info& GetStage(Shader::SwStage stage) const noexcept {
        return *stages[u32(stage)];
    }

    bool IsCompute() const {
        return is_compute;
    }

    using DescriptorWrites = std::vector<vk::WriteDescriptorSet>;
    /// `image_infos` and `buffer_infos`: the arrays the writes point into, if known (their
    /// descriptors are then copied for the recording thread in one piece each).
    void BindResources(DescriptorWrites& set_writes, const Shader::PushData& push_data,
                       std::span<const vk::DescriptorImageInfo> image_infos = {},
                       std::span<const vk::DescriptorBufferInfo> buffer_infos = {}) const;

protected:
    [[nodiscard]] std::string GetDebugString() const;

    void WaitHandle() const;
    vk::Pipeline HandleForDrawSlow(bool recurring) const;
    /// Builds the pipeline on the calling thread if its worker job has not started (and then
    /// never will); false when it has, or there is no deferred build.
    virtual bool BuildNow() const {
        return false;
    }
    /// Moves its build ahead of the queued ones, if it has not started.
    virtual void Expedite() const {}
    /// Called by the worker once `pipeline` is set.
    void PublishHandle();

    const Instance& instance;
    Scheduler& scheduler;
    DescriptorHeap& desc_heap;
    const Shader::Profile& profile;
    vk::UniquePipeline pipeline;
    vk::UniquePipelineLayout pipeline_layout;
    vk::UniqueDescriptorSetLayout desc_layout;
    std::array<const Shader::Info*, Shader::MaxStageTypes> stages{};
    bool uses_push_descriptors{};
    bool is_compute;
    std::atomic<bool> handle_pending{false};
    mutable std::mutex handle_mutex;
    mutable std::condition_variable handle_cv;
    mutable u32 missing_frame = ~0u; ///< the last shown frame a draw found it pending
    mutable u32 missing_frames = 0;  ///< shown frames in which a draw found it pending
};

} // namespace Vulkan
