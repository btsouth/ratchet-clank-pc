// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <utility>
#include <vector>
#include <x86intrin.h>

#include "bbport_toggles.h"
#include "game_profile.h"
#include "common/div_ceil.h"
#include "common/logging/log.h"
#include "core/emulator_settings.h"

#ifdef __unix__
#include "common/adaptive_mutex.h"
#else
#include "common/spin_lock.h"
#endif
#include "common/debug.h"
#include "common/types.h"
#include "video_core/buffer_cache/region_definitions.h"
#include "video_core/page_manager.h"

namespace VideoCore {

#ifdef PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP
using LockType = Common::AdaptiveMutex;
#else
using LockType = Common::SpinLock;
#endif

/**
 * Allows tracking CPU and GPU modification of pages in a contigious 16MB virtual address region.
 * Information is stored in bitsets for spacial locality and fast update of single pages.
 */
class RegionManager {
public:
    explicit RegionManager(PageManager* tracker_, VAddr cpu_addr_)
        : tracker{tracker_}, cpu_addr{cpu_addr_} {
        cpu.Fill();
        gpu.Clear();
        writeable.Fill();
        readable.Fill();
    }
    explicit RegionManager() = default;

    void SetCpuAddress(VAddr new_cpu_addr) {
        cpu_addr = new_cpu_addr;
    }

    VAddr GetCpuAddr() const {
        return cpu_addr;
    }

    /// bbport: the MemoryTracker's bit for this region: set while some page may be CPU-modified
    /// (or hot), so lock-free checks of large bindings skip clean regions without reading them.
    void SetSummary(std::atomic<u64>* word, u64 mask) {
        summary_word = word;
        summary_mask = mask;
        NoteCpuBits();
    }

    static constexpr size_t SanitizeAddress(size_t address) {
        return static_cast<size_t>(std::max<s64>(static_cast<s64>(address), 0LL));
    }

    template <Type type>
    RegionBits& GetRegionBits() noexcept {
        if constexpr (type == Type::CPU) {
            return cpu;
        } else if constexpr (type == Type::GPU) {
            return gpu;
        }
    }

    template <Type type>
    const RegionBits& GetRegionBits() const noexcept {
        if constexpr (type == Type::CPU) {
            return cpu;
        } else if constexpr (type == Type::GPU) {
            return gpu;
        }
    }

    /**
     * Change the state of a range of pages
     *
     * @param dirty_addr    Base address to mark or unmark as modified
     * @param size          Size in bytes to mark or unmark as modified
     */
    template <Type type, bool enable>
    void ChangeRegionState(u64 dirty_addr, u64 size) noexcept(type == Type::GPU) {
        RENDERER_TRACE;
        const size_t offset = dirty_addr - cpu_addr;
        const size_t start_page = SanitizeAddress(offset) / TRACKER_BYTES_PER_PAGE;
        const size_t end_page =
            Common::DivCeil(SanitizeAddress(offset + size), TRACKER_BYTES_PER_PAGE);
        if (start_page >= NUM_PAGES_PER_REGION || end_page <= start_page) {
            return;
        }

        RegionBits& bits = GetRegionBits<type>();
        if constexpr (type == Type::CPU && enable) {
            NoteCpuBits();
            CountWriteFaults(start_page, end_page);
            last_cpu_write_frame = BbStats::frame_number.load(std::memory_order_relaxed);
            NotePreuploadChurn();
        }
        if constexpr (enable) {
            bits.SetRange(start_page, end_page);
        } else {
            bits.UnsetRange(start_page, end_page);
        }
        if constexpr (type == Type::CPU) {
            if constexpr (!enable) {
                MaybeClearSummary();
            }
            UpdateProtection<!enable, false>();
        } else {
            if (EmulatorSettings.GetReadbacksMode() == GpuReadbacksMode::Precise) {
                UpdateProtection<enable, true>();
            }
            // bbport: async write tracking: GPU-written pages are page-protected (see
            // UpdateWriteProtection).
            if (tracker->AsyncWrites()) {
                UpdateWriteProtection();
            }
            // bbport: a hot page the GPU writes is protected again, so a guest write to it is
            // seen (and the GPU's data is not overwritten by an upload of the guest's copy).
            if (enable && num_hot != 0) {
                UpdateProtection<true, false>();
            }
        }
    }

