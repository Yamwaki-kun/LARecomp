#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/cvar.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif
#include <rex/chrono/clock.h>
#include <rex/runtime.h>
#include <rex/perf/counter.h>
#include "../guest_profiler.h"
#include "../draw_stats.h"
#include <rex/system/xmemory.h>
#include <rex/graphics/xenos.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/window.h>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif
#include "imgui.h"
#include "../logging.h"
#include "hooks.h"
#include "hooks_internal.h"
#include "discord_rpc/discord_rpc.h"
#include "../graphics_button.h"
#include "larecomp_log.h"
#include "../menu_camera.h"
#include "../modloader/modloader.h"
#include "../mp3custom/mp3custom.h"
#include "../hud_units.h"
#include "../cutscene_gallery.h"
#include "../map_mouse.h"
#include "../modloader/features/mod_breakables.h"
#include "../modloader/features/mod_glows.h"
#include "../camera_look.h"
#include "../texture_dump.h"
#include "../online/online_common.h"  // shared guest-memory helpers (IsGuestPtr, ...)

REXCVAR_DECLARE(std::string, aspect_ratio);  // graphics.cpp

// CVAR DEFINITIONS (Will appear in F4 menu)
// The '.lifecycle(kRequiresRestart)' forces the user to restart the game if they change the value.

