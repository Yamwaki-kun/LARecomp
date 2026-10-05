// The photo album -- the game's photo "gallery" -- and the photo-mode screens
// around it (photo mode, the Rate My Ride snapshots).
//
// photo_ui_trace logs the UI statechart while the album scene is up.
//
// RE map (default.xex, 04-05/10/2026):
//
// UI plumbing
//   *(0x8286D804)                 mcUIManager. +52 FlashNavigator.
//
// Statechart (tune/ui/*.sc.xml)
//   FlashNavigator + 4            the vhsm machine. +8 registered event objects (count
//                                 @+72), +76 states, +88 state map (atStringHash of the
//                                 id -> index; buckets* @+88, u16 count @+92; entry
//                                 {hash, index, next}), +263088 delayed events.
//   state                         +16 flags (0x80 active, 0x02 enabled), +20 char* id,
//                                 +32 parent, +36 next sibling, +44 first child.
//   event object                  +4 char* name ("photoAlbum", "uinput", "UI", ...),
//                                 +8 command names, +12 event names; vtbl[0] runs a
//                                 command (u16 at +2, args list at +4: {type, value,
//                                 next}).
//   sub_8268EE10(obj, idx, 3, -1) posts an event: machine vtbl[5] (sub_8221F060) ->
//                                 sub_8268EE40, which queues it or calls the dispatch,
//                                 machine vtbl[0] = sub_8221F160(machine, obj, prio,
//                                 &idx, channel).
//   sub_8268ECE0                  throwDelayedEvent.
//   sub_821F8A58                  the "UI" object's commands (Focus, ActivateAndFocus,
//                                 DeactivateAndUnfocus, ...): the argument is a state
//                                 index, no argument means the state running the
//                                 transition (r6).
//   FlashNavigator +263972        3-entry FIFO of modal message boxes (write index
//                                 +263984, read index +263988, count +263992); the
//                                 dispatch offers input to the oldest one first.
//
// Photo album (mcUIPhotoAlbum, the "PhotoAlbumScene" state)
//   sub_8263FA10                  "photoAlbum" commands (names at 0x828260D8).
//   album+456                     its UI state (UIPhotoAlbum); +56 there is the movie
//                                 context (PHOTOALBUMMOVIE, set by sub_8263E9C8).
//   album+588 / +592              snapshot vhsm phase / substate (see photo_mode.cpp).
//   album+1132 / +1136 / +1140    PreviewPicture texture, its texture reference, and the
//                                 1280x720 grcImage every JPEG is decoded into. The
//                                 texture shows that image's memory, so whatever was
//                                 decoded last -- the new snapshot or a slot -- is what
//                                 PreviewPicture shows.
//   album+1148                    the new snapshot's JPEG (409600 bytes).
//   album+1170                    "saving a snapshot": set by SaveSnapshot and
//                                 SetSavingSnapshot, cleared when it is saved
//                                 (sub_8263DCF8) or cancelled.
//   album+1180                    current slot; album+428 the album save buffer (slot n's
//                                 JPEG at +204 + 204800 * n).
//   sub_8263C8B0                  decodes the new snapshot into PreviewPicture (phase 4
//                                 of the capture). Also sets *(dword_8288B9AC)+68 = 5:
//                                 five frames of whatever sub_82304428 draws while the
//                                 capture settles.
//   sub_8263C9C8(album, slot)     decodes the slot's photo into PreviewPicture, or sets
//                                 the movie's empty_slot when the slot is empty.
//   sub_8263DFC0(album, slot)     SetPreviewPicture, run every time the cursor lands on
//                                 a slot: empty_slot, preview_letterbox, then
//                                 sub_8263C9C8.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/cvar.h>
#include <rex/ppc.h>
#include <rex/runtime.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include "hooks.h"
#include "hooks_internal.h"
#include "../logging.h"