    /// bbport: after a guest write fault, also unprotects the other pages of the window that
    /// are protected for CPU writes and hold no GPU-modified data (see MarkFaultWindow).
    void ExtendWriteFault(VAddr window_addr, u64 size) {
        const size_t offset = window_addr - cpu_addr;
        const size_t start_page = SanitizeAddress(offset) / TRACKER_BYTES_PER_PAGE;
        const size_t end_page =
            Common::DivCeil(SanitizeAddress(offset + size), TRACKER_BYTES_PER_PAGE);
        if (start_page >= NUM_PAGES_PER_REGION || end_page <= start_page) {
            return;
        }
        RegionBits add(~(gpu | writeable), start_page, end_page);
        if (add.None()) {
            return;
        }
        NoteCpuBits();
        cpu |= add;
        UpdateProtection<false, false>();
    }

    /**
     * Loop over each page in the given range, turn off those bits and notify the tracker if
     * needed. Call the given function on each turned off range.
     *
     * @param query_cpu_range Base CPU address to loop over
     * @param size            Size in bytes of the CPU range to loop over
     * @param func            Function to call for each turned off region
     */
    template <Type type, bool clear>
    void ForEachModifiedRange(VAddr query_cpu_range, s64 size, auto&& func) {
        RENDERER_TRACE;
        const size_t offset = query_cpu_range - cpu_addr;
        const size_t start_page = SanitizeAddress(offset) / TRACKER_BYTES_PER_PAGE;
        const size_t end_page =
            Common::DivCeil(SanitizeAddress(offset + size), TRACKER_BYTES_PER_PAGE);
        if (start_page >= NUM_PAGES_PER_REGION || end_page <= start_page) {
            return;
        }

        if constexpr (type == Type::CPU) {
            RefreshHotPages();
        }
        RegionBits& bits = GetRegionBits<type>();
        // bbport: nothing modified in the range means nothing to clear or re-protect (CPU bits
        // always match `writeable` after a change); this runs for every buffer binding, so
        // it is checked before building the range mask.
        if (type == Type::CPU && !bits.AnyInRange(start_page, end_page) &&
            !BbToggle::Disabled(BbToggle::PageTrackingEarlyExit)) {
            return;
        }
        RegionBits mask(bits, start_page, end_page);

        if constexpr (clear) {
            bits.UnsetRange(start_page, end_page);
            if constexpr (type == Type::CPU) {
                KeepHotPagesModified(start_page, end_page);
                MaybeClearSummary();
                UpdateProtection<true, false>();
            } else if (EmulatorSettings.GetReadbacksMode() != GpuReadbacksMode::Disabled) {
                UpdateProtection<false, true>();
            }
        }

        for (const auto& [start, end] : mask) {
            func(cpu_addr + start * TRACKER_BYTES_PER_PAGE, (end - start) * TRACKER_BYTES_PER_PAGE);
        }
    }

    /**
     * Returns true when a region has been modified
     *
     * @param offset Offset in bytes from the start of the buffer
     * @param size   Size in bytes of the region to query for modifications
     */
    /// bbport: every page of the region modified.
    template <Type type>
    [[nodiscard]] bool IsRegionFullyModified(u64 offset, u64 size) noexcept {
        const size_t start_page = SanitizeAddress(offset) / TRACKER_BYTES_PER_PAGE;
        const size_t end_page =
            Common::DivCeil(SanitizeAddress(offset + size), TRACKER_BYTES_PER_PAGE);
        if (start_page >= NUM_PAGES_PER_REGION || end_page <= start_page) {
            return true;
        }
        const size_t last = std::min<size_t>(end_page, NUM_PAGES_PER_REGION);
        if constexpr (type == Type::CPU) {
            if (HotRefreshPending()) {
                return (cpu | (hot & ~gpu)).AllInRange(start_page, last);
            }
        }
        return GetRegionBits<type>().AllInRange(start_page, last);
    }