// Diagnostic: dumps every tune field as it registers. Off by default -- it fires for
// every tune in the game (about 1100 unique names across ~28 classes) -- but it is the
// only way to see a tune's live layout, so it stays.
REXCVAR_DEFINE_BOOL(tune_field_probe, false, "MCLA/Diagnostics",
                    "Log every tune field name as it registers.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// NOTE: the online cvars (online_ignore_content_check, online_diag) moved to
// src/mc_engine/online/system_link.cpp along with the hooks that use them.

REXCVAR_DEFINE_BOOL(dbg_print, false, "MCLA/Patches", "Enable DbgPrint console outputs.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(lzx_stats, false, "MCLA/Debug",
    "Measure pgStreamer LZX decompression (XMemDecompressStream): per-2s window stats "
    "appended to <exe>/lzx_stats.txt. For diagnosing streaming stutter (South Central).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(extra_vinyl_layers, false, "MCLA/Garage",
    "Raise the front/rear BUMPER vinyl caps from 16 to 31 layers each "
    "(64/64/64/31/31). Top and side caps stay at 64 because MCLA's vinyl "
    "composite pipeline hard-caps a single surface at 64 layers (fixed-64 "
    "work-area buffers; >64 overflows and crashes). Bumpers stay <=64 and keep "
    "their 5-bit save field, so existing garage/online cars are 100% compatible.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(export_vinyl, false, "MCLA/Garage",
    "Export the current car's vinyl layers to a .vgp file in <exe dir>/vinyls/ "
    "(auto-named vinyl_<car>_<timestamp>.vgp). Toggle ON while in the garage with "
    "a car loaded; it fires once and flips back OFF. Share the file; import later.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(import_vinyl, "", "MCLA/Garage",
    "Import a .vgp vinyl package onto the current car. Type the file name (found "
    "in <exe dir>/vinyls/, with or without the .vgp extension) and press Enter "
    "while in the garage with a car loaded. It overwrites the car's current vinyl "
    "layers and re-composites, then clears the field. Layers past the active "
    "per-surface cap are dropped (enable extra_vinyl_layers first for 31 bumper "
    "slots).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(vinyl_auto_readback, true, "MCLA/Garage",
    "Fix: car vinyls only load outside the Vinyl Editor when GPU readback is on "
    "(the game builds the decal texture on the CPU from a GPU composite). This "
    "briefly enables d3d12_readback_resolve for ~1.5s around each vinyl "
    "(re)composite (garage entry / edits) so decals appear everywhere, while "
    "keeping readback OFF during racing for full performance. Leave ON.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(photo_auto_readback, true, "MCLA/PhotoMode",
    "Fix: photo mode previews show an old frame or garbage. The game takes its "
    "picture on the CPU, by locking the front buffer and JPEG-encoding it, so it "
    "needs the GPU resolve copied back to guest memory. This asks for a readback "
    "of just the two front buffers for a few frames around each shot, instead of "
    "the global readback_resolve cvar which stalls every resolve of every frame. "
    "Leave ON.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(photo_readback_debug, false, "MCLA/PhotoMode",
    "Diagnostics for photo_auto_readback: logs the photo album state machine's "
    "phase transitions and the front buffer addresses the readback is armed for. "
    "Pair with the GPU-side gpu_log_resolve_readback_misses to see whether the "
    "resolve that fills the front buffer actually lands in the armed range.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(dump_vinyl_shapes, false, "MCLA/Garage",
    "Dump the vinyl shape catalog (every ShapeIdx -> source bitmap filename + "
    "texture header) to <exe dir>/vinyls/vinyl_shapes.json. Enter the Vinyl editor "
    "once so the shape library loads, then toggle ON; it fires once and flips back "
    "OFF. Used to build the image->vinyl decomposer's brush set.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(capture_vinyl_shapes, false, "MCLA/Garage",
    "Hands-free: force-load every vinyl shape a few at a time, hash each, and "
    "build the full ShapeIdx->hash map in vinyls/vinyl_shape_hashes.txt (+ .json) "
    "— no manual browsing. Enter the Vinyl editor (main menu, not a picker grid), "
    "then toggle ON; it sweeps all ~894 shapes over a few seconds and logs when "
    "done. Join the hashes to the DDS the texture dumper already wrote.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Vinyl (decal) layer caps. MCLA stores car decals in a fixed 288-slot layer
// array (each layer = 20 bytes) partitioned across 5 surfaces by two parallel
// 5-entry tables in guest .data/.rdata:
//   dword_820510B0 @ 0x820510B0 = per-surface capacity  {64,64,64,16,16}
//   dword_827E9770 @ 0x827E9770 = per-surface start slot {0,64,128,192,208}
// with start[i+1] = start[i] + cap[i]. Every accessor (sub_82393118 &c.) reads
// these live, and the work-area ctor (sub_82370C38) copies the caps + allocates
// per-surface used-buffers of cap[i] bytes ONCE at garage init — so this patch
// must land before the garage is entered (InitHooks, at startup) and requires a
// restart to take effect.
//
// New layout 85/85/86/16/16 (offsets 0/85/170/256/272) sums to exactly 288, so
// it fills the existing array without any reallocation or struct growth. The
// per-surface save COUNT is serialized in bit-width(cap) bits (sub_82395A50 /
// sub_823955C8): 85/86 keep the 7-bit field of 64, and the bumpers stay 16 (5
// bits), so the save/online bitstream layout is byte-identical -> existing
// garage cars still load. Only the raw "UserVinylData" package blob is stored by
// absolute slot; saved packages read shifted until re-saved (garage cars are fine).
static void ApplyVinylLayerCaps() {
    if (!REXCVAR_GET(extra_vinyl_layers)) return;

    auto* rt = rex::Runtime::instance();
    if (!rt) return;
    auto* mem = rt->memory();
    if (!mem) return;

    // dword_820510B0 (the capacity table) lives in the XEX read-only data
    // section — the loader maps XEX_SECTION_READONLY_DATA read-only
    // (xex_module.cpp), so a raw store there faults (0xC0000005). Flip the
    // containing page to read/write first. dword_827E9770 is in .data and
    // already writable; unprotecting it too is a harmless no-op. (Switching the
    // base pointer does NOT help — the fault is page protection, not the base.)
    auto make_writable = [&](uint32_t addr) {
        if (auto* heap = mem->LookupHeap(addr)) {
            heap->Protect(addr, 5 * sizeof(uint32_t),
                          rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite);
        }
    };

    auto write_table = [&](uint32_t addr, const uint32_t (&vals)[5]) {
        auto* p = mem->TranslateVirtual<uint8_t*>(addr);
        for (int i = 0; i < 5; ++i) {
            uint32_t v = vals[i];
            p[i * 4 + 0] = (v >> 24) & 0xFF;  // guest memory is big-endian
            p[i * 4 + 1] = (v >> 16) & 0xFF;
            p[i * 4 + 2] = (v >> 8) & 0xFF;
            p[i * 4 + 3] = v & 0xFF;
        }
    };

    // Per-surface caps. IMPORTANT: MCLA's vinyl COMPOSITE pipeline has a hard
    // 64-layers-per-surface limit — the work-area buffers at wa+48 (malloc 256 =
    // 64 dwords) and wa+52 (malloc 64) are fixed-64 and indexed by layer up to
    // cap[surf] (sub_8236EA38), so any surface with cap > 64 overflows them and
    // crashes on the vinyl-layer menu / regen. So top/sides stay at 64. Bumpers
    // (surfaces 3,4) go 16 -> 31: still <= 64 (composite-safe) and still 5-bit
    // (16..31), so the save bitstream stays identical -> fully compatible.
    static constexpr uint32_t kCaps[5]    = {64, 64, 64, 31, 31};   // dword_820510B0
    static constexpr uint32_t kOffsets[5] = {0, 64, 128, 192, 223}; // dword_827E9770

    make_writable(0x820510B0);
    make_writable(0x827E9770);
    write_table(0x820510B0, kCaps);
    write_table(0x827E9770, kOffsets);

    LARECOMP_APP_INFO(
        "[Vinyl] Raised bumper layer caps to 64/64/64/31/31 (composite-safe, save-compatible).");
}

// ===========================================================================
// Vinyl exporter (phase 1 of custom-package import/export)
// ===========================================================================
//
// Layout (verified in IDA, default.xex): the current car's vinyl layers live in
// a heap "config block" = *(paint + 132); container = block + 64; the layer
// array starts at container + 2244. 5 surfaces (top, side1, side2, front bumper,
// rear bumper). Per-surface capacity table dword_820510B0 @ 0x820510B0, start-
// offset table dword_827E9770 @ 0x827E9770. Layer slot = array + 20*(offset[s]
// + localIdx); a slot is USED when its u16 shape-id at +16 != 0xFFFF. Each layer
// is a fixed 20-byte struct (half-float pos/scale/rot/skew + packed RGBA + flags)
// — copied verbatim, so export/import is lossless within the same game build.

// Current car's vinyl regen args, cached by Hook_CacheVinylPaint at the regen
// entry (sub_8236D850): work-area, paint object, composite texture, player index.
static std::atomic<uint32_t> g_vinyl_wa{0};
static std::atomic<uint32_t> g_vinyl_paint{0};
static std::atomic<uint32_t> g_vinyl_tex{0};
static std::atomic<uint32_t> g_vinyl_player{0};

// Windowed GPU readback around vinyl (re)composites. MCLA builds the car's decal
// texture on the CPU from a GPU composite; without readback the CPU reads stale
// physical RAM, so vinyls only show inside the Vinyl Editor. We flip
// d3d12_readback_resolve on for ~1.5s around each regen (the composite + CPU copy
// finish well within that), then off again — so racing keeps full performance.
static std::atomic<int64_t> g_vinyl_rb_deadline_ns{0};
static std::atomic_bool g_vinyl_rb_forced{false};

static int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Returns whether the guest vinyl composite is still running. The per-frame
// driver sub_8235AC78 advances a state machine on the car-model flags
// (+604/+605/+606/+607/+608/+6486) until they all clear = done, and each stage
// waits on the work-area "stage pending" byte wa+248. The full composite of all
// surfaces/layers spans many frames, so we hold readback until it's actually
// idle instead of a fixed guess. Self-contained (direct guest reads).
static bool VinylCompositeBusy() {
    auto* rt = rex::Runtime::instance();
    if (!rt) return false;
    auto* mem = rt->memory();
    if (!mem) return false;
    auto rd8 = [&](uint32_t a) -> uint8_t { return *mem->TranslateVirtual<const uint8_t*>(a); };
    auto is_ptr = [](uint32_t ea) { return ea >= 0x10000u && ea < 0xFFFF0000u; };

    uint32_t wa = g_vinyl_wa.load(std::memory_order_relaxed);
    // wa+248 = a regen stage's GPU work pending; wa+1696 = the composite-busy
    // byte the driver itself gates stages on (sub_8236DB68).
    if (is_ptr(wa) && (rd8(wa + 248) || rd8(wa + 1696))) return true;

    const auto* p = mem->TranslateVirtual<const uint8_t*>(0x8288DCF8);  // current mcCarModel
    uint32_t car = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    if (!is_ptr(car)) return false;
    // Pipeline-stage flags (sub_8235AC78): full +608/+604/+605/+607/+6486 and the
    // partial-update path +606 (sub_8236D2A0, which we don't hook) — polling here
    // catches every composite trigger, including the last surface / rear bumper.
    return rd8(car + 604) || rd8(car + 605) || rd8(car + 606) || rd8(car + 607) ||
           rd8(car + 608) || rd8(car + 6486);
}

// Called from the regen hook: (re)arm the readback window. Only manages the cvar
// when the user hasn't already turned global readback on themselves.
static void ArmVinylReadbackWindow() {
    if (!REXCVAR_GET(vinyl_auto_readback)) return;
    if (!g_vinyl_rb_forced.load(std::memory_order_relaxed)) {
        if (rex::cvar::GetFlagByName("d3d12_readback_resolve") == "true") return;  // user's choice
        rex::cvar::SetFlagByName("d3d12_readback_resolve", "true");
        g_vinyl_rb_forced.store(true, std::memory_order_relaxed);
    }
    // Generous bridge until the state machine spins up; TickVinylReadbackWindow
    // then keeps it alive for as long as the composite actually runs.
    g_vinyl_rb_deadline_ns.store(NowNs() + 2'000'000'000LL, std::memory_order_relaxed);  // +2s
}

// Called every frame from Patch_DeltaTimePre. Runs regardless of how the
// composite was triggered: whenever the guest state machine is busy it (re)opens
// the readback window; it closes ~1.5s after the composite goes idle. This
// catches partial updates and late/last-surface composites (e.g. rear bumper)
// that the sub_8236D850 hook alone can miss, without leaving readback on during
// racing (flags stay 0 when no vinyl work is queued).
static void TickVinylReadbackWindow() {
    if (VinylCompositeBusy() && REXCVAR_GET(vinyl_auto_readback)) {
        if (!g_vinyl_rb_forced.load(std::memory_order_relaxed) &&
            rex::cvar::GetFlagByName("d3d12_readback_resolve") != "true") {
            rex::cvar::SetFlagByName("d3d12_readback_resolve", "true");
            g_vinyl_rb_forced.store(true, std::memory_order_relaxed);
        }
        g_vinyl_rb_deadline_ns.store(NowNs() + 1'500'000'000LL, std::memory_order_relaxed);
    }
    if (g_vinyl_rb_forced.load(std::memory_order_relaxed) &&
        NowNs() >= g_vinyl_rb_deadline_ns.load(std::memory_order_relaxed)) {
        rex::cvar::SetFlagByName("d3d12_readback_resolve", "false");
        g_vinyl_rb_forced.store(false, std::memory_order_relaxed);
    }
}

// ---------------------------------------------------------------------------
// Photo mode: front buffer readback
// ---------------------------------------------------------------------------
// Photo mode never touches the GPU for its picture. sub_82178B20 locks the front
// buffer texture dword_828393AC[dword_82839374] with LockRect, untiles it with
// XGUntileSurface, endian-swaps it, and hands the raw pixels to libjpeg
// (sub_8263C728, quality 85). Both the discard/save preview and the saved slot
// are decoded back from that one JPEG, so if the locked guest memory is stale
// the preview shows an old frame or garbage.
//
// The front buffer is only ever filled by the end-of-frame EDRAM resolve in
// sub_8217B7B0, which the emulator keeps GPU-side. Instead of turning on the
// global readback_resolve cvar (every resolve, every frame, full GPU drain), arm
// an address-scoped request for just the two front buffers while the photo state
// machine winds up to the capture.

// Reads the base address out of a guest D3DTexture's GPU fetch constant, the
// same field sub_82410440 feeds to LockRect: dword 1 of the fetch constant
// (texture + 32), whose top 20 bits are the base address in 4 KB pages.
//
// That address lives in the 0xE0000000 physical alias window, not in the
// physical space the resolve reports, so it still has to go through
// GetPhysicalAddress: 0xE7C47000 -> 0x07C48000 (mask to 0x1FFFFFFF plus the
// 0x1000 offset the 0xE0 heap is mapped at). Returns 0 if the pointer isn't a
// plausible object or the address isn't in a physical heap.
static uint32_t GuestTextureBaseAddress(uint32_t texture_ea) {
    if (texture_ea < 0x10000u || texture_ea >= 0xFFFF0000u) return 0;
    auto* rt = rex::Runtime::instance();
    if (!rt) return 0;
    auto* mem = rt->memory();
    if (!mem) return 0;
    const auto* p = mem->TranslateVirtual<const uint8_t*>(texture_ea + 32);
    if (!p) return 0;
    uint32_t fetch_dword =
        (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    uint32_t base_ea = fetch_dword & 0xFFFFF000u;
    if (!base_ea) return 0;
    uint32_t physical = mem->GetPhysicalAddress(base_ea);
    return physical == UINT32_MAX ? 0 : physical;
}

// Arms readback for both entries of the front buffer array. The game alternates
// dword_82839374 on every swap and the capture locks whichever is current, so
// covering only one of them would be a coin flip.
static void ArmPhotoFrontBufferReadback() {
    if (!REXCVAR_GET(photo_auto_readback)) return;
    auto* rt = rex::Runtime::instance();
    if (!rt) return;
    auto* mem = rt->memory();
    auto* gfx = rt->graphics_system();
    if (!mem || !gfx) return;

    const auto* array = mem->TranslateVirtual<const uint8_t*>(0x828393ACu);  // front buffers[2]
    if (!array) return;

    // 1280x720 at 32 bpp tiled is 3768320 bytes (the resolve length the GPU
    // reports); 4 MB covers it without decoding the pitch out of the fetch
    // constant. The slack matters: MCLA resolves each frame as three tiled
    // bands into the same buffer (predicated tiling, sub_8241C308), so the
    // request has to span all three, not just the base one.
    constexpr uint32_t kFrontBufferSpan = 4u * 1024u * 1024u;
    // The grab fires a few frames after the phase we arm on, and the resolve
    // that fills the buffer happened the frame before that.
    constexpr uint32_t kArmedFrames = 8;

    uint32_t bases[2] = {0, 0};
    for (int i = 0; i < 2; ++i) {
        const uint8_t* e = array + i * 4;
        uint32_t texture_ea =
            (uint32_t(e[0]) << 24) | (uint32_t(e[1]) << 16) | (uint32_t(e[2]) << 8) | e[3];
        bases[i] = GuestTextureBaseAddress(texture_ea);
        if (bases[i]) {
            gfx->RequestResolveReadback(bases[i], kFrontBufferSpan, kArmedFrames);
        }
    }
    if (REXCVAR_GET(photo_readback_debug)) {
        static uint32_t last_logged[2] = {0, 0};
        if (bases[0] != last_logged[0] || bases[1] != last_logged[1]) {
            last_logged[0] = bases[0];
            last_logged[1] = bases[1];
            MC_INFO("photo: armed front buffer readback, base0={:08X} base1={:08X} span={} KB",
                    bases[0], bases[1], kFrontBufferSpan >> 10);
        }
    }
}

// Entry hook on sub_8263CB78, the photo album vhsm update. r3 is the state
// object (photo album object + 456); its phase lives at +132:
//   3 = capture pipeline running (substate 9 grabs, 12 encodes, 10 finishes)
//   4 = decode the fresh JPEG into the PreviewPicture texture
//   5/6 = fade/settle frames before the grab is triggered
// Arming from 3/5/6 puts the readback window several frames ahead of the
// LockRect in sub_82178B20, which is what the front buffer resolve needs.
void Hook_PhotoModeCapture(PPCRegister& r3) {
    auto* rt = rex::Runtime::instance();
    if (!rt) return;
    auto* mem = rt->memory();
    if (!mem) return;
    uint32_t state_ea = static_cast<uint32_t>(r3.u64);
    if (state_ea < 0x10000u || state_ea >= 0xFFFF0000u) return;
    const auto* p = mem->TranslateVirtual<const uint8_t*>(state_ea + 132);
    if (!p) return;
    uint32_t phase = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    if (REXCVAR_GET(photo_readback_debug)) {
        static uint32_t last_phase = 0xFFFFFFFFu;
        if (phase != last_phase) {
            last_phase = phase;
            const auto* s = mem->TranslateVirtual<const uint8_t*>(state_ea + 136);
            uint32_t substate =
                s ? ((uint32_t(s[0]) << 24) | (uint32_t(s[1]) << 16) | (uint32_t(s[2]) << 8) | s[3])
                  : 0;
            MC_INFO("photo: vhsm phase={} substate={} (state={:08X})", phase, substate, state_ea);
        }
    }
    // Arm across the whole snapshot approach, not just the phase that grabs.
    // The grab in phase 3 substate 9 runs inside this very call, so a request
    // armed there is already too late for it - only the resolves of earlier
    // frames can still fill the buffer it locks.
    if (phase >= 2 && phase <= 6) {
        ArmPhotoFrontBufferReadback();
    }
}

void Hook_CacheVinylPaint(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6) {
    g_vinyl_wa.store(static_cast<uint32_t>(r3.u64), std::memory_order_relaxed);
    g_vinyl_paint.store(static_cast<uint32_t>(r4.u64), std::memory_order_relaxed);
    g_vinyl_tex.store(static_cast<uint32_t>(r5.u64), std::memory_order_relaxed);
    g_vinyl_player.store(static_cast<uint32_t>(r6.u64), std::memory_order_relaxed);
    ArmVinylReadbackWindow();
    static std::atomic_bool logged{false};
    if (!logged.exchange(true)) {
        LARECOMP_APP_INFO("[Vinyl] regen hook fired, wa=0x{:08X} paint=0x{:08X} tex=0x{:08X} player={}",
                          static_cast<uint32_t>(r3.u64), static_cast<uint32_t>(r4.u64),
                          static_cast<uint32_t>(r5.u64), static_cast<uint32_t>(r6.u64));
    }
}

namespace {

constexpr uint32_t kVgpMagic = 0x47565852;  // "RXVG"
constexpr uint32_t kVgpVersion = 1;
constexpr uint32_t kVinylSurfaces = 5;
constexpr uint32_t kLayerStride = 20;
constexpr uint32_t kCapsTable = 0x820510B0;
constexpr uint32_t kOffsetsTable = 0x827E9770;

// mcCarVinylShapeLibrary (dword_8288DFC4) + its shape database (dword_828CD0F8).
// Per raster category cat (0..22): count = lib[3+cat] (dword @ lib+12+4*cat),
// descriptor table = lib[26+cat] (16 bytes/entry: +0 shapeObj, +8 flags), source
// filename = names[local] where names = *(db + 96 + 8*cat). ShapeIdx = cat*1000+local.
constexpr uint32_t kShapeLib = 0x8288DFC4;
constexpr uint32_t kShapeDb = 0x828CD0F8;
constexpr uint32_t kShapeCats = 23;

// The guest-memory helpers (IsGuestPtr, GuestRead/Write*, GuestCStr) moved to
// online/online_common.cpp so the online translation units can link them too;
// online_common.h (included above) declares them. They keep external linkage.

// Resolves the current garage car's paint object: the hook-cached value first,
// else the live mcCarModel global (dword_8288DCF8; carModel+20 = paint). Returns
// 0 if no car is loaded. `src` (optional) is set to which source succeeded.
uint32_t ResolveCurrentPaint(rex::memory::Memory* mem, const char** src = nullptr) {
    uint32_t paint = g_vinyl_paint.load(std::memory_order_relaxed);
    if (IsGuestPtr(paint)) { if (src) *src = "hook"; return paint; }
    uint32_t car_model = GuestRead32(mem, 0x8288DCF8);
    if (IsGuestPtr(car_model)) {
        paint = GuestRead32(mem, car_model + 20);
        if (IsGuestPtr(paint)) { if (src) *src = "carmodel"; return paint; }
    }
    return 0;
}

// Reads the car name C-string at *(paint+76)+8; sanitizes to a filename token.
std::string ReadCarName(rex::memory::Memory* mem, uint32_t paint) {
    std::string name;
    uint32_t p76 = GuestRead32(mem, paint + 76);
    if (IsGuestPtr(p76)) {
        uint32_t str_addr = p76 + 8;
        const auto* s = mem->TranslateVirtual<const char*>(str_addr);
        for (int i = 0; i < 48 && s[i]; ++i) {
            char c = s[i];
            name += (std::isalnum(static_cast<unsigned char>(c))) ? c : '_';
        }
    }
    return name.empty() ? "car" : name;
}

std::string TimestampToken() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm);
    return buf;
}

}  // namespace

void ExportVinyl() {
    auto* rt = rex::Runtime::instance();
    if (!rt) return;
    auto* mem = rt->memory();
    if (!mem) return;

    // Prefer the paint cached by the regen hook; fall back to the current
    // mcCarModel global (dword_8288DCF8, set every frame while the car renders;
    // carModel+20 = paint) so export works even if the hook hasn't fired.
    uint32_t paint = g_vinyl_paint.load(std::memory_order_relaxed);
    const char* src = "hook";
    if (!IsGuestPtr(paint)) {
        uint32_t car_model = GuestRead32(mem, 0x8288DCF8);
        if (IsGuestPtr(car_model)) {
            paint = GuestRead32(mem, car_model + 20);
            src = "carmodel";
        }
    }
    if (!IsGuestPtr(paint)) {
        LARECOMP_APP_ERROR("[Vinyl] Export: no car found (hook cache & mcCarModel both empty) "
                           "— be in the garage with the car visible.");
        return;
    }
    LARECOMP_APP_INFO("[Vinyl] Export: paint=0x{:08X} (via {})", paint, src);
    uint32_t block = GuestRead32(mem, paint + 132);
    if (!IsGuestPtr(block)) {
        LARECOMP_APP_ERROR("[Vinyl] Export: invalid vinyl block (0x{:08X}).", block);
        return;
    }
    uint32_t array = block + 64 + 2244;

    // Serialize: magic, version, carname, then per surface {cap, usedCount,
    // [localIdx u32 + 20 raw layer bytes]...}. Layer bytes are copied verbatim
    // (already big-endian in guest memory) for lossless round-trip.
    std::vector<uint8_t> out;
    auto put32 = [&](uint32_t v) {
        out.push_back(v & 0xFF); out.push_back((v >> 8) & 0xFF);
        out.push_back((v >> 16) & 0xFF); out.push_back((v >> 24) & 0xFF);
    };

    std::string car = ReadCarName(mem, paint);
    put32(kVgpMagic);
    put32(kVgpVersion);
    put32(static_cast<uint32_t>(car.size()));
    out.insert(out.end(), car.begin(), car.end());
    put32(kVinylSurfaces);

    uint32_t total_layers = 0;
    for (uint32_t s = 0; s < kVinylSurfaces; ++s) {
        uint32_t cap = GuestRead32(mem, kCapsTable + s * 4);
        uint32_t off = GuestRead32(mem, kOffsetsTable + s * 4);
        if (cap > 4096) cap = 0;  // sanity guard

        // Collect used slots first (shape-id != 0xFFFF).
        std::vector<uint32_t> used;
        for (uint32_t k = 0; k < cap; ++k) {
            uint32_t slot = array + kLayerStride * (off + k);
            if (GuestRead16(mem, slot + 16) != 0xFFFF) used.push_back(k);
        }

        put32(cap);
        put32(static_cast<uint32_t>(used.size()));
        for (uint32_t k : used) {
            uint32_t slot = array + kLayerStride * (off + k);
            const auto* p = mem->TranslateVirtual<const uint8_t*>(slot);
            put32(k);
            out.insert(out.end(), p, p + kLayerStride);
            ++total_layers;
        }
    }

    std::error_code ec;
    std::filesystem::path dir = std::filesystem::current_path(ec) / "vinyls";
    std::filesystem::create_directories(dir, ec);
    std::filesystem::path file = dir / ("vinyl_" + car + "_" + TimestampToken() + ".vgp");

    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    if (!f) {
        LARECOMP_APP_ERROR("[Vinyl] Export: cannot open {} for writing.", file.string());
        return;
    }
    f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    LARECOMP_APP_INFO("[Vinyl] Exported {} layers to {}", total_layers, file.string());
}

// ===========================================================================
// Vinyl shape catalog dump (ShapeIdx -> source name + content hash of the GPU
// texture, matching the SDK texture dumper so the DDS files can be joined).
// Verified in IDA (default.xex, sub_8236E400 / sub_82371368):
//   lib = mcCarVinylShapeLibrary* @ kShapeLib; db = shape DB @ kShapeDb.
//   count[cat]     = lib[3+cat]   (dword @ lib + 12 + 4*cat)
//   descTable[cat] = lib[26+cat]  (16 bytes/entry: +0 shapeObj, +8 flags)
//   refcount[cat]  = *(lib[49+cat]); request-load count per shape at +4*local
//   names[cat]     = *(db + 96 + 8*cat); filename = names[local] (char*)
//   shapeObj+8     = engine tex wrapper; wrapper+0x1C -> D3DTexture; fetch @ +0x1C
// A descriptor is "loaded" when (flags & 0x30000000) == 0x30000000.
// ===========================================================================

struct ShapeRec {
    uint64_t hash = 0;
    uint32_t w = 0, h = 0;
    std::string fmt;
};

// Resident-shape probe: parse the shape's texture and content-hash its guest
// texels exactly like the SDK dumper. hash == 0 if not loaded / not decodable.
struct ShapeProbe {
    bool loaded = false;
    uint32_t w = 0, h = 0, base = 0, size = 0, pitch = 0;
    bool tiled = false;
    std::string fmt;
    rex::graphics::xenos::TextureFormat format{};
    rex::graphics::xenos::Endian endian{};
    uint64_t hash = 0;
};

static ShapeProbe ProbeShape(rex::memory::Memory* mem, uint32_t lib, uint32_t cat, uint32_t local) {
    ShapeProbe p;
    uint32_t desc_table = GuestRead32(mem, lib + 4 * (26 + cat));
    if (!IsGuestPtr(desc_table)) return p;
    uint32_t desc = desc_table + 16 * local;
    uint32_t shape_obj = GuestRead32(mem, desc + 0);
    uint32_t flags = GuestRead32(mem, desc + 8);
    p.loaded = (flags & 0x30000000u) == 0x30000000u;
    if (!p.loaded || !IsGuestPtr(shape_obj)) return p;
    uint32_t tex = GuestRead32(mem, shape_obj + 8);
    if (!IsGuestPtr(tex)) return p;
    // wrapper+0x20 = real dims (hi16 = width, lo16 = height); the D3DTexture fetch
    // reports width/height minus 1, so use the wrapper's for the DDS name/decode
    // (matches what the SDK dumper wrote for the already-browsed shapes).
    uint32_t dims = GuestRead32(mem, tex + 0x20);
    uint32_t d3dtex = GuestRead32(mem, tex + 0x1C);
    if (!IsGuestPtr(d3dtex)) return p;
    rex::graphics::xenos::xe_gpu_texture_fetch_t fetch{};
    fetch.dword_0 = GuestRead32(mem, d3dtex + 0x1C);
    fetch.dword_1 = GuestRead32(mem, d3dtex + 0x20);
    fetch.dword_2 = GuestRead32(mem, d3dtex + 0x24);
    fetch.dword_3 = GuestRead32(mem, d3dtex + 0x28);
    fetch.dword_4 = GuestRead32(mem, d3dtex + 0x2C);
    fetch.dword_5 = GuestRead32(mem, d3dtex + 0x30);
    rex::graphics::TextureInfo ti{};
    if (rex::graphics::TextureInfo::Prepare(fetch, &ti) && ti.width >= 1 && ti.width <= 4096 &&
        ti.height >= 1 && ti.height <= 4096) {
        p.w = dims >> 16;
        p.h = dims & 0xFFFF;
        if (p.w < 1 || p.w > 4096 || p.h < 1 || p.h > 4096) {
            p.w = ti.width;
            p.h = ti.height;
        }
        p.base = ti.memory.base_address;
        p.size = ti.memory.base_size;
        p.pitch = fetch.pitch;
        p.tiled = ti.is_tiled;
        p.format = ti.format;
        p.endian = ti.endianness;
        const auto* finfo = ti.format_info();
        if (finfo && finfo->name) p.fmt = finfo->name;
        const uint8_t* bytes = mem->TranslatePhysical<const uint8_t*>(p.base);
        if (bytes && p.size)
            p.hash = mcla::HashGuestTexture(bytes, p.size);
    }
    return p;
}

static std::string ShapeName(rex::memory::Memory* mem, uint32_t db, uint32_t cat, uint32_t local) {
    uint32_t names = GuestRead32(mem, db + 96 + 8 * cat);
    if (!IsGuestPtr(names)) return {};
    return GuestCStr(mem, GuestRead32(mem, names + 4 * local), 128);
}

static std::filesystem::path VinylDir() {
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::current_path(ec) / "vinyls";
    std::filesystem::create_directories(dir, ec);
    return dir;
}

static std::map<uint32_t, ShapeRec> LoadShapeRecs(const std::filesystem::path& dir) {
    std::map<uint32_t, ShapeRec> recs;
    std::ifstream mf(dir / "vinyl_shape_hashes.txt");
    uint32_t midx = 0, mw = 0, mh = 0;
    uint64_t mhash = 0;
    std::string mfmt;
    while (mf >> midx >> std::hex >> mhash >> std::dec >> mw >> mh >> mfmt) {
        if (mhash) recs[midx] = ShapeRec{mhash, mw, mh, mfmt};
    }
    return recs;
}

// Writes vinyl_shape_hashes.txt (accumulated ShapeIdx->hash map) and
// vinyl_shapes.json (the full catalog joined to whatever hashes exist so far).
static void SaveShapeManifest(rex::memory::Memory* mem, uint32_t lib, uint32_t db,
                              const std::map<uint32_t, ShapeRec>& recs,
                              const std::filesystem::path& dir) {
    auto esc = [](const std::string& s) {
        std::string o;
        for (char c : s) {
            if (c == '"' || c == '\\') o.push_back('\\');
            o.push_back(c);
        }
        return o;
    };
    std::string json = "{\n  \"shapes\": [\n";
    uint32_t total = 0;
    bool first = true;
    for (uint32_t cat = 0; cat < kShapeCats; ++cat) {
        uint32_t count = GuestRead32(mem, lib + 12 + 4 * cat);
        if (count > 100000) continue;
        for (uint32_t local = 0; local < count; ++local) {
            uint32_t idx = cat * 1000 + local;
            std::string name = ShapeName(mem, db, cat, local);
            auto it = recs.find(idx);
            uint64_t hash = it != recs.end() ? it->second.hash : 0;
            uint32_t w = it != recs.end() ? it->second.w : 0;
            uint32_t h = it != recs.end() ? it->second.h : 0;
            std::string fmt = it != recs.end() ? it->second.fmt : std::string();
            char hbuf[19];
            std::snprintf(hbuf, sizeof(hbuf), "0x%016llX", static_cast<unsigned long long>(hash));
            if (!first) json += ",\n";
            first = false;
            json += "    {\"idx\": " + std::to_string(idx) + ", \"cat\": " + std::to_string(cat) +
                    ", \"local\": " + std::to_string(local) + ", \"name\": \"" + esc(name) +
                    "\", \"w\": " + std::to_string(w) + ", \"h\": " + std::to_string(h) +
                    ", \"fmt\": \"" + fmt + "\", \"hash\": \"" + std::string(hbuf) + "\"}";
            ++total;
        }
    }
    json += "\n  ],\n  \"total\": " + std::to_string(total) +
            ",\n  \"mapped\": " + std::to_string(static_cast<uint32_t>(recs.size())) + "\n}\n";

    {
        std::ofstream mf(dir / "vinyl_shape_hashes.txt", std::ios::trunc);
        for (const auto& [rid, r] : recs) {
            char hb[17];
            std::snprintf(hb, sizeof(hb), "%016llx", static_cast<unsigned long long>(r.hash));
            mf << rid << ' ' << hb << ' ' << r.w << ' ' << r.h << ' ' << r.fmt << '\n';
        }
    }
    std::ofstream f(dir / "vinyl_shapes.json", std::ios::binary | std::ios::trunc);
    if (f) f.write(json.data(), static_cast<std::streamsize>(json.size()));
}

// Single-pass dump: hash every currently-resident shape, merge into the
// persistent map, rewrite the manifest. Use after browsing categories.
void DumpVinylShapes() {
    auto* rt = rex::Runtime::instance();
    if (!rt) return;
    auto* mem = rt->memory();
    if (!mem) return;
    uint32_t lib = GuestRead32(mem, kShapeLib);
    uint32_t db = GuestRead32(mem, kShapeDb);
    if (!IsGuestPtr(lib) || !IsGuestPtr(db)) {
        LARECOMP_APP_ERROR("[Vinyl] Shape dump: catalog not ready (lib=0x{:08X} db=0x{:08X}) "
                           "— enter the Vinyl editor once, then retry.", lib, db);
        return;
    }
    std::filesystem::path dir = VinylDir();
    auto recs = LoadShapeRecs(dir);
    uint32_t resident = 0;
    for (uint32_t cat = 0; cat < kShapeCats; ++cat) {
        uint32_t count = GuestRead32(mem, lib + 12 + 4 * cat);
        if (count > 100000) continue;
        for (uint32_t local = 0; local < count; ++local) {
            ShapeProbe p = ProbeShape(mem, lib, cat, local);
            if (p.loaded && p.hash) {
                recs[cat * 1000 + local] = ShapeRec{p.hash, p.w, p.h, p.fmt};
                ++resident;
            }
        }
    }
    SaveShapeManifest(mem, lib, db, recs, dir);
    LARECOMP_APP_INFO("[Vinyl] Shapes: {} mapped total ({} resident this pass).", recs.size(),
                      resident);
}

// ---------------------------------------------------------------------------
// Hands-free capture: force-load every shape a few at a time via the game's own
// request-refcount (lib[49+cat][local]), hash it while resident, release it,
// advance. Runs off Patch_DeltaTimePre so async loads have frames to complete.
// Bounds memory to a small sliding window; the hash matches the DDS the SDK
// dumper already wrote, so no re-browsing / re-dumping is needed.
// ---------------------------------------------------------------------------
static std::atomic_bool g_scap_request{false};  // set by cvar callback (any thread)
static bool g_scap_active = false;               // tick-thread only below
static std::vector<std::pair<uint32_t, uint32_t>> g_scap_list;
static std::map<uint32_t, ShapeRec> g_scap_recs;
static size_t g_scap_cursor = 0;
static size_t g_scap_requested = 0;
static int g_scap_wait = 0;
static uint32_t g_scap_captured = 0;
static constexpr size_t kScapWindow = 6;   // max shapes we hold resident at once
static constexpr int kScapMaxWait = 120;   // frames to await one load before skip

void RequestVinylShapeCapture() { g_scap_request.store(true, std::memory_order_relaxed); }

// Adjust a shape's request-refcount by delta (big-endian RMW). +1 asks the
// per-frame loader to load it; -1 lets it be released. Increment (not set) so we
// never clobber the game's own reference for shapes it currently wants.
static void ShapeRefAdjust(rex::memory::Memory* mem, uint32_t lib, uint32_t cat, uint32_t local,
                           int delta) {
    uint32_t arr = GuestRead32(mem, lib + 4 * (49 + cat));
    if (!IsGuestPtr(arr)) return;
    uint32_t a = arr + 4 * local;
    int64_t v = static_cast<int32_t>(GuestRead32(mem, a));
    v += delta;
    if (v < 0) v = 0;
    GuestWrite32(mem, a, static_cast<uint32_t>(v));
}

void TickVinylShapeCapture() {
    auto* rt = rex::Runtime::instance();
    if (!rt) return;
    auto* mem = rt->memory();
    if (!mem) return;
    uint32_t lib = GuestRead32(mem, kShapeLib);
    uint32_t db = GuestRead32(mem, kShapeDb);

    if (g_scap_request.exchange(false, std::memory_order_relaxed)) {
        if (!IsGuestPtr(lib) || !IsGuestPtr(db)) {
            LARECOMP_APP_ERROR("[Vinyl] Capture: catalog not ready — enter the Vinyl editor first.");
        } else if (!g_scap_active) {
            g_scap_list.clear();
            for (uint32_t cat = 0; cat < kShapeCats; ++cat) {
                uint32_t count = GuestRead32(mem, lib + 12 + 4 * cat);
                if (count > 100000) continue;
                for (uint32_t local = 0; local < count; ++local)
                    g_scap_list.emplace_back(cat, local);
            }
            g_scap_recs = LoadShapeRecs(VinylDir());
            g_scap_cursor = 0;
            g_scap_requested = 0;
            g_scap_wait = 0;
            g_scap_captured = 0;
            g_scap_active = true;
            LARECOMP_APP_INFO("[Vinyl] Capture: sweeping {} shapes...", g_scap_list.size());
        }
    }

    if (!g_scap_active) return;
    if (!IsGuestPtr(lib) || !IsGuestPtr(db)) return;  // catalog gone; pause this frame

    // Keep a sliding window of load requests ahead of the cursor.
    size_t want = std::min(g_scap_cursor + kScapWindow, g_scap_list.size());
    while (g_scap_requested < want) {
        auto [c, l] = g_scap_list[g_scap_requested];
        ShapeRefAdjust(mem, lib, c, l, +1);
        ++g_scap_requested;
    }

    if (g_scap_cursor < g_scap_list.size()) {
        auto [c, l] = g_scap_list[g_scap_cursor];
        ShapeProbe p = ProbeShape(mem, lib, c, l);
        if (p.loaded && p.hash) {
            g_scap_recs[c * 1000 + l] = ShapeRec{p.hash, p.w, p.h, p.fmt};
            // Force-loaded shapes are never drawn, so the SDK dump-on-sample path
            // misses them. Write the DDS ourselves — same
            // dump/<hash>_<w>x<h>_<fmt>.dds naming, and it skips any file that
            // already exists (already dumped while browsing).
            static const std::filesystem::path s_dump_dir = mcla::TextureDumpDir();
            const uint8_t* bytes = mem->TranslatePhysical<const uint8_t*>(p.base);
            if (bytes && p.size)
                mcla::DumpGuestTexture(s_dump_dir, p.hash, p.w, p.h, p.pitch, p.tiled, p.format,
                                       p.endian, bytes, p.size);
            ++g_scap_captured;
            ShapeRefAdjust(mem, lib, c, l, -1);
            ++g_scap_cursor;
            g_scap_wait = 0;
        } else if (++g_scap_wait > kScapMaxWait) {
            ShapeRefAdjust(mem, lib, c, l, -1);  // give up on this one
            ++g_scap_cursor;
            g_scap_wait = 0;
        }
        return;
    }

    SaveShapeManifest(mem, lib, db, g_scap_recs, VinylDir());
    LARECOMP_APP_INFO("[Vinyl] Capture done: {} shapes mapped ({} newly captured this run).",
                      g_scap_recs.size(), g_scap_captured);
    g_scap_active = false;
    g_scap_list.clear();
    g_scap_recs.clear();
}

// Imports a .vgp package (from <exe dir>/vinyls/) onto the current garage car,
// mirroring the game's own package loader (sub_826A2520): for each surface,
// begin-edit (write container+2072=surf, +8004=1), empty every cap slot, drop in
// the imported layers verbatim, end-edit (container+2072=-1); then trigger one
// full regen by replicating sub_8236D850's work-area writes. All pure guest-
// memory writes (the block is a writable heap object) — no guest calls, so this
// is safe to run from the F4/cvar-callback thread.
void ImportVinyl(const std::string& name_in) {
    auto* rt = rex::Runtime::instance();
    if (!rt) return;
    auto* mem = rt->memory();
    if (!mem) return;

    // Trim surrounding whitespace/quotes from the typed name.
    std::string name = name_in;
    auto trim = [](std::string& s) {
        auto notspace = [](unsigned char c) { return !std::isspace(c) && c != '"'; };
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), notspace));
        s.erase(std::find_if(s.rbegin(), s.rend(), notspace).base(), s.end());
    };
    trim(name);
    if (name.empty()) return;

    std::error_code ec;
    std::filesystem::path dir = std::filesystem::current_path(ec) / "vinyls";
    std::filesystem::path file = dir / name;
    if (!std::filesystem::exists(file, ec))
        if (std::filesystem::exists(dir / (name + ".vgp"), ec)) file = dir / (name + ".vgp");
    if (!std::filesystem::exists(file, ec)) {
        LARECOMP_APP_ERROR("[Vinyl] Import: '{}' not found in {}.", name, dir.string());
        for (auto it = std::filesystem::directory_iterator(dir, ec);
             !ec && it != std::filesystem::directory_iterator(); ++it)
            if (it->path().extension() == ".vgp")
                LARECOMP_APP_INFO("[Vinyl]   available: {}", it->path().filename().string());
        return;
    }

    std::ifstream in(file, std::ios::binary);
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    size_t o = 0;
    auto rd32 = [&](uint32_t& v) -> bool {
        if (o + 4 > buf.size()) return false;
        v = uint32_t(buf[o]) | (uint32_t(buf[o + 1]) << 8) | (uint32_t(buf[o + 2]) << 16) |
            (uint32_t(buf[o + 3]) << 24);
        o += 4;
        return true;
    };

    uint32_t magic = 0, ver = 0, nlen = 0, nsurf = 0;
    if (!rd32(magic) || magic != kVgpMagic) {
        LARECOMP_APP_ERROR("[Vinyl] Import: {} is not a RexGlue .vgp (bad magic).",
                           file.filename().string());
        return;
    }
    rd32(ver);
    if (ver != kVgpVersion)
        LARECOMP_APP_INFO("[Vinyl] Import: file format v{} (importer is v{}); reading anyway.",
                          ver, kVgpVersion);
    if (!rd32(nlen) || o + nlen > buf.size()) {
        LARECOMP_APP_ERROR("[Vinyl] Import: {} is truncated.", file.filename().string());
        return;
    }
    std::string src_car(reinterpret_cast<const char*>(buf.data() + o), nlen);
    o += nlen;
    if (!rd32(nsurf) || nsurf != kVinylSurfaces) {
        LARECOMP_APP_ERROR("[Vinyl] Import: unexpected surface count ({}).", nsurf);
        return;
    }

    // Parse per-surface used layers: {cap, used, [localIdx u32 + 20 raw bytes]...}.
    struct Layer { uint32_t idx; uint8_t bytes[kLayerStride]; };
    std::vector<std::vector<Layer>> surf_layers(kVinylSurfaces);
    for (uint32_t s = 0; s < nsurf; ++s) {
        uint32_t fcap = 0, used = 0;
        if (!rd32(fcap) || !rd32(used)) {
            LARECOMP_APP_ERROR("[Vinyl] Import: truncated surface {}.", s);
            return;
        }
        (void)fcap;  // layers are placed by live caps, not the file's
        for (uint32_t i = 0; i < used; ++i) {
            uint32_t idx = 0;
            if (!rd32(idx) || o + kLayerStride > buf.size()) {
                LARECOMP_APP_ERROR("[Vinyl] Import: truncated layer data (surface {}).", s);
                return;
            }
            Layer L;
            L.idx = idx;
            std::memcpy(L.bytes, buf.data() + o, kLayerStride);
            o += kLayerStride;
            surf_layers[s].push_back(L);
        }
    }

    // Resolve the current car and its writable vinyl block.
    const char* psrc = "?";
    uint32_t paint = ResolveCurrentPaint(mem, &psrc);
    if (!paint) {
        LARECOMP_APP_ERROR("[Vinyl] Import: no car loaded — be in the garage with the car visible.");
        return;
    }
    uint32_t block = GuestRead32(mem, paint + 132);
    if (!IsGuestPtr(block)) {
        LARECOMP_APP_ERROR("[Vinyl] Import: invalid vinyl block (0x{:08X}).", block);
        return;
    }
    uint32_t container = block + 64;
    uint32_t array = container + 2244;

    uint32_t written = 0, dropped = 0;
    for (uint32_t s = 0; s < kVinylSurfaces; ++s) {
        uint32_t cap = GuestRead32(mem, kCapsTable + s * 4);
        uint32_t off = GuestRead32(mem, kOffsetsTable + s * 4);
        if (cap > 4096) cap = 0;  // sanity guard

        GuestWrite32(mem, container + 2072, s);  // sub_82392538: begin surface edit
        GuestWrite8(mem, container + 8004, 1);

        for (uint32_t k = 0; k < cap; ++k)  // empty every slot first
            GuestWrite16(mem, array + kLayerStride * (off + k) + 16, 0xFFFF);

        for (const auto& L : surf_layers[s]) {
            if (L.idx >= cap) { ++dropped; continue; }  // past the active cap
            auto* p = mem->TranslateVirtual<uint8_t*>(array + kLayerStride * (off + L.idx));
            std::memcpy(p, L.bytes, kLayerStride);
            ++written;
        }

        GuestWrite32(mem, container + 2072, 0xFFFFFFFFu);  // sub_82392548: end surface edit
    }

    // Trigger a full vinyl regen by replicating sub_8236D850(wa, paint, tex,
    // player) with plain memory writes. Work-area comes from the global
    // dword_8288DFC0 (fall back to the hook-cached r3); tex/player are the values
    // the hook captured at the last regen for this car.
    uint32_t wa = GuestRead32(mem, 0x8288DFC0);
    if (!IsGuestPtr(wa)) wa = g_vinyl_wa.load(std::memory_order_relaxed);
    uint32_t tex = g_vinyl_tex.load(std::memory_order_relaxed);
    uint32_t player = g_vinyl_player.load(std::memory_order_relaxed);
    if (IsGuestPtr(wa)) {
        GuestWrite32(mem, wa + 256, tex);     // a3: composite target texture
        GuestWrite8(mem, wa + 248, 1);
        GuestWrite32(mem, wa + 244, paint);   // a2: paint object
        GuestWrite32(mem, wa + 220, 0);
        GuestWrite32(mem, wa + 224, 0);
        GuestWrite8(mem, wa + 20, 1);
        GuestWrite8(mem, wa + 21, 1);
        GuestWrite8(mem, wa + 251, 1);
        GuestWrite32(mem, wa + 16, player);   // a4: player index
    } else {
        LARECOMP_APP_ERROR("[Vinyl] Import: work-area not ready — layers written but not "
                           "re-composited. Nudge a vinyl edit or re-enter the garage.");
    }

    LARECOMP_APP_INFO("[Vinyl] Imported {} layers ({} over-cap dropped) from '{}' (made for '{}') "
                      "onto car paint=0x{:08X} via {}.",
                      written, dropped, file.filename().string(), src_car, paint, psrc);
}

