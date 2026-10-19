// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <mutex>
#include <type_traits>
#include <thread>
#include <time.h>
#include <vector>
#include <x86intrin.h>
#include "common/thread.h"

#include "bbport_toggles.h"
#include "common/debug.h"
#include "common/types.h"
#include "core/emulator_settings.h"
#include "video_core/buffer_cache/region_manager.h"
#include "video_core/buffer_cache/region_summary.h"

namespace VideoCore {

class MemoryTracker {
public:
    static constexpr size_t MAX_CPU_PAGE_BITS = 40;
    static constexpr size_t NUM_HIGH_PAGES = 1ULL << (MAX_CPU_PAGE_BITS - TRACKER_HIGHER_PAGE_BITS);
    static constexpr size_t MANAGER_POOL_SIZE = 32;

public:
    explicit MemoryTracker(PageManager& tracker_) : tracker{&tracker_} {}
    ~MemoryTracker() = default;

    /// bbport: whether some 4 MiB region of the range may hold CPU-modified pages, from one bit
    /// per region. Ratchet & Clank binds MiBs per draw (its geometry ring; 12, 22 and 800 MiB
    /// ranges in frame captures): read region by region, the check was up to ~8% of the draw
    /// recording thread.
    bool MaybeCpuModified(VAddr query_cpu_addr, u64 query_size) const noexcept {
        if (query_size == 0) {
            return false;
        }
        if (BbToggle::Experiment(2)) {
            return true; // experiment bit 2: every region looked at, as before (A/B)
        }
        return cpu_summary.Any(query_cpu_addr >> TRACKER_HIGHER_PAGE_BITS,
                               (query_cpu_addr + query_size - 1) >> TRACKER_HIGHER_PAGE_BITS);
    }

    /// bbport (async write tracking, PageManager::AsyncWrites): pages written since the last
    /// scan of their 4 MiB region become CPU-modified. The work of a thread must see a scan made
    /// in the epoch it belongs to or later (BbStats::upload_epoch: a guest submission or a met
    /// WaitRegMem; what the guest wrote before it lets the GPU go on; the draw recording thread's
    /// packets carry the epoch they were decoded in). The scanner thread scans the regions in use
    /// as soon as the epoch changes, and every BB_ASYNC_WP_US besides; the caller scans the ones
    /// it did not get to yet.
    void SyncAsyncWrites(VAddr query_cpu_addr, u64 query_size, bool keep_scanned = true) {
        if (!tracker->AsyncWrites() || query_size == 0) {
            return;
        }
        const u64 epoch = BbStats::upload_epoch.load(std::memory_order_acquire);
        const u64 required = BbStats::async_required_epoch ? BbStats::async_required_epoch : epoch;
        const u64 last = (query_cpu_addr + query_size - 1) >> TRACKER_HIGHER_PAGE_BITS;
        for (u64 index = query_cpu_addr >> TRACKER_HIGHER_PAGE_BITS; index <= last; ++index) {
            auto* manager = top_tier[index];
            if (!manager) {
                continue;
            }
            if (keep_scanned && manager->NoteAsyncUse(epoch)) {
                ListForScanner(manager);
            }
            if (manager->AsyncFresh(required)) {
                continue;
            }
            std::scoped_lock lk{manager->lock};
            if (!manager->AsyncFresh(required)) {
                BbStats::async_inline_scans.fetch_add(1, std::memory_order_relaxed);
                if (BbStats::async_required_epoch) {
                    BbStats::async_recorder_scans.fetch_add(1, std::memory_order_relaxed);
                }
                manager->ScanAsyncWrites(BbStats::upload_epoch.load(std::memory_order_acquire),
                                         __rdtsc());
            }
        }
    }

    /// bbport (async write tracking): the GPU side wrote [addr, addr + size) itself (command
    /// processor writes): those pages are CPU-modified, as a write fault would have marked them.
    void NoteHostWrite(VAddr addr, u64 size) {
        if (!tracker->AsyncWrites() || size == 0) {
            return;
        }
        IteratePages<false>(addr, size, [](RegionManager* manager, u64 offset, size_t size) {
            std::scoped_lock lk{manager->lock};
            manager->NoteHostWrite(manager->GetCpuAddr() + offset, size);
        });
    }