    template <Type type>
    [[nodiscard]] bool IsRegionModified(u64 offset, u64 size) noexcept {
        RENDERER_TRACE;
        const size_t start_page = SanitizeAddress(offset) / TRACKER_BYTES_PER_PAGE;
        const size_t end_page =
            Common::DivCeil(SanitizeAddress(offset + size), TRACKER_BYTES_PER_PAGE);
        if (start_page >= NUM_PAGES_PER_REGION || end_page <= start_page) {
            return false;
        }

        const RegionBits& bits = GetRegionBits<type>();
        if (bits.AnyInRange(start_page, end_page)) {
            return true;
        }
        // bbport: callers may hold no lock here (read-only bindings), so the hot pages are only
        // looked at; ForEachModifiedRange marks them modified under the lock.
        if constexpr (type == Type::CPU) {
            return HotRefreshPending() && (hot & ~gpu).AnyInRange(start_page, end_page);
        }
        return false;
    }

    /// bbport (pre-upload): whether some page is CPU-modified but not GPU-modified.
    [[nodiscard]] bool HasCpuOnlyPages() const noexcept {
        return (cpu & ~gpu).Any();
    }
    /// bbport (pre-upload): the frame of the newest guest write seen in this region.
    u32 last_cpu_write_frame = 0;
    /// bbport (pre-upload): when it was last pre-uploaded, and no pre-upload before this frame:
    /// a region the game writes again soon after a pre-upload backs off, longer each time.
    u32 preupload_frame = 0;
    u32 preupload_backoff_until = 0;
    u8 preupload_strikes = 0;
    u32 preupload_count = 0;
    void NotePreuploadChurn() noexcept {
        constexpr u32 ChurnFrames = 600; // ~10 s: regions the game rewrites every few seconds
        if (preupload_frame == 0 || last_cpu_write_frame - preupload_frame > ChurnFrames) {
            return;
        }
        // One quick rewrite may be the game loading new data there (worth pre-uploading); from
        // the second on it is data it rewrites now and then.
        preupload_strikes = std::min<u8>(preupload_strikes + 1, 7);
        if (preupload_strikes >= 2) {
            preupload_backoff_until = last_cpu_write_frame + (240u << (preupload_strikes - 1));
        }
        preupload_frame = 0;
    }

    /// bbport (async write tracking): whether the pages were scanned in `epoch` (guest
    /// submission or wait) or later and no GPU-side write came since. Read without the lock.
    [[nodiscard]] bool AsyncFresh(u64 epoch) const noexcept {
        return scan_epoch.load(std::memory_order_acquire) >= epoch;
    }
    /// TSC of the last scan.
    [[nodiscard]] u64 AsyncScanTsc() const noexcept {
        return scan_tsc.load(std::memory_order_relaxed);
    }
    /// The GPU side used the region in `epoch`: the scanner thread keeps it scanned (true: it
    /// was not on its list).
    bool NoteAsyncUse(u64 epoch) noexcept {
        if (use_epoch.load(std::memory_order_relaxed) != epoch) {
            use_epoch.store(epoch, std::memory_order_relaxed);
        }
        return !async_listed.load(std::memory_order_relaxed) &&
               !async_listed.exchange(true, std::memory_order_acq_rel);
    }
    [[nodiscard]] u64 AsyncUseEpoch() const noexcept {
        return use_epoch.load(std::memory_order_relaxed);
    }
    void AsyncUnlist() noexcept {
        async_listed.store(false, std::memory_order_release);
    }
    /// A write the GPU side made itself (command processor writes), with the lock held: its
    /// pages are CPU-modified now, as a write fault would have marked them.
    void NoteHostWrite(VAddr addr, u64 size) {
        if (!async_covered) {
            return; // page protection (a write fault) saw it
        }
        thread_local std::vector<std::pair<VAddr, VAddr>> runs;
        runs.assign(1, {addr, addr + size});
        MarkAsyncWritten(~writeable & ~sync, runs);
    }

    /// bbport (async write tracking), with the lock held: the pages written since the last
    /// scan become CPU-modified, as a write fault would have marked them.
    void ScanAsyncWrites(u64 epoch, u64 now) {
        // Fresh only once the written pages are marked (a thread that finds it fresh reads the
        // CPU bits without the lock).
        ScanAsyncWritesNow();
        scan_tsc.store(now, std::memory_order_relaxed);
        scan_epoch.store(epoch, std::memory_order_release);
    }