void InitHooks() {
    // Builds xarchive_mods.rpf from models/*.obj. Must run before guest code
    // reaches sub_822C4630 and mounts the archives.
    mc::modloader::Init();

    // Scans <exe>/music and the User Music folder. The tracks are handed to
    // mcMusicManager later, from the ctor hook MCLA_CustomMusic_Install.
    InitCustomMusic();

    ApplyAspectRatioPatch(REXCVAR_GET(aspect_ratio));

    rex::cvar::RegisterChangeCallback("aspect_ratio",
        [](std::string_view name, std::string_view new_value) {
            ApplyAspectRatioPatch(new_value);
        }
    );

    ApplyVinylLayerCaps();

    // export_vinyl acts as a button: toggling it ON runs the export, then it
    // flips back OFF so it can be triggered again.
    rex::cvar::RegisterChangeCallback("export_vinyl",
        [](std::string_view name, std::string_view new_value) {
            if (new_value == "true" || new_value == "1") {
                ExportVinyl();
                rex::cvar::SetFlagByName("export_vinyl", "false");
            }
        }
    );

    // cam_probe_mark / cam_probe_dump: buttons — snapshot the gameplay camera
    // and diff it, to find what the look-around actually moves. Same
    // flip-back-off shape as the dumps below.
    rex::cvar::RegisterChangeCallback("cam_probe_mark",
        [](std::string_view name, std::string_view new_value) {
            if (new_value == "true" || new_value == "1") {
                CameraProbeMark();
                rex::cvar::SetFlagByName("cam_probe_mark", "false");
            }
        }
    );

    rex::cvar::RegisterChangeCallback("cam_probe_dump",
        [](std::string_view name, std::string_view new_value) {
            if (new_value == "true" || new_value == "1") {
                CameraProbeDump();
                rex::cvar::SetFlagByName("cam_probe_dump", "false");
            }
        }
    );

    // The one that is actually usable: arms both samples on a timer, because
    // opening this overlay to press a button is itself enough to take the game
    // out of the camera state being measured.
    rex::cvar::RegisterChangeCallback("cam_probe_run",
        [](std::string_view name, std::string_view new_value) {
            if (new_value == "true" || new_value == "1") {
                CameraProbeRun();
                rex::cvar::SetFlagByName("cam_probe_run", "false");
            }
        }
    );

    // rubberband_dump: button — writes the 11 parsed tune entries out and flips
    // itself back off so it can be triggered again.
    rex::cvar::RegisterChangeCallback("rubberband_dump",
        [](std::string_view name, std::string_view new_value) {
            if (new_value == "true" || new_value == "1") {
                DumpRubberBandTuning();
                rex::cvar::SetFlagByName("rubberband_dump", "false");
            }
        }
    );

    // import_vinyl: type a .vgp file name + Enter to apply it, then the field
    // clears itself. The empty write re-fires this callback, hence the guard.
    rex::cvar::RegisterChangeCallback("import_vinyl",
        [](std::string_view name, std::string_view new_value) {
            if (new_value.empty()) return;
            ImportVinyl(std::string(new_value));
            rex::cvar::SetFlagByName("import_vinyl", "");
        }
    );

    // dump_vinyl_shapes: button — toggling ON writes the shape catalog manifest,
    // then flips back OFF so it can be triggered again.
    rex::cvar::RegisterChangeCallback("dump_vinyl_shapes",
        [](std::string_view name, std::string_view new_value) {
            if (new_value == "true" || new_value == "1") {
                DumpVinylShapes();
                rex::cvar::SetFlagByName("dump_vinyl_shapes", "false");
            }
        }
    );

    // capture_vinyl_shapes: button — starts the hands-free sweep, which runs on
    // the per-frame tick (Patch_DeltaTimePre) and finalizes itself.
    rex::cvar::RegisterChangeCallback("capture_vinyl_shapes",
        [](std::string_view name, std::string_view new_value) {
            if (new_value == "true" || new_value == "1") {
                RequestVinylShapeCapture();
                rex::cvar::SetFlagByName("capture_vinyl_shapes", "false");
            }
        }
    );

    // Reactivate the game's dev command-line options from <exe>/debug_options.txt.
    // Must run before the game's option consumers (all init/level-load reads).
    ApplyDebugOptions();

    // The MCLA/Performance cvars that map onto those same dev switches. Runs
    // after the file, so a cvar that is on overrides the same name coming from
    // debug_options.txt; a cvar that is off leaves the file's value alone.
    ApplyPerfDebugOptions();

    StartFreezeWatchdog();
}