    /// Returns true if a region has been modified from the CPU
    bool IsRegionCpuModified(VAddr query_cpu_addr, u64 query_size) noexcept {
        SyncAsyncWrites(query_cpu_addr, query_size);
        if (!BbToggle::Disabled(BbToggle::LockFreeUploadCheck) &&
            !MaybeCpuModified(query_cpu_addr, query_size)) {
            return false;
        }
        return IteratePages<true>(
            query_cpu_addr, query_size, [](RegionManager* manager, u64 offset, size_t size) {
                return manager->template IsRegionModified<Type::CPU>(offset, size);
            });
    }

    /// bbport (pre-upload): whether the 4 MiB region at `region_addr` holds CPU-modified pages
    /// that are not GPU-modified, with no guest write seen since frame `quiet_since`.
    /// `rewritten`: only regions the guest wrote after the GPU had used them (write faults).
    bool PreuploadCandidate(VAddr region_addr, u32 quiet_since, bool rewritten) noexcept {
        auto* manager = top_tier[region_addr >> TRACKER_HIGHER_PAGE_BITS];
        if (!manager) {
            return !rewritten; // never used by the GPU: all of it is CPU-modified
        }
        SyncAsyncWrites(region_addr, TRACKER_HIGHER_PAGE_SIZE, false);
        std::scoped_lock lk{manager->lock};
        if (rewritten && manager->last_cpu_write_frame == 0) {
            return false;
        }
        return manager->last_cpu_write_frame < quiet_since &&
               manager->preupload_backoff_until <= quiet_since && manager->HasCpuOnlyPages();
    }
    /// bbport (pre-upload): notes that the region was pre-uploaded at `frame`.
    /// Returns how many times it was pre-uploaded before.
    u32 NotePreuploaded(VAddr region_addr, u32 frame) noexcept {
        if (auto* manager = top_tier[region_addr >> TRACKER_HIGHER_PAGE_BITS]) {
            std::scoped_lock lk{manager->lock};
            manager->preupload_frame = frame;
            return manager->preupload_count++;
        }
        return 0;
    }

    /// Returns true if a region has been modified from the GPU
    bool IsRegionGpuModified(VAddr query_cpu_addr, u64 query_size) noexcept {
        return IteratePages<false>(
            query_cpu_addr, query_size, [](RegionManager* manager, u64 offset, size_t size) {
                return manager->template IsRegionModified<Type::GPU>(offset, size);
            });
    }

    /// Mark region as CPU modified, notifying the device_tracker about this change
    void MarkRegionAsCpuModified(VAddr dirty_cpu_addr, u64 query_size) {
        IteratePages<false>(dirty_cpu_addr, query_size,
                            [](RegionManager* manager, u64 offset, size_t size) {
                                std::scoped_lock lk{manager->lock};
                                manager->template ChangeRegionState<Type::CPU, true>(
                                    manager->GetCpuAddr() + offset, size);
                            });
    }

    /// bbport: see RegionManager::ExtendWriteFault.
    void ExtendWriteFault(VAddr window_addr, u64 size) {
        IteratePages<false>(window_addr, size,
                            [](RegionManager* manager, u64 offset, size_t size) {
                                std::scoped_lock lk{manager->lock};
                                manager->ExtendWriteFault(manager->GetCpuAddr() + offset, size);
                            });
    }

    /// Unmark region as modified from the host GPU
    void UnmarkRegionAsGpuModified(VAddr dirty_cpu_addr, u64 query_size) noexcept {
        IteratePages<false>(dirty_cpu_addr, query_size,
                            [](RegionManager* manager, u64 offset, size_t size) {
                                std::scoped_lock lk{manager->lock};
                                manager->template ChangeRegionState<Type::GPU, false>(
                                    manager->GetCpuAddr() + offset, size);
                            });
    }

    /// Removes all protection from a page and ensures GPU data has been flushed if requested
    void InvalidateRegion(VAddr cpu_addr, u64 size, auto&& on_flush) noexcept {
        IteratePages<false>(
            cpu_addr, size, [&on_flush](RegionManager* manager, u64 offset, size_t size) {
                const bool should_flush = [&] {
                    // Perform both the GPU modification check and CPU state change with the lock
                    // in case we are racing with GPU thread trying to mark the page as GPU
                    // modified. If we need to flush the flush function is going to perform CPU
                    // state change.
                    std::scoped_lock lk{manager->lock};
                    if (EmulatorSettings.GetReadbacksMode() != GpuReadbacksMode::Disabled &&
                        manager->template IsRegionModified<Type::GPU>(offset, size)) {
                        return true;
                    }
                    manager->template ChangeRegionState<Type::CPU, true>(
                        manager->GetCpuAddr() + offset, size);
                    return false;
                }();
                if (should_flush) {
                    on_flush();
                }
            });
    }

