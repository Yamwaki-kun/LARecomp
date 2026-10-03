// City density: the ambient traffic, pedestrian and parked car tuning of every
// ambient zone, and the fragTuneStruct overrides (breakable prop draw distance
// and the framerate floor for new breaks).

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/cvar.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>

#include "hooks.h"
#include "hooks_internal.h"
#include "larecomp_log.h"

// BadassBaboon's Recomp Adjustments: Ambient traffic & pedestrian density tuning for city performance
REXCVAR_DEFINE_BOOL(enable_ambient_tuning, true, "MCLA/Performance",
    "Enable ambient traffic and pedestrian density tuning.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(traffic_unspawn_dist, 250.0, "MCLA/Performance",
    "Traffic vehicle unspawn radius in meters (default 400.0, lower = higher FPS in city).")
    .range(100.0, 600.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(ped_density_scale, 0.5, "MCLA/Performance",
    "Pedestrian density scale multiplier (0.0 = none, 0.5 = half, 1.0 = full).")
    .range(0.0, 2.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(parked_car_scale, 0.5, "MCLA/Performance",
    "Parked car density scale multiplier (0.0 = none, 0.5 = half, 1.0 = full).")
    .range(0.0, 2.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// ── rage::fragTuneStruct overrides ──────────────────────────────────────────
// Both values live in the fragment tune the game parses out of
// $/tune/types/fragments (fragment = breakable prop: poles, signs, fences,
// barriers). They are patched in the parsed struct at runtime, so no RPF edit
// and no decryption is involved. 0 keeps whatever the tune file loaded.
REXCVAR_DEFINE_DOUBLE(global_max_draw_distance, 0.0, "MCLA/Performance",
    "fragTuneStruct::GlobalMaxDrawingDistance — draw distance for breakable props "
    "(poles, signs, fences). 0 = keep the tune file's value (3000); the engine's own "
    "constructor default is 250.")
    .range(0.0, 6000.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(breaking_frame_rate_limit, 0.0, "MCLA/Performance",
    "fragTuneStruct::BreakingFrameRateLimit — framerate floor under which the engine "
    "stops spawning new fragment breaks. 0 = keep the tune file's value (10); the "
    "engine's own constructor default is 30.")
    .range(0.0, 120.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// BadassBaboon's Recomp Adjustments: ambient traffic / pedestrian density.
//
// mcAmbientDensityTuning is not a separate object: sub_826F5CB0 calls its
// constructor sub_826F5B18 with its OWN `this` (`sub_826F5B18(a1)`), so the
// tuning fields are the base of the ~5936-byte ambient zone. The zone array is
// built in sub_826D8E70 -- 31 zones, `v5 = manager + 56476`, stride 1484 dwords
// -- and each one re-parses $/tune/ambients/density_tuning.xml through
// sub_826F4CB8, whose r31 is that zone. r31 is therefore the tuning object,
// and 31 hook firings per load is expected, one per zone.
//
// (The value passed as the parse's 4th argument is NOT the instance: it is the
// shared class descriptor returned by vtable slot 1, identical across all 31
// zones. Writing tuning fields through it corrupts a live RAGE structure.)
//
// The override has to land AFTER the parse -- hooking the constructor is
// pointless because the parse overwrites every field it set.
//
// Per-zone originals are captured from what the parse left behind, and every
// override is computed from those, so re-parsing a zone never compounds the
// scale the way the original code did.
struct DensityTuningValues {
    float spawn = 0.0f;
    float unspawn = 0.0f;
    float cull = 0.0f;
    float ped = 0.0f;
    float parked = 0.0f;
};

static std::mutex g_density_mutex;
static std::map<uint32_t, DensityTuningValues> g_density_orig;  // zone address -> XML values

// Last state pushed into guest memory, so the per-frame tick only writes when a
// cvar actually moved.
static bool g_density_applied_valid = false;
static bool g_density_applied_enabled = false;
static DensityTuningValues g_density_applied;

// Writes one zone. Caller holds g_density_mutex.
// Offsets verified via rage::mcAmbientDensityTuning in mcla_rage_types.h:
//   +0x08 = spawn_max
//   +0x10 = unspawn_max
//   +0x14 = cull_max
//   +0x60 = ped_density (96)
//   +0x98 = parked_factor (152)
static void WriteDensityZone(uint8_t* base, uint32_t a, const DensityTuningValues& orig,
                             bool enabled, const DensityTuningValues& want) {
    if (!enabled) {
        WriteGuestF32(base, a + 8, orig.spawn);
        WriteGuestF32(base, a + 16, orig.unspawn);
        WriteGuestF32(base, a + 20, orig.cull);
        WriteGuestF32(base, a + 96, orig.ped);
        WriteGuestF32(base, a + 152, orig.parked);
        return;
    }
    float unspawn_val = want.unspawn > 0.0f ? want.unspawn : orig.unspawn;
    WriteGuestF32(base, a + 16, unspawn_val);
    if (orig.spawn > 0.0f) {
        WriteGuestF32(base, a + 8, orig.spawn * 0.75f);
    }
    if (orig.cull > 0.0f) {
        WriteGuestF32(base, a + 20, orig.cull * 0.75f);
    }
    WriteGuestF32(base, a + 96, orig.ped * want.ped);
    WriteGuestF32(base, a + 152, orig.parked * want.parked);
}

static void ReadDensityCvars(bool& enabled, DensityTuningValues& want) {
    enabled = REXCVAR_GET(enable_ambient_tuning);
    want.unspawn = static_cast<float>(REXCVAR_GET(traffic_unspawn_dist));
    want.ped = static_cast<float>(REXCVAR_GET(ped_density_scale));
    want.parked = static_cast<float>(REXCVAR_GET(parked_car_scale));
}

// 0x826F5CA0, in the epilogue of the mcAmbientDensityTuning constructor
// sub_826F5B18, after the last field write. r3 holds the ambient zone, i.e.
// the tuning object. See larecomp_config.toml for why this is NOT hooked at
// the density_tuning.xml parse - that path never executes.
void MCLAAmbientDensityTuning(PPCRegister& r3) {
    const uint32_t a = static_cast<uint32_t>(r3.u64);
    if (a == 0) return;

    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;

    bool enabled = false;
    DensityTuningValues want;
    ReadDensityCvars(enabled, want);

    std::lock_guard<std::mutex> lock(g_density_mutex);

    // The parse just restored this zone to its XML values, so re-reading here
    // is what keeps the scale from compounding across reloads.
    DensityTuningValues orig;
    orig.spawn = ReadGuestF32(base, a + 8);
    orig.unspawn = ReadGuestF32(base, a + 16);   // 400.0 stock
    orig.cull = ReadGuestF32(base, a + 20);
    orig.ped = ReadGuestF32(base, a + 96);       // 15.0 / XML stock
    orig.parked = ReadGuestF32(base, a + 152);   // 0.25 stock
    const bool first = g_density_orig.find(a) == g_density_orig.end();
    g_density_orig[a] = orig;

    WriteDensityZone(base, a, orig, enabled, want);

    // One line per zone the first time it is seen; reloads are silent, since 31
    // identical lines every district change is noise.
    if (first) {
        LARECOMP_APP_INFO(
            "[Ambient Tuning] zone {} at 0x{:08X}: unspawn {:.1f} -> {:.1f}, "
            "ped {:.4f} -> {:.4f}, parked {:.2f} -> {:.2f}",
            g_density_orig.size(), a, orig.unspawn, ReadGuestF32(base, a + 16), orig.ped,
            ReadGuestF32(base, a + 96), orig.parked, ReadGuestF32(base, a + 152));
    }
}

// Per-frame, so the cvars behave as the kHotReload they are declared to be.
// Costs a compare per frame and touches guest memory only when one moved.
void ApplyAmbientDensityTuning() {
    bool enabled = false;
    DensityTuningValues want;
    ReadDensityCvars(enabled, want);

    std::lock_guard<std::mutex> lock(g_density_mutex);
    if (g_density_applied_valid && g_density_applied_enabled == enabled &&
        g_density_applied.unspawn == want.unspawn && g_density_applied.ped == want.ped &&
        g_density_applied.parked == want.parked) {
        return;
    }
    g_density_applied_enabled = enabled;
    g_density_applied = want;
    g_density_applied_valid = true;

    if (g_density_orig.empty()) return;

    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;

    for (const auto& [addr, orig] : g_density_orig) {
        WriteDensityZone(base, addr, orig, enabled, want);
    }
    LARECOMP_APP_INFO("[Ambient Tuning] {} zones updated (enabled={}, unspawn={:.1f}, ped={:.2f}, parked={:.2f})",
                      g_density_orig.size(), enabled, want.unspawn, want.ped, want.parked);
}

// ── rage::fragTuneStruct overrides ──────────────────────────────────────────
//
// sub_82729B10 allocates the 1448-byte singleton and parks the pointer in
// dword_828D4CE4; sub_8275E518 is its constructor. The field offsets come from
// the parser registration in sub_8275E308, which writes each parMember's offset
// slot: the record naming "GlobalMaxDrawingDistance" (0x8282F370) gets 8 and the
// one naming "BreakingFrameRateLimit" (0x8282F400) gets 32. Both line up with
// the constructor defaults (+8 = 250.0f, +32 = 30.0f).
//
// The game overwrites both from $/tune/types/fragments after construction, so
// this runs per-frame and re-asserts the override on top of whatever the tune
// file loaded — no RPF edit, no decryption. A cvar left at 0 writes nothing and
// instead keeps re-reading the guest value, so switching back to 0 restores
// exactly what the tune file had.
static constexpr uint32_t kFragTunePtr        = 0x828D4CE4u;  // rage::fragTuneStruct*
static constexpr uint32_t kFragTuneDrawDist   = 8u;           // GlobalMaxDrawingDistance
static constexpr uint32_t kFragTuneBreakLimit = 32u;          // BreakingFrameRateLimit

struct FragTuneField {
    uint32_t offset;
    const char* cvar;
    const char* label;
    float stock = 0.0f;   // last value seen while the cvar was 0
    bool have_stock = false;
    float applied = 0.0f;
    bool have_applied = false;
};

static FragTuneField g_frag_tune[] = {
    {kFragTuneDrawDist,   "global_max_draw_distance",  "GlobalMaxDrawingDistance"},
    {kFragTuneBreakLimit, "breaking_frame_rate_limit", "BreakingFrameRateLimit"},
};

void ApplyFragTuneOverrides() {
    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;

    const uint32_t obj = ReadGuestU32(base, kFragTunePtr);
    if (obj == 0) return;

    for (auto& f : g_frag_tune) {
        const double want = std::strtod(rex::cvar::GetFlagByName(f.cvar).c_str(), nullptr);

        if (!(want > 0.0)) {
            // Auto: track whatever the tune file left there, and put it back once
            // if we had been overriding.
            if (f.have_applied) {
                if (f.have_stock) {
                    WriteGuestF32(base, obj + f.offset, f.stock);
                    LARECOMP_APP_INFO("[FragTune] {} restored to {:.1f}", f.label, f.stock);
                }
                f.have_applied = false;
            }
            f.stock = ReadGuestF32(base, obj + f.offset);
            f.have_stock = true;
            continue;
        }

        const float v = static_cast<float>(want);
        const float cur = ReadGuestF32(base, obj + f.offset);
        if (f.have_applied && f.applied == v && cur == v) continue;

        // If the field moved out from under us the game wrote it itself — the
        // tune file landing after construction — so that is the real stock
        // value, not whatever was in the struct before the parse.
        if (!f.have_stock || (f.have_applied && cur != f.applied)) {
            f.stock = cur;
            f.have_stock = true;
        }
        WriteGuestF32(base, obj + f.offset, v);
        if (!f.have_applied || f.applied != v) {
            LARECOMP_APP_INFO("[FragTune] {} {:.1f} -> {:.1f} (fragTuneStruct 0x{:08X}+{})",
                              f.label, f.stock, v, obj, f.offset);
        }
        f.applied = v;
        f.have_applied = true;
    }
}
#else // REXGLUE_HAS_XEO3_TARGET
// XEO3 stubs: empty implementations so the linker resolves codegen calls.

#include <rex/ppc/context.h>

void MCLAAmbientDensityTuning(PPCRegister& r3) {}
#endif // REXGLUE_HAS_XEO3_TARGET