// HOOK FUNCTIONS (Called in the middle of translated Assembly execution)

REXCVAR_DEFINE_STRING(stream_trace, "", "MCLA/Diag",
    "Log every resource the streamer is asked for whose path contains this "
    "text, and whether the file was found.\n"
    "\n"
    "The hook sits one instruction after pgStreamer::Open returns inside "
    "sub_821E2940, where the resolved path is still on the stack at r1+0x60 and "
    "r3 holds the handle -- minus one when the file was not found. That pair is "
    "the only thing that separates \"the game never asked for it\" from \"the "
    "game asked and the archive did not have it\", which is the question a car "
    "that loads forever comes down to.\n"
    "\n"
    "Empty turns it off. `resources/vehicle` is the useful setting for a car; "
    "`/` logs every file the game opens, which is thousands of lines.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Shared by the two open probes: read the path the call was handed off the
// guest stack, hold it against the filter, and say whether it was found.
static void TraceOpen(const char* kind, uint32_t stack, uint32_t offset, bool found) {
    const std::string filter = REXCVAR_GET(stream_trace);
    if (filter.empty()) return;

    const auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;

    char path[192];
    size_t n = 0;
    while (n < sizeof(path) - 1) {
        const char c = static_cast<char>(base[stack + offset + n]);
        if (!c) break;
        path[n++] = c;
    }
    path[n] = '\0';
    if (n == 0) return;
    if (filter != "/" && std::string_view(path).find(filter) == std::string_view::npos) return;

    LARECOMP_APP_INFO("[stream] {} {} {}", kind, found ? "opened   " : "NOT FOUND", path);
}

// Entry of sub_821BD618, RAGE's fatal error.
//
// It is `__noreturn { if (handler) handler(msg); while (1) ; }` -- and the
// handler pointer is null in the shipped build, so every fatal in this game is
// a silent infinite spin on whatever thread hit it. Nothing is printed, no
// watchdog fires, and if the thread was the game's own the frame simply stops
// while the render thread keeps drawing the last one. That is what a MCLA
// "infinite loading" actually looks like, and 223 call sites can produce it.
//
// This makes it speak. r3 is the printf format, r4 the first argument, which
// between them name the failure ("Unknown manufacturer %s in vehicle list
// file", "global array [%s] does not exist", and so on). It does not stop the
// spin -- the callers are written assuming this never returns -- it only says
// what happened before the game stops.
void MCLA_RageFatal(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6) {
    const auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;

    // Most of these messages take a plain integer -- "zlibInflater::InflateBegin
    // - error %08x" is the one that brought the game down here -- and an integer
    // that happens to look like an address is not one. So every byte is read
    // only after the host page behind it is known to be committed and readable:
    // walking off the end of a mapped region is what turned the one assert that
    // finally fired into an access violation inside this hook.
    auto readable = [&](uint32_t ea) {
#if defined(_WIN32)
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(base + ea, &mbi, sizeof(mbi))) return false;
        constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                    PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                    PAGE_EXECUTE_WRITECOPY;
        return mbi.State == MEM_COMMIT && (mbi.Protect & kReadable) != 0 &&
               (mbi.Protect & PAGE_GUARD) == 0;
#else
        (void)ea;
        return false;
#endif
    };

    auto read = [&](uint32_t ea) {
        std::string out;
        if (ea < 0x1000 || ea >= 0xC0000000) return out;
        for (size_t i = 0; i < 256; ++i) {
            const uint32_t at = ea + i;
            // Probe once per page rather than once per byte.
            if (i == 0 || (at & 0xFFFu) == 0) {
                if (!readable(at)) break;
            }
            const char c = static_cast<char>(base[at]);
            if (!c) break;
            // A string argument is text. Anything else is an integer that
            // happened to point at mapped memory, and printing it is noise.
            if (static_cast<unsigned char>(c) < 0x20 ||
                static_cast<unsigned char>(c) > 0x7E) {
                return std::string();
            }
            out.push_back(c);
        }
        return out;
    };

    const std::string format = read(static_cast<uint32_t>(r3.u64));
    // Several of these messages carry two names -- "invalid group (%s) on car
    // %s" -- and which car it was is the whole answer, so the next three
    // argument registers are read as strings too and the ones that are not
    // simply come back empty.
    const std::string a1 = read(static_cast<uint32_t>(r4.u64));
    const std::string a2 = read(static_cast<uint32_t>(r5.u64));
    const std::string a3 = read(static_cast<uint32_t>(r6.u64));
    LARECOMP_APP_ERROR("[rage-fatal] {} | args: '{}' '{}' '{}' (r4={:#x} r5={:#x} r6={:#x})",
                       format.empty() ? "<unreadable>" : format, a1, a2, a3,
                       static_cast<uint32_t>(r4.u64), static_cast<uint32_t>(r5.u64),
                       static_cast<uint32_t>(r6.u64));

    // "zlibInflater::InflateBegin - not in XCompress format" means the four
    // bytes at the inflater's input pointer are not 0x0FF512EF. Which four they
    // are says where the stream really is: eight bytes on from the magic means
    // the header check ran a second time on a stream that had already been
    // opened, which happens when the first decode step consumed nothing.
    // r4 is the pointer the comparison was made through.
    if (format.rfind("zlibInflater", 0) == 0) {
        const uint32_t ctx = static_cast<uint32_t>(r4.u64);
        auto hex = [&](uint32_t from, uint32_t count, uint32_t mark) {
            std::string out;
            for (uint32_t i = 0; i < count; ++i) {
                const uint32_t ea = from + i;
                if (i == 0 || (ea & 0xFFFu) == 0) {
                    if (!readable(ea)) break;
                }
                char byte[8] = {};
                std::snprintf(byte, sizeof byte, "%s%02X", ea == mark ? "|" : " ", base[ea]);
                out += byte;
            }
            return out;
        };

        if (ctx >= 0x1000 && ctx < 0xC0000000 - 0x40) {
            // r4 is the inflater's context: +0 input left, +4 input pointer,
            // +8 consumed, +12 stream length, +16 output space.
            auto word = [&](uint32_t offset) {
                const uint8_t* p = base + ctx + offset;
                return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
                       (uint32_t(p[2]) << 8) | uint32_t(p[3]);
            };
            LARECOMP_APP_ERROR("[rage-fatal] inflater ctx={:#010x} in_left={} in_ptr={:#010x} "
                               "consumed={} len={} out_left={}",
                               ctx, word(0), word(4), word(8), word(12), word(16));

            // The comparison that failed dereferences the input pointer, so the
            // bytes it read are the answer: the read window starts twelve bytes
            // before it, at the file's own first byte, and a resource begins
            // 05 43 53 52 -- "RSC" with its version. If those four are there and
            // 0F F5 12 EF follows at twelve, the file arrived intact and the
            // fault is the frame, not the bytes.
            const uint32_t in_ptr = word(4);
            if (in_ptr >= 0x1000 + 12 && in_ptr < 0xC0000000 - 0x40) {
                LARECOMP_APP_ERROR("[rage-fatal] file as read, offset 0 (| is the pointer, "
                                   "at offset 12):{}",
                                   hex(in_ptr - 12, 40, in_ptr));
            }
        }
    }
}