    /// Call 'func' for each CPU modified range and unmark those pages as CPU modified
    void ForEachUploadRange(VAddr query_cpu_range, u64 query_size, bool is_written, auto&& func,
                            auto&& on_upload) {
        SyncAsyncWrites(query_cpu_range, query_size);
        // bbport: a write binding over pages the CPU did not change and the GPU already owns
        // needs neither an upload nor a state change (it ran under the region lock for every
        // GPU write binding, twice per page). Racing guest writes are as in the read path.
        if (!is_written && !BbToggle::Disabled(BbToggle::LockFreeUploadCheck) &&
            !MaybeCpuModified(query_cpu_range, query_size)) {
            on_upload();
            return;
        }
        if (is_written && !BbToggle::Disabled(BbToggle::LockFreeUploadCheck)) {
            bool settled = true;
            IteratePages<true>(query_cpu_range, query_size,
                               [&settled](RegionManager* manager, u64 offset, size_t size) {
                                    settled = settled &&
                                              !manager->template IsRegionModified<Type::CPU>(offset, size) &&
                                              manager->template IsRegionFullyModified<Type::GPU>(offset, size);
                                });
            if (settled) {
                on_upload();
                return;
            }
        }
        IteratePages<true>(query_cpu_range, query_size,
                           [&func, is_written](RegionManager* manager, u64 offset, size_t size) {
                               // bbport: read-only bindings skip the region lock when no page
                               // in range is CPU-modified. A guest write sets its bit in the
                               // fault handler before the store, and the draw that uses the
                               // data reaches this thread through the submission queue lock.
                               if (!is_written &&
                                   !BbToggle::Disabled(BbToggle::LockFreeUploadCheck) &&
                                   !manager->template IsRegionModified<Type::CPU>(offset, size)) {
                                   return;
                               }
                               manager->lock.lock();
                               manager->template ForEachModifiedRange<Type::CPU, true>(
                                   manager->GetCpuAddr() + offset, size, func);
                               if (!is_written) {
                                   manager->lock.unlock();
                               }
                           });
        on_upload();
        if (!is_written) {
            return;
        }
        IteratePages<false>(query_cpu_range, query_size,
                            [&func, is_written](RegionManager* manager, u64 offset, size_t size) {
                                manager->template ChangeRegionState<Type::GPU, true>(
                                    manager->GetCpuAddr() + offset, size);
                                manager->lock.unlock();
                            });
    }

    /// Call 'func' for each GPU modified range and unmark those pages as GPU modified
    template <bool clear>
    void ForEachDownloadRange(VAddr query_cpu_range, u64 query_size, auto&& func) {
        IteratePages<false>(query_cpu_range, query_size,
                            [&func](RegionManager* manager, u64 offset, size_t size) {
                                std::scoped_lock lk{manager->lock};
                                manager->template ForEachModifiedRange<Type::GPU, clear>(
                                    manager->GetCpuAddr() + offset, size, func);
                            });
    }

private:
    void ListForScanner(RegionManager* manager) {
        std::scoped_lock lk{async_mutex};
        async_regions.push_back(manager);
        async_regions_changed.store(true, std::memory_order_release);
        if (!async_scanner.joinable()) {
            async_scanner = std::jthread([this](std::stop_token stop) { AsyncScanner(stop); });
        }
    }