    /// Batched scans (MemoryTracker::AsyncScanner), with the lock held: whether the region has
    /// pages a scan must look at; if not, it counts as scanned now.
    bool BeginAsyncScan(u64 epoch, u64 now) {
        RefreshAsyncCoverage();
        if ((~writeable & ~sync).Any()) {
            return true;
        }
        scan_tsc.store(now, std::memory_order_relaxed);
        scan_epoch.store(epoch, std::memory_order_release);
        return false;
    }
    /// After BeginAsyncScan returned true: `runs` are the written pages of a scan covering it.
    void FinishAsyncScan(const std::vector<std::pair<VAddr, VAddr>>& runs, u64 epoch, u64 now) {
        MarkAsyncWritten(~writeable & ~sync, runs);
        scan_tsc.store(now, std::memory_order_relaxed);
        scan_epoch.store(epoch, std::memory_order_release);
    }

    LockType lock;

private:
    void ScanAsyncWritesNow() {
        RefreshAsyncCoverage();
        const RegionBits async_pages = ~writeable & ~sync;
        if (async_pages.None()) {
            return;
        }
        const size_t first = async_pages.FirstRange().first;
        const size_t last = async_pages.LastRange().second;
        thread_local std::vector<std::pair<VAddr, VAddr>> runs;
        runs.clear();
        const u64 t0 = __rdtsc();
        if (!tracker->ScanWritten(cpu_addr + first * TRACKER_BYTES_PER_PAGE,
                                  (last - first) * TRACKER_BYTES_PER_PAGE, runs)) {
            // Not registered any more (an unmap), or the kernel refused: everything counts.
            BbStats::async_scan_failures.fetch_add(1, std::memory_order_relaxed);
            runs.assign(1, {cpu_addr + first * TRACKER_BYTES_PER_PAGE,
                            cpu_addr + last * TRACKER_BYTES_PER_PAGE});
        }
        BbStats::async_scans.fetch_add(1, std::memory_order_relaxed);
        BbStats::async_scan_ticks.fetch_add(__rdtsc() - t0, std::memory_order_relaxed);
        MarkAsyncWritten(async_pages, runs);
    }

    /// Pages of `async_pages` within `runs` (written pages) become CPU-modified.
    void MarkAsyncWritten(const RegionBits& async_pages,
                          const std::vector<std::pair<VAddr, VAddr>>& runs) {
        bool any = false;
        for (const auto& [from, to] : runs) {
            if (to <= cpu_addr || from >= cpu_addr + TRACKER_HIGHER_PAGE_SIZE) {
                continue;
            }
            const size_t start = (std::max(from, cpu_addr) - cpu_addr) / TRACKER_BYTES_PER_PAGE;
            const size_t end = std::min<size_t>(
                Common::DivCeil(to - cpu_addr, TRACKER_BYTES_PER_PAGE), NUM_PAGES_PER_REGION);
            if (start >= end) {
                continue;
            }
            const RegionBits hit(async_pages, start, end);
            if (hit.None()) {
                continue;
            }
            if (!any) {
                NoteCpuBits();
                any = true;
            }
            cpu |= hit;
            for (const auto& [a, b] : hit) {
                BbStats::async_written_pages.fetch_add(b - a, std::memory_order_relaxed);
            }
        }
        if (any) {
            last_cpu_write_frame = BbStats::frame_number.load(std::memory_order_relaxed);
            NotePreuploadChurn();
            UpdateWriteProtection();
        }
    }

