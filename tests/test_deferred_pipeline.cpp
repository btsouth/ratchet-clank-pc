// SPDX-License-Identifier: GPL-2.0-or-later
// A pipeline whose driver compile runs on a worker: Handle() waits for it, then returns at once.
// HandleForDraw() waits at most the shown frame's budget, then leaves the draw out.
#include <dlfcn.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include "shader_recompiler/info.h"
#include "video_core/host_shaders/fs_tri_vert.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {
extern std::atomic<u64> g_bb_pipeline_wait_ns;
extern std::atomic<u64> g_bb_pipeline_skipped_draws;
}

namespace {
struct DeferredPipeline : Vulkan::Pipeline {
    DeferredPipeline(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                     Vulkan::DescriptorHeap& heap, const Shader::Profile& profile)
        : Pipeline{instance, scheduler, heap, profile, {}} {
        handle_pending.store(true);
    }
    void Finish() {
        PublishHandle();
    }
};
} // namespace

int main() {
    using namespace std::chrono;
    setenv("BB_PIPELINE_WAIT_MS", "8", 1);
    setenv("BB_PIPELINE_STALL_MS", "100", 1);
    setenv("BB_PIPELINE_SKIP_FRAMES", "3", 1);
    // The library's shown-frame counter (this file's inline copy is hidden).
    auto* shown = static_cast<std::atomic<std::uint32_t>*>(
        dlsym(RTLD_DEFAULT, "_ZN7BbStats12frame_numberE"));
    assert(shown);
    Vulkan::Instance instance(0, false);
    Vulkan::Scheduler scheduler(instance);
    const std::array pool_sizes{vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer, 4}};
    Vulkan::DescriptorHeap heap(instance, scheduler.GetWorkSemaphore(), pool_sizes, 1);
    const Shader::Profile profile{};

    DeferredPipeline pipeline(instance, scheduler, heap, profile);
    assert(pipeline.IsHandlePending());
    const auto start = steady_clock::now();
    std::thread worker([&] {
        std::this_thread::sleep_for(milliseconds(50));
        pipeline.Finish();
    });
    (void)pipeline.Handle(); // the recording thread's bind
    const auto waited = steady_clock::now() - start;
    worker.join();
    assert(!pipeline.IsHandlePending());
    assert(waited >= milliseconds(45));
    assert(Vulkan::g_bb_pipeline_wait_ns.load() >= 45'000'000);

    // Published: later binds do not wait.
    const auto again = steady_clock::now();
    (void)pipeline.Handle();
    assert(steady_clock::now() - again < milliseconds(5));
    std::printf("Deferred pipeline: Handle() waited %.1f ms for the worker, then none\n",
                duration<double, std::milli>(waited).count());

    // HandleForDraw: a draw waits for a pipeline still being built at most what is left of the
    // shown frame's budget (8 ms for recurring geometry, 100 ms here for others), then goes
    // without; the budget renews each shown frame; a pipeline missing in more than 3 shown
    // frames is waited for like a one-time draw (here 30 ms, past the 8 ms of geometry).
    const auto timed = [](auto&& call) {
        const auto t = steady_clock::now();
        const vk::Pipeline result = call();
        return std::pair{result, duration<double, std::milli>(steady_clock::now() - t).count()};
    };
    {
        DeferredPipeline slow(instance, scheduler, heap, profile);
        shown->fetch_add(1);
        const u64 skipped = Vulkan::g_bb_pipeline_skipped_draws.load();
        auto [h1, ms1] = timed([&] { return slow.HandleForDraw(true); });
        assert(!h1 && ms1 >= 7.0 && ms1 < 60.0);
        auto [h2, ms2] = timed([&] { return slow.HandleForDraw(true); });
        assert(!h2 && ms2 < 3.0); // the recurring budget is spent
        auto [h3, ms3] = timed([&] { return slow.HandleForDraw(false); });
        assert(!h3 && ms3 >= 80.0 && ms3 < 150.0); // the rest of the 100 ms
        auto [h4, ms4] = timed([&] { return slow.HandleForDraw(false); });
        assert(!h4 && ms4 < 3.0);
        assert(Vulkan::g_bb_pipeline_skipped_draws.load() == skipped + 4);
        shown->fetch_add(1); // a new frame, a new budget (missing in 2 frames)
        auto [h5, ms5] = timed([&] { return slow.HandleForDraw(true); });
        assert(!h5 && ms5 >= 7.0);
        shown->fetch_add(1);
        (void)slow.HandleForDraw(true); // 3 frames
        shown->fetch_add(1);           // 4: overdue, waited for
        std::thread finisher([&] {
            std::this_thread::sleep_for(milliseconds(30));
            slow.Finish();
        });
        auto [h6, ms6] = timed([&] { return slow.HandleForDraw(true); });
        finisher.join();
        assert(!slow.IsHandlePending() && ms6 >= 25.0);
        std::printf("HandleForDraw: left out after %.1f / %.1f ms of a frame's budget, waited "
                    "%.1f ms once missing in 4 frames\n",
                    ms1, ms3, ms6);
    }

    // A real GraphicsPipeline built through a deferral (vertex stage only, no attachments): the
    // driver compile runs on the worker, Handle() waits for it and gets a pipeline; destroying a
    // pipeline whose build has not run yet waits for that build.
    const vk::Device device = instance.GetDevice();
    const vk::ShaderModule vs_module = Vulkan::CompileSPV(FS_TRI_VERT, device);
    const std::array<u32, Shader::ShaderParams::NumShaderUserData> user_data{};
    const Shader::Info vs_info{Shader::HwStage::Vertex, Shader::SwStage::Vertex,
                               Shader::ShaderParams{.user_data = user_data, .code = {}, .hash = 1}};
    std::array<const Shader::Info*, Vulkan::MaxShaderStages> infos{};
    infos[u32(Shader::SwStage::Vertex)] = &vs_info;
    std::array<Shader::RuntimeInfo, Vulkan::MaxShaderStages> runtime_infos{};
    std::array<vk::ShaderModule, Vulkan::MaxShaderStages> modules{}; // indexed by stage
    modules[u32(Shader::SwStage::Vertex)] = vs_module;
    Vulkan::GraphicsPipelineKey key{};
    key.stage_hashes[u32(Shader::SwStage::Vertex)] = 1;
    key.prim_type = AmdGpu::PrimitiveType::TriangleList;
    key.polygon_mode = AmdGpu::PolygonMode::Fill;
    key.depth_clip_enable = 1;
    key.num_samples = key.depth_samples = 1;
    key.color_samples.fill(1);

    std::mutex jobs_mutex;
    std::vector<std::function<void()>> jobs;
    std::atomic<u32> built{0};
    const Vulkan::GraphicsPipeline::Deferral deferral{
        .submit =
            [&](std::function<void()> job, bool urgent) {
                std::scoped_lock lk{jobs_mutex};
                jobs.insert(urgent ? jobs.begin() : jobs.end(), std::move(job));
            },
        .built = [&](const Vulkan::GraphicsPipeline::SerializationSupport&) { ++built; },
    };
    const auto make = [&] {
        Vulkan::GraphicsPipeline::SerializationSupport sdata{};
        return std::make_unique<Vulkan::GraphicsPipeline>(
            instance, scheduler, heap, profile, key, vk::PipelineCache{}, infos, runtime_infos,
            std::nullopt, modules, sdata, false, &deferral);
    };
    const auto run_jobs_later = [&](milliseconds delay) {
        return std::thread([&, delay] {
            std::this_thread::sleep_for(delay);
            std::vector<std::function<void()>> todo;
            {
                std::scoped_lock lk{jobs_mutex};
                todo.swap(jobs);
            }
            for (auto& job : todo) {
                job();
            }
        });
    };

    auto real = make();
    assert(real->IsHandlePending() && built.load() == 0);
    std::thread real_worker = run_jobs_later(milliseconds(20));
    const vk::Pipeline handle = real->Handle();
    real_worker.join();
    assert(handle && !real->IsHandlePending() && built.load() == 1);

    // A draw that waits puts its build ahead of the queued ones: a worker that runs only the
    // first job builds it; the job queued first then does nothing.
    auto queued_behind = make();
    shown->fetch_add(1);
    std::thread first_job([&] {
        std::this_thread::sleep_for(milliseconds(20));
        std::function<void()> job;
        {
            std::scoped_lock lk{jobs_mutex};
            assert(jobs.size() == 2);
            job = std::move(jobs.front());
            jobs.erase(jobs.begin());
        }
        job();
    });
    const vk::Pipeline expedited = queued_behind->HandleForDraw(false);
    first_job.join();
    assert(expedited && !queued_behind->IsHandlePending() && built.load() == 2);
    run_jobs_later(milliseconds(0)).join();
    assert(built.load() == 2);

    // Destroying a pipeline whose build has not started returns at once; its job does nothing.
    auto dropped = make();
    assert(dropped->IsHandlePending());
    const auto drop_start = steady_clock::now();
    dropped.reset();
    assert(steady_clock::now() - drop_start < milliseconds(20));
    run_jobs_later(milliseconds(0)).join();
    assert(built.load() == 2);
    queued_behind.reset();

    real.reset();
    // Vulkan calls from this executable need their own dispatcher (libbbgpu.so has its own).
    static vk::detail::DynamicLoader loader;
    vk::detail::DispatchLoaderDynamic dispatch;
    dispatch.init(loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
    dispatch.init(instance.GetInstance());
    dispatch.init(device);
    device.destroyShaderModule(vs_module, nullptr, dispatch);
    std::printf("Deferred GraphicsPipeline: built on the worker, bound after it; moved ahead "
                "of the queue by a draw that waits; "
                "dropped before its build without one\n");
    return 0;
}
