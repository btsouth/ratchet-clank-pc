// SPDX-License-Identifier: GPL-2.0-or-later
// Flip events are cleared on retrieval, including when the flip preceded the wait.
// A missing flip must still trip the guest watchdog, regardless of host compilation.
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>

#include "core/libraries/gnmdriver/gnmdriver.h"
#include "core/libraries/kernel/equeue.h"
#include "core/libraries/videoout/video_out.h"

using namespace Libraries;
using Kernel::OrbisKernelEvent;

namespace {
constexpr u64 FlipId = u64(VideoOut::OrbisVideoOutInternalEventId::Flip);

Kernel::EqueueInternal* FlipQueue(s64 handle) {
    auto* eq = new Kernel::EqueueInternal(handle, "flip");
    Kernel::EqueueEvent event{};
    event.event.ident = FlipId;
    event.event.filter = OrbisKernelEvent::Filter::VideoOut;
    event.event.flags = OrbisKernelEvent::Flags::Add;
    assert(eq->AddEvent(event));
    return eq;
}

void Flip(Kernel::EqueueInternal* eq, u64 arg = 0) {
    assert(eq->TriggerEvent(FlipId, OrbisKernelEvent::Filter::VideoOut,
                            reinterpret_cast<void*>(FlipId | (arg << 16))));
}

int Wait(Kernel::EqueueInternal* eq, u32 micros) {
    OrbisKernelEvent ev[4]{};
    return eq->WaitForEvents(ev, 4, &micros);
}

// LBP3 0x54630..0x5467f: true if the game would trap.
bool Watchdog(Kernel::EqueueInternal* eq) {
    int timeouts = 0;
    while (Wait(eq, 100000) == 0) {
        timeouts += !GnmDriver::sceGnmDriverCaptureInProgress();
        if (timeouts == 10) {
            return true;
        }
    }
    return false;
}
} // namespace

int main() {
    assert(!GnmDriver::sceGnmDriverCaptureInProgress());
    auto* eq = FlipQueue(1);
    // Edge triggered: two flips before a wait are one event, and the next wait times out.
    Flip(eq, 41);
    Flip(eq, 42);
    u32 zero = 0;
    OrbisKernelEvent ev[1]{};
    assert(eq->WaitForEvents(ev, 1, &zero) == 1);
    assert(ev[0].ident == FlipId);
    assert(ev[0].filter == OrbisKernelEvent::Filter::VideoOut);
    assert(ev[0].flags & OrbisKernelEvent::Flags::Clear);
    assert(((ev[0].data >> 12) & 15) == 2);
    assert((ev[0].data >> 16) == 42);
    assert(Wait(eq, 10000) == 0);
    // Count saturates at 15 and restarts from one after retrieval.
    for (int i = 0; i < 20; ++i) {
        Flip(eq, 100 + i);
    }
    assert(eq->WaitForEvents(ev, 1, &zero) == 1);
    assert(((ev[0].data >> 12) & 15) == 15);
    assert((ev[0].data >> 16) == 119);
    Flip(eq, 120);
    assert(eq->WaitForEvents(ev, 1, &zero) == 1);
    assert(((ev[0].data >> 12) & 15) == 1);
    assert((ev[0].data >> 16) == 120);
    assert(eq->WaitForEvents(ev, 1, &zero) == 0);

    // The traced boot ordering: a flip completes before the guest starts waiting.
    // That wait consumes it immediately; the next wait must await a NEW flip.
    Flip(eq);
    assert(Wait(eq, 100000) == 1);
    assert(Wait(eq, 10000) == 0);
    std::thread presenter([eq] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        Flip(eq);
    });
    assert(Wait(eq, 100000) == 1);
    presenter.join();
    assert(Wait(eq, 10000) == 0);

    // No new flip: the real ten-timeout watchdog remains active.
    const auto start = std::chrono::steady_clock::now();
    assert(Watchdog(eq));
    assert(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(1000));

    // Polls (timeout 0) racing triggers: every flip is seen.
    constexpr int Flips = 20000;
    std::atomic<int> seen{0};
    std::thread flipper([&] {
        for (int i = 0; i < Flips; ++i) {
            Flip(eq);
            while (seen.load() <= i) {
                std::this_thread::yield();
            }
        }
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (seen.load() < Flips) {
        assert(std::chrono::steady_clock::now() < deadline);
        if (eq->WaitForEvents(ev, 1, &zero) == 1) {
            seen.fetch_add(1);
        }
    }
    flipper.join();
    assert(eq->WaitForEvents(ev, 1, &zero) == 0);

    std::printf("Flip watchdog: capture remains false, missing flips still trap, "
                "flip events edge triggered, %d polled flips none lost\n",
                Flips);
    return 0;
}