    /// Whether the region's memory is registered for async tracking (rechecked when GPU
    /// memory is mapped or unmapped). Where it is not, every protected page is page-protected.
    bool RefreshAsyncCoverage() {
        const u64 generation = tracker->AsyncGeneration();
        if (generation == async_generation) {
            return async_covered;
        }
        async_generation = generation;
        const bool covered = tracker->AsyncCovered(cpu_addr, TRACKER_HIGHER_PAGE_SIZE);
        if (covered == async_covered) {
            return covered;
        }
        if (async_covered) {
            // Unregistered meanwhile: what the still registered part saw is taken first, the
            // rest counts as written (the scan fails there).
            async_covered = false;
            const RegionBits async_pages = ~writeable & ~sync;
            if (async_pages.Any()) {
                thread_local std::vector<std::pair<VAddr, VAddr>> runs;
                runs.clear();
                if (!tracker->ScanWritten(cpu_addr, TRACKER_HIGHER_PAGE_SIZE, runs)) {
                    runs.assign(1, {cpu_addr, cpu_addr + TRACKER_HIGHER_PAGE_SIZE});
                }
                MarkAsyncWritten(async_pages, runs);
            }
        }
        async_covered = covered;
        UpdateWriteProtection();
        return covered;
    }

    /// bbport (async write tracking): protected pages (no CPU modification) are page-protected
    /// (`sync`) where a write must be seen when it happens: pages the GPU wrote (their data is
    /// read back before the guest's bytes land) and memory not registered for async tracking.
    /// The others are left writable and found by ScanAsyncWrites. Pages under images get page
    /// protection from the texture cache's own watchers.
    void UpdateWriteProtection() {
        const RegionBits protected_pages = ~cpu;
        const RegionBits want_sync = async_covered ? (protected_pages & gpu) : protected_pages;
        writeable = cpu;
        RegionBits add = want_sync & ~sync;
        RegionBits remove = sync & ~want_sync;
        sync = want_sync;
        if (add.Any()) {
            tracker->UpdatePageWatchersForRegion<true, false>(cpu_addr, add);
        }
        if (remove.Any()) {
            tracker->UpdatePageWatchersForRegion<false, false>(cpu_addr, remove);
        }
    }

    /**
     * Notify tracker about changes in the CPU tracking state of a word in the buffer
     *
     * @param word_index   Index to the word to notify to the tracker
     * @param current_bits Current state of the word
     * @param new_bits     New state of the word
     *
     * @tparam track True when the tracker should start tracking the new pages
     */
    template <bool track, bool is_read>
    void UpdateProtection() {
        RENDERER_TRACE;
        if constexpr (!is_read) {
            if (tracker->AsyncWrites()) {
                RefreshAsyncCoverage();
                UpdateWriteProtection();
                return;
            }
        }
        // bbport: hot pages stay writable whether or not they are marked modified.
        const RegionBits writable_target = num_hot ? (cpu | (hot & ~gpu)) : cpu;
        RegionBits mask = is_read ? (~gpu ^ readable) : (writable_target ^ writeable);
        if (mask.None()) {
            return;
        }
        if constexpr (is_read) {
            readable = ~gpu;
        } else {
            writeable = writable_target;
        }
        tracker->UpdatePageWatchersForRegion<track, is_read>(cpu_addr, mask);
    }

    // bbport: pages the guest writes again and again (per-frame constants, skinning output)
    // cost a protection fault in the writing thread plus an mprotect with TLB shootdowns on
    // every upload. After HotFaults faults a page stays writable and counts as CPU modified once
    // per guest submission (upload_epoch), so the first use in each submission uploads it. The
    // set is rebuilt every HotPeriod. Opt-in (BB_HOT_PAGES=1). Marked modified again right
    // after every upload instead, it was re-uploaded on every binding: Ratchet & Clank binds its
    // geometry ring whole, ~780 MB of uploads a frame from ~300 hot pages, half the frame rate;
    // in Hunter's Nightmare the set grew to ~14k pages (56 MB), 33 FPS and a GPU ring timeout.
    static bool HotPagesEnabled() {
        static const bool enabled = [] {
            const char* env = std::getenv("BB_HOT_PAGES");
            return env && env[0] == '1';
        }();
        return enabled && !BbToggle::Disabled(BbToggle::HotPages);
    }
    bool HotPagesActive() const {
        return HotPagesEnabled() && !tracker->AsyncWrites();
    }
    // A short period re-protects thousands of pages at once and each faults again before it is
    // hot, a fault storm (a 2 s period gave ~20k faults/s and frame spikes).
    static constexpr u8 HotFaults = 2;
    static constexpr auto HotPeriod = std::chrono::seconds(20);