// One instruction after `bl sub_821BCE68` in sub_821E2940 (0x821E29C4). The
// resolved path is the stack buffer at r1+0x60 that the call was handed, and r3
// is the handle it came back with: -1 means pgStreamer::Open could not find the
// file. Pure observer.
void MCLA_StreamOpenResult(PPCRegister& r1, PPCRegister& r3) {
    TraceOpen("rsc ", static_cast<uint32_t>(r1.u64), 0x60u,
              static_cast<int32_t>(static_cast<uint32_t>(r3.u64)) != -1);
}

// 0x821CA708, one instruction after `bl sub_821BDF20` inside sub_821CA6A8 --
// fiDevice's plain-file open, the one every tune, camera and garage file goes
// through. The resource streamer above never sees any of those, which is the
// half of the file system a car that loads forever could be stuck in. The path
// the call was handed is the stack buffer at r1+0x50 and r3 is the stream, zero
// when nothing opened. Pure observer.
void MCLA_FileOpenResult(PPCRegister& r1, PPCRegister& r3) {
    TraceOpen("file", static_cast<uint32_t>(r1.u64), 0x50u, r3.u32 != 0);
}

// datResource's fixup error, at 0x821D2378, one instruction of cost because it
// is only reached on the way to the fatal.
//
// r3 is the resource descriptor: +0 the segment table, +8 the name. r5 is the
// address that fit no segment. The table starts with two 16-bit counts -- how
// many virtual segments and how many physical -- followed by twelve-byte
// entries, and the two lookups that disagree here (sub_82187A38 and
// sub_8217D828) read it from different offsets, so the raw words go in the log
// and the layout can be settled by looking at them rather than by guessing.
void MCLA_ResourceFixupError(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5) {
    const auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;

    auto word = [&](uint32_t ea) {
        const uint8_t* p = base + ea;
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) |
               uint32_t(p[3]);
    };
    auto text = [&](uint32_t ea) {
        std::string out;
        if (ea < 0x1000 || ea >= 0xC0000000) return out;
        for (int i = 0; i < 96; ++i) {
            const char c = static_cast<char>(base[ea + i]);
            if (c < 0x20 || c > 0x7E) break;
            out.push_back(c);
        }
        return out;
    };

    const uint32_t desc = static_cast<uint32_t>(r3.u64);
    const uint32_t addr = static_cast<uint32_t>(r5.u64);
    if (desc < 0x1000 || desc >= 0xC0000000) return;

    LARECOMP_APP_ERROR("[fixup] {} | resource '{}' | address {:#010x}",
                       text(static_cast<uint32_t>(r4.u64)), text(word(desc + 8)), addr);

    // The descriptor first: whatever says how many of the table's entries
    // belong to this resource lives here, not in the table. The table's own
    // first word turned out to be the first entry's size, so reading it as a
    // pair of counts was wrong.
    std::string head;
    for (uint32_t i = 0; i < 12; ++i) {
        char part[16] = {};
        std::snprintf(part, sizeof part, " %08X", word(desc + i * 4));
        head += part;
    }
    LARECOMP_APP_ERROR("[fixup] descriptor {:#010x}:{}", desc, head);

    const uint32_t table = word(desc + 0);
    if (table < 0x1000 || table >= 0xC0000000) {
        LARECOMP_APP_ERROR("[fixup] segment table pointer {:#010x} is not readable", table);
        return;
    }

    // An entry is twelve bytes: size, the address the resource was built for,
    // and the address it actually landed at. sub_8217D890 takes its delta from
    // the second and third, which is what pins the order down.
    uint64_t virtual_total = 0, physical_total = 0;
    for (uint32_t i = 0; i < 16; ++i) {
        const uint32_t at = table + i * 12;
        const uint32_t size = word(at), declared = word(at + 4), real = word(at + 8);
        if (declared == 0 && size == 0) break;
        if ((declared & 0xF0000000u) == 0x50000000u) virtual_total += size;
        if ((declared & 0xF0000000u) == 0x60000000u) physical_total += size;
        LARECOMP_APP_ERROR("[fixup]   entry {:2}: size {:#010x} built for {:#010x} landed at "
                           "{:#010x} delta {:#010x}{}",
                           i, size, declared, real, real - declared,
                           (addr >= declared && addr < declared + size) ? "   <-- holds it" : "");
    }
    LARECOMP_APP_ERROR("[fixup] totals so far: virtual {} bytes, physical {} bytes",
                       virtual_total, physical_total);
}