REXCVAR_DEFINE_BOOL(photo_ui_trace, true, "MCLA/Diagnostics",
    "Log the UI statechart while the photo album scene is up (photo mode, the album, "
    "the Rate My Ride snapshots): events posted and dispatched, delayed events, the "
    "UI.* and photoAlbum.* commands with the states they touch. Silent everywhere else.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REX_EXTERN(__imp__rex_sub_8221F160);  // FlashNavigator dispatch
REX_EXTERN(__imp__rex_sub_8268EE40);  // vhsm post
REX_EXTERN(__imp__rex_sub_8268ECE0);  // vhsm throwDelayedEvent
REX_EXTERN(__imp__rex_sub_821F8A58);  // "UI" commands
REX_EXTERN(__imp__rex_sub_8263FA10);  // "photoAlbum" commands

namespace {

constexpr uint32_t kUiManagerPtr   = 0x8286D804;
constexpr uint32_t kMgrNavigator   = 52;

constexpr uint32_t kMachineObjects     = 8;
constexpr uint32_t kMachineObjectCount = 72;
constexpr uint32_t kMachineStates      = 76;
constexpr uint32_t kMachineStateMap    = 88;

constexpr uint32_t kStateFlags     = 16;
constexpr uint32_t kStateName      = 20;
constexpr uint32_t kStateActive    = 0x80;

constexpr uint32_t kNavLayers      = 263972;
constexpr uint32_t kNavLayerRead   = 263988;
constexpr uint32_t kNavLayerCount  = 263992;

uint32_t R32(const uint8_t* base, uint32_t ea) { return ea ? ReadGuestU32(base, ea) : 0; }
uint16_t R16(const uint8_t* base, uint32_t ea) {
    return ea ? uint16_t((uint32_t(base[ea]) << 8) | base[ea + 1]) : 0;
}

// A guest C string, or "" when the pointer is null or the bytes are not text.
std::string GuestString(const uint8_t* base, uint32_t ea, size_t max = 48) {
    if (ea < 0x10000u) return {};
    const char* s = reinterpret_cast<const char*>(base + ea);
    size_t n = 0;
    while (n < max && s[n]) {
        const unsigned char c = static_cast<unsigned char>(s[n]);
        if (c < 0x20 || c > 0x7E) return {};
        ++n;
    }
    return std::string(s, n);
}

std::string Hex(uint32_t v) {
    char buf[12];
    std::snprintf(buf, sizeof(buf), "%08X", v);
    return buf;
}

// rage::atStringHash (sub_821C9790): one-at-a-time over the lower-cased name.
uint32_t AtStringHash(std::string_view s) {
    uint32_t h = 0;
    for (char ch : s) {
        uint8_t c = static_cast<uint8_t>(ch);
        if (c >= 'A' && c <= 'Z') c = uint8_t(c + 32);
        else if (c == '\\') c = '/';
        h += c;
        h += h << 10;
        h ^= h >> 6;
    }
    h += h << 3;
    h ^= h >> 11;
    h += h << 15;
    return h;
}

uint32_t Navigator(const uint8_t* base) {
    const uint32_t mgr = R32(base, kUiManagerPtr);
    return mgr ? R32(base, mgr + kMgrNavigator) : 0;
}

// The machine's state with this id (sub_82691650), 0 when there is none.
uint32_t FindState(const uint8_t* base, uint32_t machine, std::string_view id) {
    const uint32_t buckets = R32(base, machine + kMachineStateMap);
    const uint16_t count = R16(base, machine + kMachineStateMap + 4);
    const uint32_t states = R32(base, machine + kMachineStates);
    if (!buckets || !count || !states) return 0;
    const uint32_t hash = AtStringHash(id);
    for (uint32_t e = R32(base, buckets + 4 * (hash % count)), guard = 0; e && guard < 256;
         e = R32(base, e + 8), ++guard) {
        if (R32(base, e) != hash) continue;
        const uint32_t index = R32(base, e + 4);
        return index < 0x10000u ? R32(base, states + 4 * index) : 0;
    }
    return 0;
}

std::string StateName(const uint8_t* base, uint32_t state) {
    std::string name = state ? GuestString(base, R32(base, state + kStateName), 64) : "";
    return name.empty() ? Hex(state) : name;
}

// --------------------------------------------------------------------------
// Statechart trace (photo_ui_trace)
// --------------------------------------------------------------------------

// Only while PhotoAlbumScene is active: photo mode, the album, the Rate My Ride
// snapshots and their dialogs all live under it.
bool Tracing(const uint8_t* base) {
    if (!REXCVAR_GET(photo_ui_trace)) return false;
    // Posts also come from the render thread (the capture), hence the atomics.
    static std::atomic<uint32_t> cached_machine{0};
    static std::atomic<uint32_t> scene{0};
    const uint32_t nav = Navigator(base);
    if (!nav) return false;
    const uint32_t machine = nav + 4;
    if (machine != cached_machine.load(std::memory_order_relaxed)) {
        scene.store(FindState(base, machine, "PhotoAlbumScene"), std::memory_order_relaxed);
        cached_machine.store(machine, std::memory_order_relaxed);
    }
    const uint32_t s = scene.load(std::memory_order_relaxed);
    return s && (R32(base, s + kStateFlags) & kStateActive);
}

std::string EventName(const uint8_t* base, uint32_t obj, uint32_t idx) {
    std::string name = GuestString(base, R32(base, obj + 4), 32);
    if (name.empty()) name = Hex(obj);
    std::string ev;
    const uint32_t table = R32(base, obj + 12);
    if (table && idx < 128) ev = GuestString(base, R32(base, table + 4 * idx), 40);
    if (ev.empty()) ev = "#" + std::to_string(idx);
    return name + "." + ev;
}

std::string CommandName(const uint8_t* base, uint32_t obj, uint32_t cmd) {
    std::string name = GuestString(base, R32(base, obj + 4), 32);
    std::string c;
    const uint32_t table = R32(base, obj + 8);
    if (table && cmd < 128) c = GuestString(base, R32(base, table + 4 * cmd), 40);
    if (c.empty()) c = "#" + std::to_string(cmd);
    return name + "." + c;
}

std::string LayerFifo(const uint8_t* base, uint32_t nav) {
    const uint32_t count = R32(base, nav + kNavLayerCount);
    if (!count) return "[]";
    std::string out = "[";
    uint32_t read = R32(base, nav + kNavLayerRead);
    for (uint32_t i = 0; i < count && i < 3; ++i) {
        read = (read + 1) % 3;
        if (i) out += ", ";
        out += StateName(base, R32(base, nav + kNavLayers + 4 * read));
    }
    return out + "]";
}

// Analog input arrives every frame while a stick is held, and the held-button
// repeats ("uinput.action") many times a second: both would drown the rest.
bool NoisyEvent(const std::string& ev) {
    return ev.find("Analog") != std::string::npos || ev == "uinput.action" ||
           ev == "uinput.cancel" || ev.find(".#") != std::string::npos;
}

}  // namespace

// ---------------------------------------------------------------------------
// Trace
// ---------------------------------------------------------------------------

// FlashNavigator's dispatch: (machine, obj, prio, &idx, channel). Every event is
// dispatched once per channel; channel 0 is enough to follow the flow.
extern "C" REX_FUNC(rex_sub_8221F160) {
    if (int8_t(ctx.r7.u8) == 0 && Tracing(base)) {
        const uint32_t machine = ctx.r3.u32;
        uint32_t idx = R32(base, ctx.r6.u32);
        if (idx > 0xFFFFu) idx &= 0xFFFFu;
        const std::string ev = EventName(base, ctx.r4.u32, idx);
        if (!NoisyEvent(ev))
            MC_INFO("[photo-ui] dispatch {} layers={}", ev, LayerFifo(base, machine - 4));
    }
    __imp__rex_sub_8221F160(ctx, base);
}

// Post: (machine, obj, prio, idx, channel, immediate).
extern "C" REX_FUNC(rex_sub_8268EE40) {
    if (ctx.r8.u8 == 0 && Tracing(base)) {  // the immediate re-posts repeat the queued one
        const std::string ev = EventName(base, ctx.r4.u32, ctx.r6.u32);
        if (!NoisyEvent(ev))
            MC_INFO("[photo-ui] post {} from {:08X}", ev, uint32_t(ctx.lr));
    }
    __imp__rex_sub_8268EE40(ctx, base);
}

// throwDelayedEvent: (machine, idx | object index << 16, delay in f1).
extern "C" REX_FUNC(rex_sub_8268ECE0) {
    if (Tracing(base)) {
        const uint32_t machine = ctx.r3.u32;
        const uint32_t packed = ctx.r4.u32;
        const uint32_t index = packed >> 16;
        const uint32_t obj = index < R32(base, machine + kMachineObjectCount)
                                 ? R32(base, machine + kMachineObjects + 4 * index)
                                 : 0;
        MC_INFO("[photo-ui] post-delayed {} in {:.2f}s from {:08X}",
                obj ? EventName(base, obj, packed & 0xFFFFu) : Hex(packed), ctx.f1.f64,
                uint32_t(ctx.lr));
    }
    __imp__rex_sub_8268ECE0(ctx, base);
}

// "UI" commands: (obj, machine, cmd, current state).
extern "C" REX_FUNC(rex_sub_821F8A58) {
    if (Tracing(base)) {
        const uint32_t machine = ctx.r4.u32;
        const uint32_t cmd = ctx.r5.u32;
        const uint32_t arg = R32(base, cmd + 4);
        std::string target;
        if (arg) {
            const uint32_t index = R32(base, arg + 4);
            const uint32_t states = R32(base, machine + kMachineStates);
            target = index < 0x10000u && states ? StateName(base, R32(base, states + 4 * index))
                                                : "#" + std::to_string(index);
        } else {
            target = "self " + StateName(base, ctx.r6.u32);
        }
        MC_INFO("[photo-ui] {}({}) layers={}", CommandName(base, ctx.r3.u32, R16(base, cmd + 2)),
                target, LayerFifo(base, machine - 4));
    }
    __imp__rex_sub_821F8A58(ctx, base);
}

// "photoAlbum" commands: (obj, ?, cmd).
extern "C" REX_FUNC(rex_sub_8263FA10) {
    if (Tracing(base))
        MC_INFO("[photo-ui] {}", CommandName(base, ctx.r3.u32, R16(base, ctx.r5.u32 + 2)));
    __imp__rex_sub_8263FA10(ctx, base);
}

#endif  // REXGLUE_HAS_XEO3_TARGET