    /// bbport (async write tracking): scans the regions the GPU side uses as soon as a new
    /// epoch starts, off the draw recording thread (which then finds them scanned), and every
    /// BB_ASYNC_WP_US besides (writes the guest makes without a submission in between are then
    /// seen within about that time too). A scan costs mostly per ioctl (~1 us), little per page
    /// (~0.3 ns), so adjacent regions are scanned together, with their locks held.
    void AsyncScanner(std::stop_token stop) {
        Common::SetCurrentThreadName("bb:AsyncScan");
        // Regions used in the last two frames: the next submissions use them too. Passes come
        // at most every PassUs: a pass covers all epochs started since the last one (the draw
        // recording thread lags behind them; it needs a scan from its packet's epoch on).
        constexpr u64 PassUs = 400;
        u64 recent_epochs = 256;
        u32 seen_frame = 0;
        std::array<u64, 3> frame_epochs{};
        constexpr u64 ForgetEpochs = 4096;  // seconds of submissions without a use
        constexpr size_t MaxGroup = 64;     // regions per ioctl (256 MiB)
        std::vector<RegionManager*> regions, group, active;
        std::vector<std::pair<VAddr, VAddr>> runs;
        u64 scanned_epoch = 0, pass_tsc = 0;
        const u64 max_age = tracker->AsyncMaxAgeTicks();
        const u64 tsc_per_us = std::max<u64>(1, BbStats::async_tsc_per_ms.load() / 1000);
        std::stop_callback wake_on_stop(stop, [] {
            BbStats::async_wake.fetch_add(1);
            syscall(SYS_futex, reinterpret_cast<u32*>(&BbStats::async_wake), FUTEX_WAKE_PRIVATE,
                    1, nullptr, nullptr, 0);
        });
        const auto scan_group = [&](u64 epoch, u64 now) {
            for (auto* manager : group) {
                manager->lock.lock();
            }
            active.clear();
            for (auto* manager : group) {
                if (manager->BeginAsyncScan(epoch, now)) {
                    active.push_back(manager);
                }
            }
            if (!active.empty()) {
                runs.clear();
                const VAddr from = active.front()->GetCpuAddr();
                const VAddr to = active.back()->GetCpuAddr() + TRACKER_HIGHER_PAGE_SIZE;
                const u64 t0 = __rdtsc();
                const bool ok = tracker->ScanWritten(from, to - from, runs);
                BbStats::async_scans.fetch_add(1, std::memory_order_relaxed);
                BbStats::async_scan_ticks.fetch_add(__rdtsc() - t0, std::memory_order_relaxed);
                for (auto* manager : active) {
                    if (ok) {
                        manager->FinishAsyncScan(runs, epoch, now);
                    } else {
                        manager->ScanAsyncWrites(epoch, now); // on its own: which one failed
                    }
                }
            }
            for (auto* manager : group) {
                manager->lock.unlock();
            }
            group.clear();
        };
        while (!stop.stop_requested()) {
            const u32 wake = BbStats::async_wake.load(std::memory_order_acquire);
            BbStats::async_scanner_idle.store(true, std::memory_order_seq_cst);
            const u64 epoch = BbStats::upload_epoch.load(std::memory_order_seq_cst);
            const u64 now = __rdtsc();
            const u64 since = now - pass_tsc;
            if (epoch == scanned_epoch && since < max_age) {
                // Until the next epoch (NewUploadEpoch wakes this) or the next age pass.
                const u64 left_us =
                    max_age == ~0ull ? 100'000 : std::max<u64>(1, (max_age - since) / tsc_per_us);
                timespec timeout{s64(left_us / 1'000'000), s64(left_us % 1'000'000) * 1000};
                syscall(SYS_futex, reinterpret_cast<u32*>(&BbStats::async_wake), FUTEX_WAIT_PRIVATE,
                        wake, &timeout, nullptr, 0);
                continue;
            }
            BbStats::async_scanner_idle.store(false, std::memory_order_relaxed);
            if (since < PassUs * tsc_per_us) {
                // A new epoch soon after the last pass: the next pass takes it with the ones after.
                timespec pause{0, s64((PassUs * tsc_per_us - since) / tsc_per_us) * 1000};
                nanosleep(&pause, nullptr);
                continue;
            }
            const bool aged = epoch == scanned_epoch;
            scanned_epoch = epoch;
            if (const u32 frame = BbStats::frame_number.load(std::memory_order_relaxed);
                frame != seen_frame) {
                seen_frame = frame;
                frame_epochs = {epoch, frame_epochs[0], frame_epochs[1]};
                if (frame_epochs[2] != 0) {
                    recent_epochs = std::clamp<u64>(epoch - frame_epochs[2], 16, 4096);
                }
            }
            pass_tsc = now;
            if (async_regions_changed.exchange(false, std::memory_order_acq_rel)) {
                std::scoped_lock lk{async_mutex};
                regions = async_regions;
                std::sort(regions.begin(), regions.end(), [](auto* a, auto* b) {
                    return a->GetCpuAddr() < b->GetCpuAddr();
                });
            }
            bool forget = false;
            for (auto* manager : regions) {
                const u64 used = epoch - std::min(epoch, manager->AsyncUseEpoch());
                if (used > ForgetEpochs) {
                    forget = true;
                    continue;
                }
                const bool due = used <= recent_epochs &&
                                 (aged ? now - manager->AsyncScanTsc() >= max_age
                                       : !manager->AsyncFresh(epoch));
                if (!due) {
                    continue;
                }
                if (aged) {
                    BbStats::async_age_scans.fetch_add(1, std::memory_order_relaxed);
                }
                if (!group.empty() &&
                    (group.size() == MaxGroup ||
                     group.back()->GetCpuAddr() + TRACKER_HIGHER_PAGE_SIZE != manager->GetCpuAddr())) {
                    scan_group(epoch, now);
                }
                group.push_back(manager);
            }
            if (!group.empty()) {
                scan_group(epoch, now);
            }
            if (forget) {
                std::scoped_lock lk{async_mutex};
                std::erase_if(async_regions, [&](RegionManager* manager) {
                    if (epoch - std::min(epoch, manager->AsyncUseEpoch()) <= ForgetEpochs) {
                        return false;
                    }
                    manager->AsyncUnlist();
                    return true;
                });
                async_regions_changed.store(true, std::memory_order_release);
            }
        }
    }