REXCVAR_DEFINE_STRING(segment_trace, "", "MCLA/Diag",
    "Dump the segment table of every resource whose file name contains this "
    "text, as the streamer allocates it.\n"
    "\n"
    "A resource does not arrive in memory as one block. sub_821E58F0 walks a "
    "table of twelve-byte entries -- destination, size, and the address the "
    "resource was built for -- and either allocates them one by one (each landing "
    "wherever the heap had room) or sums them into a single allocation, with "
    "every size rounded up to 128 either way. Every pointer inside the resource "
    "is then resolved by finding which entry's range it falls in (sub_8217D828) "
    "and adding that entry's own delta.\n"
    "\n"
    "So a buffer that starts in one segment and runs past its end is read as if "
    "the next segment followed it, and from the boundary on the GPU fetches "
    "whatever the heap put there -- a mesh that is perfect on disk and blades on "
    "screen. This prints the boundaries so a rebuilt drawable's buffers can be "
    "held against them.\n"
    "\n"
    "Empty turns it off. `/` dumps every resource. `>N` dumps only the ones "
    "whose segments total N bytes or more, which is the setting that works: a "
    "resource read out of an archive has no name to match -- that is what the "
    "fixup error means by 'Unknown file in an archive' -- and a rebuilt car body "
    "is the only thing in the game past a megabyte. Anything else is matched "
    "against the name.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// 0x821E5908, just after sub_821E57B8 has filled the table. r31 is the table.
// Pure observer -- it runs before the allocator, so the destination column is
// still whatever was there last, and only the sizes and declared bases are real.
void MCLA_ResourceSegments(PPCRegister& r31) {
    const std::string filter = REXCVAR_GET(segment_trace);
    if (filter.empty()) return;

    const auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;

    const uint32_t table = static_cast<uint32_t>(r31.u64);
    if (table < 0x1000 || table >= 0xC0000000) return;

    auto word = [&](uint32_t ea) {
        const uint8_t* p = base + ea;
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) |
               uint32_t(p[3]);
    };
    auto half = [&](uint32_t ea) {
        return static_cast<uint32_t>((base[ea] << 8) | base[ea + 1]);
    };

    const uint32_t virtuals = half(table), physicals = half(table + 2);
    const uint32_t total = virtuals + physicals;
    // The table lives in a 1540-byte streamer slot starting at +44, so 124
    // entries is all the room there is. A count past that is its own answer, and
    // reading it as a real count would walk off the slot.
    if (total == 0 || total > 124) {
        if (total > 124) {
            LARECOMP_APP_ERROR("[segments] {} + {} blocks, past the 124 the streamer slot holds",
                               virtuals, physicals);
        }
        return;
    }

    // An entry is twelve bytes from +4: the address the resource was built for,
    // the address it will land at, and the size. sub_82187A38 reads the pair at
    // +4/+8 and sub_821E58F0 writes the destination at +4 and takes the size
    // from +8, which pins all three down.
    uint64_t virtual_bytes = 0, physical_bytes = 0;
    for (uint32_t i = 0; i < total; ++i) {
        const uint32_t size = word(table + 4 + i * 12 + 8);
        (i < virtuals ? virtual_bytes : physical_bytes) += size;
    }

    if (filter != "/" && filter[0] == '>') {
        const uint64_t least = std::strtoull(filter.c_str() + 1, nullptr, 10);
        if (virtual_bytes + physical_bytes < least) return;
    }

    // The byte the next instructions branch on. Non-zero means every block of
    // this resource comes out of ONE allocation and lands end to end, so a
    // buffer crossing a block boundary costs nothing. Zero means each block is
    // allocated on its own and a crossing buffer reads whatever followed the
    // block in the heap -- which is the whole question this probe exists for.
    const bool one_allocation = base[0x8286CE81] != 0;

    LARECOMP_APP_INFO("[segments] {} virtual block(s) totalling {}, {} physical totalling {}; "
                      "blocks are {}",
                      virtuals, virtual_bytes, physicals, physical_bytes,
                      one_allocation ? "ONE allocation, contiguous"
                                     : "allocated SEPARATELY, not contiguous");

    uint64_t running = 0;
    for (uint32_t i = 0; i < total; ++i) {
        const uint32_t at = table + 4 + i * 12;
        const uint32_t built_for = word(at), size = word(at + 8);
        if (i == virtuals) running = 0;
        LARECOMP_APP_INFO("[segments]   {:3} {:<8} {:#010x}..{:#010x} size {:>9}, {:#x}..{:#x} of "
                          "the segment's own half",
                          i, i < virtuals ? "virtual" : "physical", built_for, built_for + size,
                          size, running, running + size);
        running += size;
    }
}