    void CountWriteFaults(size_t start_page, size_t end_page) {
        const bool hot_enabled = HotPagesActive();
        static const bool streamed_only = Game::HasStreamedMemory();
        for (size_t page = start_page; page < end_page; ++page) {
            if (writeable.Get(page)) {
                continue; // not protected: no fault
            }
            BbStats::tracker_faults.fetch_add(1, std::memory_order_relaxed);
            if (!hot_enabled) {
                continue;
            }
            // With streamed memory named by the game's profile, only pages there, at once.
            if (streamed_only) {
                if (!Game::InStreamedMemory(cpu_addr + page * TRACKER_BYTES_PER_PAGE)) {
                    continue;
                }
            } else if (write_faults[page] < HotFaults) {
                ++write_faults[page];
                continue;
            }
            if (!hot.Get(page)) {
                if (num_hot == 0) {
                    hot_since = std::chrono::steady_clock::now();
                }
                hot.Set(page);
                ++num_hot;
                BbStats::hot_pages.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    /// A new guest submission since the hot pages were last marked: they may have been written
    /// (they are writable), so they count as modified until uploaded again.
    /// Called with the lock held: a fault handler sets bits of `cpu` under it too.
    void RefreshHotPages() {
        if (num_hot == 0) {
            return;
        }
        const u64 epoch = BbStats::upload_epoch.load(std::memory_order_relaxed);
        if (epoch == hot_epoch) {
            return;
        }
        hot_epoch = epoch;
        // Not the pages the GPU wrote since: an upload of the guest's copy would overwrite the
        // GPU's data (Ratchet & Clank's skinning output in its geometry ring).
        NoteCpuBits();
        cpu |= hot & ~gpu;
    }

    [[nodiscard]] bool HotRefreshPending() const noexcept {
        return num_hot != 0 && BbStats::upload_epoch.load(std::memory_order_relaxed) != hot_epoch;
    }

    void KeepHotPagesModified(size_t start_page, size_t end_page) {
        (void)start_page;
        (void)end_page;
        if (num_hot == 0) {
            return;
        }
        if (!HotPagesEnabled() ||
            std::chrono::steady_clock::now() - hot_since > HotPeriod) {
            // Writes since their last upload were not tracked: modified, so the next use uploads
            // them and protects them again. Counting starts again.
            NoteCpuBits();
            cpu |= hot & ~gpu;
            BbStats::hot_pages.fetch_sub(num_hot, std::memory_order_relaxed);
            hot.Clear();
            write_faults.fill(0);
            num_hot = 0;
        }
    }

    /// Set before CPU bits are (with the lock held), so a lock-free reader that sees it clear
    /// sees no CPU-modified page.
    void NoteCpuBits() noexcept {
        if (summary_word && !(summary_word->load(std::memory_order_relaxed) & summary_mask)) {
            summary_word->fetch_or(summary_mask, std::memory_order_release);
        }
    }
    /// With the lock held, after CPU bits were cleared.
    void MaybeClearSummary() noexcept {
        if (summary_word && num_hot == 0 && cpu.None()) {
            summary_word->fetch_and(~summary_mask, std::memory_order_release);
        }
    }

    std::atomic<u64>* summary_word = nullptr;
    u64 summary_mask = 0;
    std::array<u8, NUM_PAGES_PER_REGION> write_faults{};
    RegionBits hot{};
    u32 num_hot = 0;
    u64 hot_epoch = 0; ///< upload_epoch when the hot pages were last marked modified
    std::chrono::steady_clock::time_point hot_since{};

    PageManager* tracker;
    VAddr cpu_addr = 0;
    RegionBits cpu;
    RegionBits gpu;
    RegionBits writeable;
    RegionBits readable;
    /// bbport (async write tracking): pages with page protection from this region (the other
    /// protected ones are found by scans).
    RegionBits sync;
    std::atomic<u64> scan_epoch{0}, scan_tsc{0};
    std::atomic<u64> use_epoch{0};
    std::atomic<bool> async_listed{false};
    u64 async_generation = 0;
    bool async_covered = false;
};

} // namespace VideoCore