    /**
     * @brief IteratePages Iterates L2 word manager page table.
     * @param cpu_address Start byte cpu address
     * @param size Size in bytes of the region of iterate.
     * @param func Callback for each word manager.
     * @return
     */
    template <bool create_region_on_fail, typename Func>
    bool IteratePages(VAddr cpu_address, size_t size, Func&& func) {
        RENDERER_TRACE;
        using FuncReturn = typename std::invoke_result<Func, RegionManager*, u64, size_t>::type;
        static constexpr bool BOOL_BREAK = std::is_same_v<FuncReturn, bool>;
        std::size_t remaining_size{size};
        std::size_t page_index{cpu_address >> TRACKER_HIGHER_PAGE_BITS};
        u64 page_offset{cpu_address & TRACKER_HIGHER_PAGE_MASK};
        while (remaining_size > 0) {
            const std::size_t copy_amount{
                std::min<std::size_t>(TRACKER_HIGHER_PAGE_SIZE - page_offset, remaining_size)};
            auto* manager{top_tier[page_index]};
            if (manager) {
                if constexpr (BOOL_BREAK) {
                    if (func(manager, page_offset, copy_amount)) {
                        return true;
                    }
                } else {
                    func(manager, page_offset, copy_amount);
                }
            } else if constexpr (create_region_on_fail) {
                CreateRegion(page_index);
                manager = top_tier[page_index];
                if constexpr (BOOL_BREAK) {
                    if (func(manager, page_offset, copy_amount)) {
                        return true;
                    }
                } else {
                    func(manager, page_offset, copy_amount);
                }
            }
            page_index++;
            page_offset = 0;
            remaining_size -= copy_amount;
        }
        return false;
    }

    void CreateRegion(std::size_t page_index) {
        const VAddr base_cpu_addr = page_index << TRACKER_HIGHER_PAGE_BITS;
        if (free_managers.empty()) {
            manager_pool.emplace_back();
            auto& last_pool = manager_pool.back();
            for (size_t i = 0; i < MANAGER_POOL_SIZE; i++) {
                std::construct_at(&last_pool[i], tracker, 0);
                free_managers.push_back(&last_pool[i]);
            }
        }
        // Each manager tracks a 4_MB virtual address space.
        auto* new_manager = free_managers.back();
        new_manager->SetCpuAddress(base_cpu_addr);
        new_manager->SetSummary(cpu_summary.Word(page_index), cpu_summary.Mask(page_index));
        free_managers.pop_back();
        top_tier[page_index] = new_manager;
    }

    PageManager* tracker;
    std::deque<std::array<RegionManager, MANAGER_POOL_SIZE>> manager_pool;
    std::vector<RegionManager*> free_managers;
    std::array<RegionManager*, NUM_HIGH_PAGES> top_tier{};
    RegionSummary<NUM_HIGH_PAGES> cpu_summary;
    std::mutex async_mutex;
    std::vector<RegionManager*> async_regions;
    std::atomic<bool> async_regions_changed{false};
    std::jthread async_scanner; ///< last: stopped and joined before the members it reads go
};

} // namespace VideoCore
