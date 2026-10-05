// The global resolve readback, held open for a while on behalf of whichever
// feature needs the GPU's result in guest memory right now.
//
// d3d12_readback_resolve copies every resolve of every frame back to guest
// memory, which costs a GPU drain per resolve, so it stays off -- larecomp_app
// sets it false at startup -- and is only turned on around the moments that
// read a GPU result on the CPU:
//
//   * vinyls: the game builds the car's decal texture on the CPU from a GPU
//     composite (vinyl.cpp holds it while that composite runs);
//   * photos, on an SDK without an address-scoped readback: the shot is the
//     front buffer, locked and JPEG-encoded on the CPU (photo_mode.cpp; the
//     fork's SDK reads back just the two front buffers instead).
//
// Each client keeps its own deadline and the cvar goes back off only once all of
// them have run out, so a photo taken during a vinyl composite cannot close the
// window under it, or the other way round. If the user has turned the cvar on
// themselves, none of this touches it.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/cvar.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>

#include "hooks_internal.h"

namespace {

constexpr const char* kReadbackFlag = "d3d12_readback_resolve";

// Hold and Tick come from different guest threads (the vinyl regen hook, the
// photo album update, the frame), and a hold landing between Tick's "all
// expired" and its switch-off would lose the window, so all of it is under one
// lock. A few calls a frame at most.
std::mutex g_mutex;
bool g_forced = false;  // this code turned the flag on and owes the switch-off
std::array<int64_t, size_t(ReadbackClient::kCount)> g_deadline_ns{};

int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

void HoldGlobalReadback(ReadbackClient client, int64_t hold_ns) {
    std::lock_guard lock(g_mutex);
    if (!g_forced) {
        if (rex::cvar::GetFlagByName(kReadbackFlag) == "true") return;  // the user's choice
        // Absent when no emulated GPU is loaded (the native renderer's no-CP
        // mode): nothing to turn on, and nothing to turn back off later.
        if (!rex::cvar::SetFlagByName(kReadbackFlag, "true")) return;
        g_forced = true;
    }
    int64_t& deadline = g_deadline_ns[size_t(client)];
    deadline = std::max(deadline, NowNs() + hold_ns);
}

void TickGlobalReadback() {
    std::lock_guard lock(g_mutex);
    if (!g_forced) return;
    const int64_t now = NowNs();
    for (int64_t deadline : g_deadline_ns) {
        if (now < deadline) return;
    }
    rex::cvar::SetFlagByName(kReadbackFlag, "false");
    g_forced = false;
    g_deadline_ns.fill(0);
}
#else // REXGLUE_HAS_XEO3_TARGET
#include "hooks_internal.h"

void HoldGlobalReadback(ReadbackClient, int64_t) {}
void TickGlobalReadback() {}
#endif // REXGLUE_HAS_XEO3_TARGET