REXCVAR_DEFINE_BOOL(inflate_trace, false, "MCLA/Diag",
    "Log every call into zlibInflater's decode step, for the first few hundred.\n"
    "\n"
    "A repacked resource that never arrives leaves no other trace: the streamer "
    "opens the file, reads it, and then either spins in sub_821BC140 or asserts "
    "that the stream is not in XCompress format. Both of those follow from one "
    "number -- how much input the decoder actually consumed on a call -- and "
    "this prints it. Read consecutive lines: input left falling says it ate "
    "something, output left falling says it produced something, and a pair of "
    "identical lines is the decoder refusing to move.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Entry of zlibInflater's step. r4 is the context: +0 input left, +4 input
// pointer, +8 input consumed, +12 stream length, +16 output space left,
// +20 output pointer. Pure observer, capped so a healthy boot cannot drown the
// log.
void MCLA_InflateStep(PPCRegister& r3, PPCRegister& r4) {
    if (!REXCVAR_GET(inflate_trace)) return;

    const auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;
    const uint32_t ctx = static_cast<uint32_t>(r4.u64);
    if (ctx < 0x1000 || ctx >= 0xC0000000) return;

    auto word = [&](uint32_t offset) {
        const uint8_t* p = base + ctx + offset;
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) |
               uint32_t(p[3]);
    };

    struct Step {
        uint32_t ctx, in_left, in_ptr, consumed, len, out_left;
    };
    const Step now{ctx, word(0), word(4), word(8), word(12), word(16)};

    // A healthy boot makes tens of thousands of these calls, so printing them
    // all buries the one that matters. What matters is a call that changes
    // nothing: the decoder was handed input and output room and moved neither,
    // which is the shape of both failures seen here -- the read loop only
    // refills when the input buffer empties, so a decoder that will not move
    // leaves the game turning on the spot with no I/O and no assert. So keep a
    // short history, watch for repeats, and print only when one is found.
    static std::mutex mutex;
    static Step ring[12]{};
    static uint32_t depth = 0, repeats = 0;
    static bool reported = false;

    std::lock_guard<std::mutex> lock(mutex);

    // Every resource starts 05 43 53 52 -- "RSC" and its version -- and the read
    // window begins twelve bytes before the inflater's pointer, so a first call
    // whose head is anything else is a resource that did not arrive. That is the
    // whole anomaly worth printing: the game's own files all read correctly, so a
    // filter on the head is quiet on a healthy boot and names the broken file by
    // its size. A few good ones go in first as the reference.
    static uint32_t good = 0, bad = 0;
    if (now.consumed == 0) {
        const uint32_t from = now.in_ptr - 12;
        if (from >= 0x1000 && from < 0xC0000000 - 0x40) {
            const bool is_resource = base[from] == 0x05 && base[from + 1] == 0x43 &&
                                     base[from + 2] == 0x53 && base[from + 3] == 0x52;
            const bool want = is_resource ? (now.len > 20000 && good < 4) : (bad < 12);
            if (want) {
                if (is_resource) ++good; else ++bad;
                std::string head;
                for (uint32_t i = 0; i < 24; ++i) {
                    char byte[8] = {};
                    std::snprintf(byte, sizeof byte, " %02X", base[from + i]);
                    head += byte;
                }
                if (is_resource) {
                    LARECOMP_APP_INFO("[inflate] arrived: in_left={} len={} out_left={} "
                                      "head:{}",
                                      now.in_left, now.len, now.out_left, head);
                } else {
                    LARECOMP_APP_ERROR("[inflate] NOT A RESOURCE: the read window holds no "
                                       "RSC5 header. in_left={} len={} out_left={} "
                                       "in_ptr={:#010x} head:{}",
                                       now.in_left, now.len, now.out_left, now.in_ptr, head);
                }
            }
        }
    }

    if (reported) return;

    // The streamer reuses one context, and the game ships families of resources
    // whose sizes are identical, so two different files in a row can look like
    // the same call repeating. Eight in a row is past coincidence.
    const Step& last = ring[(depth + 11) % 12];
    const bool stuck = depth != 0 && now.ctx == last.ctx && now.in_left == last.in_left &&
                       now.out_left == last.out_left && now.consumed == last.consumed &&
                       now.len == last.len && now.in_ptr == last.in_ptr;
    repeats = stuck ? repeats + 1 : 0;
    ring[depth % 12] = now;
    ++depth;

    if (repeats < 8) return;
    reported = true;

    LARECOMP_APP_ERROR("[inflate] STALLED: the decoder has taken nothing and produced "
                       "nothing {} times running. Last {} calls, oldest first:",
                       repeats + 1, depth < 12 ? depth : 12u);
    const uint32_t count = depth < 12 ? depth : 12u;
    for (uint32_t i = 0; i < count; ++i) {
        const Step& s = ring[(depth - count + i) % 12];
        LARECOMP_APP_ERROR("[inflate]   ctx={:#010x} in_left={:6} in_ptr={:#010x} "
                           "consumed={:7} len={:7} out_left={:6}",
                           s.ctx, s.in_left, s.in_ptr, s.consumed, s.len, s.out_left);
    }
    (void)r3;
}

// Tune field registration probe. sub_824DF200(owner, type, name, &field, ...).
//
// `owner` is the class descriptor for a top-level field, but a field of type 13 is a
// nested sub-object and everything registered after it reports that sub-object as its
// owner -- so owner/field together give the tree, not a flat list.
//
// Known limit: the vehicle handling tune does NOT come through here. A full capture
// (boot through gameplay) yields ~1100 fields across cameras, HUD, effects, AI, input
// and cop lights, and no vehicle physics class at all -- no SteeringLimit, TurnBias,
// SlidingFric or OptSlipPercent. Do not spend another session looking for them here.
void MCLA_TuneFieldProbe(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6) {
    const auto* base = rex::Runtime::instance()->virtual_membase();
    const auto name_addr = static_cast<uint32_t>(r5.u64);
    if (!base || !name_addr) return;

    char name[64];
    size_t n = 0;
    while (n < sizeof(name) - 1) {
        const char c = static_cast<char>(base[name_addr + n]);
        if (!c) break;
        name[n++] = c;
    }
    name[n] = '\0';
    if (n == 0) return;

    // r6 is the address the field will live at, which is the only thing that
    // makes a tune writable from here without hardcoding an offset into one
    // instance of it. The camera free look listens for its own field names on
    // the way past; this runs whether or not the diagnostic below is on,
    // because the addresses are only announced once, at load.
    CameraLookOnTuneField(name, static_cast<uint32_t>(r3.u64),
                          static_cast<uint32_t>(r6.u64));

    if (!REXCVAR_GET(tune_field_probe)) return;

    static std::set<std::string> seen;
    if (!seen.insert(name).second) return;

    LARECOMP_APP_INFO("[TuneField] {:<26} type={} owner={:#010x} field={:#010x}", name,
                      static_cast<uint32_t>(r4.u64), static_cast<uint32_t>(r3.u64),
                      static_cast<uint32_t>(r6.u64));
}

// Archive list injection for the modloader. sub_822C4630 mounts a ';'-separated
// list of packfiles, all at "a:/archive/". Which list it uses is decided across
// three branches (the caller's argument, the "audlo" fallback, the
// dword_8288BA44 override) that merge at 0x822C4858, and 0x822C4860 copies the
// winner into a 511-byte stack buffer at r1+0xD0. This hook sits on the
// instruction right after that copy and appends to the buffer in place.
//
// Appending rather than repointing matters twice over: it keeps whichever list
// the game picked, and it writes to the guest stack. Parking a string in the
// dead stub region instead is not an option -- those pages belong to the XEX
// image and are mapped read-only, so writing there faults.
//
// xarchive_mods.rpf ends up mounted last, and fiDevice::GetDevice
// (sub_821CB488) searches a mount point's devices last-registered-first,
// falling through when one does not hold the file -- so the mod archive
// overrides per file and everything else still comes from the shipped ones.
void Patch_ArchiveList(PPCRegister& r1) {
    constexpr uint32_t kListBufferOffset = 0xD0;  // v43 in sub_822C4630's frame
    constexpr size_t kListBufferSize = 512;       // copied with a 511-byte bound
    mc::modloader::AppendModArchiveTo(static_cast<uint32_t>(r1.u64) + kListBufferOffset,
                                      kListBufferSize);
}

// Driver animations of a new car. The game files them under the car's name
// minus "vp_" and, when the pack is missing, falls back to the Challenger's
// (sub_823D2AB0) -- which in a cloned Impala's seat put the driver into the
// seat back with his hands off the wheel. The three buffers are renamed right
// after they are formatted, so the pack, its dictionary and every animation in
// it are the ones the donor's seat was authored for. See hooks.h.
void MCLA_DriverAnimPack(PPCRegister& r1) {
    mc::modloader::AliasDriverAnimName(static_cast<uint32_t>(r1.u64) + 0x80, 0x80);
}

void MCLA_DriverAnimDict(PPCRegister& r28, PPCRegister& r29) {
    mc::modloader::AliasDriverAnimName(static_cast<uint32_t>(r29.u64),
                                       static_cast<uint32_t>(r28.u64));
}

void MCLA_DriverAnimName(PPCRegister& r1) {
    mc::modloader::AliasDriverAnimName(static_cast<uint32_t>(r1.u64) + 0x50, 0x80);
}

// Fires at 0x822C22EC, right after the clock update (sub_821BDA90). The delta
// time itself is delivered at the clock source (MCLAFrameDelta /
// MCLAUseRealDelta / MCLAFixedStepPath) rather than overwritten late in the
// frame, which is what caused traffic jitter and physics stutter. What is left
// here is the per-frame housekeeping.
void Patch_DeltaTimePre() {
    TickVinylReadbackWindow();  // runs every frame regardless of real_frame_delta
    TickVinylShapeCapture();    // hands-free shape-catalog sweep, if requested
    TickButtonPrompts();        // picks up a live button_prompts change
    TickCustomMusic();          // custom radio: volume + end-of-track advance
    TickHudUnits();             // hud_speed_units: mph -> km/h, live
    TickCutsceneGallery();      // cutscene replay: close the menu, start the script
    TickMapMouse();             // full map: notices the screen closed, frees the cursor
    TickModGlows();             // guest scratch for the mods' light glows
    TickModBreakables();        // mods' breakable sector props: knock-over on a hard hit
    TickCameraLook();           // cam_freelook: mouse -> gameplay camera lookaround
    RpcOnRaceTick();            // Discord RPC: race name + series/tournament standings
    ApplyAmbientDensityTuning();  // no-op unless an ambient cvar moved
    ApplyFragTuneOverrides();     // re-asserts the fragment tune overrides
    ApplyRenderPhaseMask();       // perf_no_shadows, live
    ApplyLoadTimeDevOptions();    // keeps node+4 in sync for the load-time switches
    ApplyRubberBandLevel();       // forced AI difficulty tune index
    ApplyRubberBandScales();      // AI catch-up / hold-back limits
    LogRenderPhaseMaskOnce();     // a few samples of the real per-frame masks
    g_frame_heartbeat.fetch_add(1, std::memory_order_relaxed);
}

// The player's current district (return of Racer_GetCurrentDistrict). Fires on
// the game's own district queries -> the RPC updates the area live while driving.
void Hook_CaptureDistrict(PPCRegister& r3) {
    // rpc-diag: confirm the hook fires + what district it sees. Remove later.
    static int last_diag = -999;
    int idx = static_cast<int>(r3.u64);
    if (idx != last_diag) {
        last_diag = idx;
        LARECOMP_APP_INFO("[rpc-diag] district hook fired, idx={}", idx);
    }
    RpcOnDistrictChanged(idx);
}


// LZX streaming decompression probe. pgStreamer worker threads decompress
// world resources through zlibInflater::InflateBegin (sub_821D5E10), which
// wraps the statically linked XMemDecompressStream (sub_8244FF20, XCompress
// LZX, 128KB window) — all of it recompiled guest code. The pair of hooks
// brackets that call: Pre fires at 0x821D5EB4 (just before the bl), Post at
// 0x821D5EBC (first instruction after it). The wrapper keeps its in/out sizes
// in stack slots: [r1+0x50] holds the source bytes offered (consumed after the
// call) and [r1+0x54] the destination capacity (bytes produced after the
// call). Two worker threads run this concurrently, hence thread_local pairing
// and atomic totals. Results append to <exe>/lzx_stats.txt every 2 seconds
// while the lzx_stats cvar is on.
namespace {

struct LzxWindow {
    uint64_t calls = 0;
    uint64_t ns = 0;
    uint64_t src_bytes = 0;
    uint64_t dst_bytes = 0;
    uint64_t errors = 0;
};

std::atomic<uint64_t> g_lzx_calls{0};
std::atomic<uint64_t> g_lzx_ns{0};
std::atomic<uint64_t> g_lzx_src_bytes{0};
std::atomic<uint64_t> g_lzx_dst_bytes{0};
std::atomic<uint64_t> g_lzx_max_ns{0};
std::atomic<uint64_t> g_lzx_errors{0};
std::atomic<int64_t> g_lzx_last_dump_ns{0};
std::mutex g_lzx_dump_mutex;
LzxWindow g_lzx_prev;

thread_local int64_t tl_lzx_start_ns = 0;

int64_t LzxNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void LzxDump(int64_t now_ns, int64_t prev_dump_ns) {
    std::lock_guard<std::mutex> lock(g_lzx_dump_mutex);

    LzxWindow cur;
    cur.calls = g_lzx_calls.load(std::memory_order_relaxed);
    cur.ns = g_lzx_ns.load(std::memory_order_relaxed);
    cur.src_bytes = g_lzx_src_bytes.load(std::memory_order_relaxed);
    cur.dst_bytes = g_lzx_dst_bytes.load(std::memory_order_relaxed);
    cur.errors = g_lzx_errors.load(std::memory_order_relaxed);
    uint64_t max_ns = g_lzx_max_ns.exchange(0, std::memory_order_relaxed);

    double wall_ms = double(now_ns - prev_dump_ns) / 1e6;
    double busy_ms = double(cur.ns - g_lzx_prev.ns) / 1e6;
    double out_mb = double(cur.dst_bytes - g_lzx_prev.dst_bytes) / (1024.0 * 1024.0);
    double in_mb = double(cur.src_bytes - g_lzx_prev.src_bytes) / (1024.0 * 1024.0);
    uint64_t calls = cur.calls - g_lzx_prev.calls;
    uint64_t errors = cur.errors - g_lzx_prev.errors;

    std::error_code ec;
    std::filesystem::path file = std::filesystem::current_path(ec) / "lzx_stats.txt";
    if (ec) return;
    std::ofstream out(file, std::ios::app);
    if (!out) return;

    char line[320];
    std::snprintf(line, sizeof(line),
                  "wall=%.0fms calls=%llu busy=%.2fms busy_pct=%.1f%% in=%.2fMB out=%.2fMB "
                  "out_rate=%.1fMB/s max_call=%.0fus errors=%llu | total: calls=%llu busy=%.0fms "
                  "out=%.1fMB\n",
                  wall_ms, static_cast<unsigned long long>(calls), busy_ms,
                  wall_ms > 0.0 ? busy_ms * 100.0 / wall_ms : 0.0, in_mb, out_mb,
                  wall_ms > 0.0 ? out_mb * 1000.0 / wall_ms : 0.0, double(max_ns) / 1e3,
                  static_cast<unsigned long long>(errors),
                  static_cast<unsigned long long>(cur.calls), double(cur.ns) / 1e6,
                  double(cur.dst_bytes) / (1024.0 * 1024.0));
    out << line;

    g_lzx_prev = cur;
}

}  // namespace

void Hook_LzxDecompressPre(PPCRegister& r1) {
    (void)r1;
    if (!REXCVAR_GET(lzx_stats)) {
        tl_lzx_start_ns = 0;
        return;
    }
    tl_lzx_start_ns = LzxNowNs();
}

void Hook_LzxDecompressPost(PPCRegister& r1, PPCRegister& r3) {
    if (!tl_lzx_start_ns) return;
    int64_t now = LzxNowNs();
    uint64_t dur = uint64_t(now - tl_lzx_start_ns);
    tl_lzx_start_ns = 0;

    auto* rt = rex::Runtime::instance();
    if (!rt) return;
    auto* mem = rt->memory();
    if (!mem) return;

    uint32_t sp = static_cast<uint32_t>(r1.u64);
    // After XMemDecompressStream returns: [sp+0x50] = source bytes consumed,
    // [sp+0x54] = destination bytes produced (the wrapper advances its
    // pointers by exactly these values right after the call).
    uint32_t consumed = GuestRead32(mem, sp + 0x50);
    uint32_t produced = GuestRead32(mem, sp + 0x54);

    g_lzx_calls.fetch_add(1, std::memory_order_relaxed);
    g_lzx_ns.fetch_add(dur, std::memory_order_relaxed);
    g_lzx_src_bytes.fetch_add(consumed, std::memory_order_relaxed);
    g_lzx_dst_bytes.fetch_add(produced, std::memory_order_relaxed);

    uint64_t prev_max = g_lzx_max_ns.load(std::memory_order_relaxed);
    while (dur > prev_max &&
           !g_lzx_max_ns.compare_exchange_weak(prev_max, dur, std::memory_order_relaxed)) {
    }

    // 0x81DE2001 is the "needs more input" status the game itself tolerates.
    int32_t status = static_cast<int32_t>(r3.u64);
    if (status < 0 && status != int32_t(0x81DE2001)) {
        g_lzx_errors.fetch_add(1, std::memory_order_relaxed);
    }

    int64_t last = g_lzx_last_dump_ns.load(std::memory_order_relaxed);
    if (now - last >= 2'000'000'000 &&
        g_lzx_last_dump_ns.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
        // First window after enabling has no baseline timestamp — skip the dump,
        // the totals still carry into the next one.
        if (last != 0) LzxDump(now, last);
    }
}
#else // REXGLUE_HAS_XEO3_TARGET
// XEO3 stubs: empty implementations so the linker resolves codegen calls.

#include <rex/ppc/context.h>

void Hook_CacheVinylPaint(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6) {}
void Hook_PhotoModeCapture(PPCRegister& r3) {}
void ExportVinyl() {}
void DumpVinylShapes() {}
void RequestVinylShapeCapture() {}
void TickVinylShapeCapture() {}
void InitHooks() {}
void Patch_DeltaTimePre() {}
void Hook_CaptureDistrict(PPCRegister& r3) {}
void Hook_LzxDecompressPre(PPCRegister& r1) {}
void Hook_LzxDecompressPost(PPCRegister& r1, PPCRegister& r3) {}
void MCLA_StreamOpenResult(PPCRegister& r1, PPCRegister& r3) {}
void MCLA_RageFatal(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6) {}
void MCLA_FileOpenResult(PPCRegister& r1, PPCRegister& r3) {}
void MCLA_DriverAnimPack(PPCRegister& r1) {}
void MCLA_DriverAnimDict(PPCRegister& r28, PPCRegister& r29) {}
void MCLA_DriverAnimName(PPCRegister& r1) {}
void MCLA_InflateStep(PPCRegister& r3, PPCRegister& r4) {}
void MCLA_ResourceFixupError(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5) {}
void MCLA_ResourceSegments(PPCRegister& r31) {}
void MCLA_TuneFieldProbe(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6) {}
#endif // REXGLUE_HAS_XEO3_TARGET
