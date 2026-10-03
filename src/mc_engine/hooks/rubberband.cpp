// AI rubberband (mcRubberBandMgr): the off switch, a forced difficulty level,
// scales over the parsed RubberBandTune entries, and a dump of them.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/cvar.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "hooks.h"
#include "hooks_internal.h"
#include "larecomp_log.h"

REXCVAR_DEFINE_BOOL(disable_rubberbanding, false, "MCLA/Patches",
    "Disable the AI RubberBand system outright. Note this also removes MinThrottle, the "
    "leash that slows the AI when it is AHEAD — so it makes races harder, not fairer. "
    "Prefer the rubberband_* scales under MCLA/Difficulty.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// ── AI rubberband scales ────────────────────────────────────────────────────
// Applied on top of whatever $/tune/career/RubberBandTune00..10 loaded, to all
// 11 entries. 1.0 leaves the tune exactly as shipped.
REXCVAR_DEFINE_BOOL(rubberband_dump, false, "MCLA/Difficulty",
    "Write every RubberBandTune entry to <exe>/rubberband_dump.txt and flip back off. "
    "Read it before picking the scales below.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(rubberband_catchup_scale, 1.0, "MCLA/Difficulty",
    "Scales how hard the AI pulls when it is BEHIND: MaxThrottle, BehindMaxThrottle, "
    "HomeStretchBehindMaxThrottle, PlayerCloseMaxThrottle. Below 1 = less catch-up.")
    .range(0.0, 2.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(rubberband_holdback_scale, 1.0, "MCLA/Difficulty",
    "Scales MinThrottle, the throttle cap while the AI is AHEAD. Below 1 = the leader "
    "backs off harder; above 1 = it runs away more freely.")
    .range(0.0, 2.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// The 11 RubberBandTune entries are difficulty levels. sub_82743B30
// (RubberBandMgr_SetupRace) picks one into mgr+60: it starts from the race's
// base (mgr+56) and then, when the race got clipped down to the racer's own
// level, subtracts mgr+68 (2) — or adds mgr+72 (3) when the racer is below what
// the race asked for. The game's own `rubberband` dev switch (node 0x8290AC10)
// overrides that index outright, clamped 0..10, which is what this reproduces —
// written every frame so it also survives the restore path at the end of setup.
REXCVAR_DEFINE_INT32(rubberband_level, -1, "MCLA/Difficulty",
    "Force which RubberBandTune level the AI races at: -1 leaves the game's own choice, "
    "0 = easiest, 10 = hardest. This is the direct way to make races harder — the tune "
    "index the game normally derives from your racer level and the race's base.")
    .range(-1, 10)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(rubberband_distance_scale, 1.0, "MCLA/Difficulty",
    "Scales the distance bands the rubberband reacts within: MinDist, MaxDist, "
    "BehindMinDist, BehindMaxDist. Larger = the band engages from further away.")
    .range(0.1, 4.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// AI nitrous policy. The tune gates nitrous on four distances; raising them
// makes the AI hold it instead of dumping the bottle off the line.
//
// Direction of each comparison is NOT confirmed by RE — the names and offsets
// are (parser registration sub_823990E0), and LoadTuning rescales two of them
// (+48 *= 2, +56 *= 2.5) after parsing. Read rubberband_dump.txt for the real
// numbers and try one knob at a time.
// Absolute, not a scale: measured across all 11 levels, UseNitroMinDistFromStart
// is a flat 100 at every one of them — the only field in the struct with no
// difficulty ramp. Scaling a constant tells you nothing about what you asked
// for, and 100 is short enough that even x8 lands inside the opening straight.
REXCVAR_DEFINE_DOUBLE(rubberband_nitro_start_dist, -1.0, "MCLA/Difficulty",
    "UseNitroMinDistFromStart in world units: how far into the race the AI must be "
    "before it may touch nitrous. Stock is 100 at every difficulty level, which is why "
    "they empty the bottle off the line. -1 keeps stock.")
    .range(-1.0, 8000.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(rubberband_nitro_finish_scale, 1.0, "MCLA/Difficulty",
    "Scales UseNitroMinDistFromFinish, the gate tied to the run-in to the finish.")
    .range(0.0, 8.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(rubberband_nitro_behind_scale, 1.0, "MCLA/Difficulty",
    "Scales UseNitroMinDistBehindPlayer and UnlimitedNitroMinDistBehindPlayer — how far "
    "behind you the AI has to be before it spends nitrous, and before it gets unlimited.")
    .range(0.0, 8.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

bool Patch_DisableRubberBanding() {
    return REXCVAR_GET(disable_rubberbanding);
}

// ── mcRubberBandMgr tuning ──────────────────────────────────────────────────
//
// sub_827440E8 allocates the 1876-byte manager and parks it in dword_8290AC0C;
// sub_82743A10 (RubberBandMgr_LoadTuning) fills 11 mcRubberBandTuning entries at
// mgr+76, stride 140, from $/tune/career/RubberBandTune00..10 — then rescales a
// few of them in code (+48 *= 2, +56 *= 2.5, +64 *= 2), so the file value is not
// the final value. Field names and offsets come from the parser registration in
// sub_823990E0, same shape as fragTuneStruct.
//
// disable_rubberbanding kills the whole system, which also removes MinThrottle —
// the leash that slows the AI down when it is AHEAD. That is why turning it off
// makes races harder rather than fairer. Scaling the individual limits keeps
// both halves and just changes how hard they pull.
static constexpr uint32_t kRubberBandMgrPtr = 0x8290AC0Cu;
static constexpr uint32_t kRubberBandFirst = 76u;
static constexpr uint32_t kRubberBandStride = 140u;
static constexpr int kRubberBandCount = 11;

struct RubberBandField {
    uint32_t offset;
    const char* name;
};

// The scalar members. +76 AheadDistOffsets and +108 BehindDistOffsets are
// 8-float arrays and are dumped separately.
static constexpr RubberBandField kRubberBandFields[] = {
    {0,  "MaxThrottle"},
    {4,  "MinSpeed"},
    {8,  "MinDist"},
    {12, "MaxDist"},
    {16, "MinThrottle"},
    {20, "BehindMinDist"},
    {24, "BehindMaxDist"},
    {28, "BehindMaxThrottle"},
    {32, "OutsideTrafficBubbleThresholdSpeed"},
    {36, "OutsideTrafficBubbleThrottlePenalty"},
    {40, "DistBehindBeforeTakingShortcuts"},
    {44, "UseNitroMinDistFromStart"},
    {48, "UseNitroMinDistFromFinish"},
    {52, "UseNitroMinDistBehindPlayer"},
    {56, "UnlimitedNitroMinDistBehindPlayer"},
    {60, "HomeStretchBehindMaxThrottle"},
    {64, "HomeStretchDistance"},
    {68, "PlayerCloseMaxThrottle"},
    {72, "PlayerCloseDistance"},
};

// Writes every entry to <exe>/rubberband_dump.txt. The knobs below are meant to
// be chosen from these numbers rather than guessed at.
void DumpRubberBandTuning() {
    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;
    const uint32_t mgr = ReadGuestU32(base, kRubberBandMgrPtr);
    if (mgr == 0) {
        LARECOMP_APP_ERROR("[RubberBand] manager not created yet");
        return;
    }

    std::error_code ec;
    std::ofstream out(std::filesystem::current_path(ec) / "rubberband_dump.txt");
    if (!out) {
        LARECOMP_APP_ERROR("[RubberBand] cannot open rubberband_dump.txt");
        return;
    }
    out << "mcRubberBandMgr 0x" << std::hex << mgr << std::dec
        << "  (11 x mcRubberBandTuning, 140 bytes, at mgr+76)\n"
        << "values are post-LoadTuning: +48 *= 2, +56 *= 2.5, +64 *= 2\n\n";

    for (int i = 0; i < kRubberBandCount; ++i) {
        const uint32_t e = mgr + kRubberBandFirst + uint32_t(i) * kRubberBandStride;
        out << "RubberBandTune" << (i < 10 ? "0" : "") << i << "  @0x" << std::hex << e
            << std::dec << "\n";
        for (const auto& f : kRubberBandFields) {
            char line[128];
            std::snprintf(line, sizeof(line), "  +%-3u %-36s %g\n", f.offset, f.name,
                          double(ReadGuestF32(base, e + f.offset)));
            out << line;
        }
        for (const char* which : {"AheadDistOffsets", "BehindDistOffsets"}) {
            const uint32_t off = (which[0] == 'A') ? 76u : 108u;
            out << "  +" << off << " " << which << " ";
            for (int k = 0; k < 8; ++k) out << ReadGuestF32(base, e + off + 4u * k) << " ";
            out << "\n";
        }
        out << "\n";
    }
    LARECOMP_APP_INFO("[RubberBand] dumped {} entries to rubberband_dump.txt", kRubberBandCount);
}

// Which fields each scale drives. `throttle` marks the normalised 0..1 fields,
// which are not pushed past 1.0 when scaling up.
struct RubberBandScale {
    const char* cvar;
    uint32_t offsets[4];  // 0-terminated
    bool throttle;
    // Scales multiply the loaded value and are neutral at 1.0. Absolute knobs
    // write the value straight in and are neutral at -1 — used where the shipped
    // tune is the same at every difficulty level, so a multiplier would be
    // meaningless to reason about.
    bool absolute;
};

static constexpr RubberBandScale kRubberBandScales[] = {
    {"rubberband_catchup_scale",      {0, 28, 60, 68}, true,  false},  // *MaxThrottle
    {"rubberband_holdback_scale",     {16, 0, 0, 0},   true,  false},  // MinThrottle
    {"rubberband_distance_scale",     {8, 12, 20, 24}, false, false},  // *Dist bands
    {"rubberband_nitro_start_dist",   {44, 0, 0, 0},   false, true},   // UseNitroMinDistFromStart
    {"rubberband_nitro_finish_scale", {48, 0, 0, 0},   false, false},  // UseNitroMinDistFromFinish
    {"rubberband_nitro_behind_scale", {52, 56, 0, 0},  false, false},  // *MinDistBehindPlayer
};

static bool RubberBandGroupNeutral(const RubberBandScale& g, double v) {
    return g.absolute ? (v < 0.0) : (v == 1.0);
}

// Offset 0 is a real field (MaxThrottle) and only ever appears first, so a zero
// past index 0 is the terminator.
static constexpr size_t RubberBandFieldTotal() {
    size_t n = 0;
    for (const auto& g : kRubberBandScales)
        for (size_t i = 0; i < 4; ++i)
            if (i == 0 || g.offsets[i] != 0) ++n;
    return n;
}

// Same shape as the fragTune override: while every scale is 1.0 the loaded
// values are re-read as the baseline, so the scales always multiply the tune the
// game actually parsed rather than compounding on themselves. Throttle fields
// are clamped back to their original ceiling when that ceiling was <= 1.0, since
// those read as normalised throttle and overshooting them is not something the
// shipped tune ever does.
// mgr+60 holds the index of the RubberBandTune entry in force. Writing it every
// frame beats both places the game sets it: sub_82743B30 computes it at race
// setup, and the tail of that function can overwrite it again from the stored
// value at dword_8286EC70+2616.
static constexpr uint32_t kRubberBandLevelField = 60u;

void ApplyRubberBandLevel() {
    static int last_logged = -2;
    const int want = int(std::strtol(rex::cvar::GetFlagByName("rubberband_level").c_str(),
                                     nullptr, 10));
    if (want < 0) {
        last_logged = -2;
        return;
    }

    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;
    const uint32_t mgr = ReadGuestU32(base, kRubberBandMgrPtr);
    if (mgr == 0) return;

    const uint32_t clamped = uint32_t(want > 10 ? 10 : want);
    if (ReadGuestU32(base, mgr + kRubberBandLevelField) != clamped)
        WriteGuestU32(base, mgr + kRubberBandLevelField, clamped);

    if (last_logged != want) {
        last_logged = want;
        LARECOMP_APP_INFO("[RubberBand] forcing tune level {} (mgr+60)", clamped);
    }
}

void ApplyRubberBandScales() {
    static constexpr size_t kFieldCount = RubberBandFieldTotal();
    static float baseline[kRubberBandCount][kFieldCount] = {};
    static bool have_base = false;
    static bool was_scaled = false;

    double scale[std::size(kRubberBandScales)];
    bool neutral = true;
    for (size_t g = 0; g < std::size(kRubberBandScales); ++g) {
        scale[g] = std::strtod(rex::cvar::GetFlagByName(kRubberBandScales[g].cvar).c_str(),
                               nullptr);
        if (!RubberBandGroupNeutral(kRubberBandScales[g], scale[g])) neutral = false;
    }

    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;
    const uint32_t mgr = ReadGuestU32(base, kRubberBandMgrPtr);
    if (mgr == 0) return;
    // Entry 0 is still zero until LoadTuning has run; do not snapshot that.
    if (ReadGuestF32(base, mgr + kRubberBandFirst + 12) == 0.0f) return;

    // Dump once, automatically, the first frame the tune is actually parsed —
    // the scales below are only meaningful against these numbers, and waiting
    // for someone to press a button means the file is usually missing when it
    // is needed. The rubberband_dump cvar still forces a fresh one on demand.
    static bool auto_dumped = false;
    if (!auto_dumped) {
        auto_dumped = true;
        DumpRubberBandTuning();
    }

    // Every field this touches, in the order the baseline stores them.
    auto for_each_field = [&](auto&& fn) {
        size_t slot = 0;
        for (size_t g = 0; g < std::size(kRubberBandScales); ++g) {
            const auto& grp = kRubberBandScales[g];
            for (size_t i = 0; i < 4; ++i) {
                if (i != 0 && grp.offsets[i] == 0) break;
                fn(slot++, grp.offsets[i], scale[g], grp.throttle, grp.absolute);
            }
        }
    };

    if (neutral) {
        if (was_scaled) {
            for (int i = 0; i < kRubberBandCount; ++i) {
                const uint32_t e = mgr + kRubberBandFirst + uint32_t(i) * kRubberBandStride;
                for_each_field([&](size_t slot, uint32_t off, double, bool, bool) {
                    WriteGuestF32(base, e + off, baseline[i][slot]);
                });
            }
            was_scaled = false;
            LARECOMP_APP_INFO("[RubberBand] scales cleared, shipped tune restored");
            return;
        }
        // Keep the baseline tracking whatever LoadTuning last produced.
        for (int i = 0; i < kRubberBandCount; ++i) {
            const uint32_t e = mgr + kRubberBandFirst + uint32_t(i) * kRubberBandStride;
            for_each_field([&](size_t slot, uint32_t off, double, bool, bool) {
                baseline[i][slot] = ReadGuestF32(base, e + off);
            });
        }
        have_base = true;
        return;
    }

    for (int i = 0; i < kRubberBandCount; ++i) {
        const uint32_t e = mgr + kRubberBandFirst + uint32_t(i) * kRubberBandStride;
        for_each_field([&](size_t slot, uint32_t off, double v, bool is_throttle,
                           bool is_absolute) {
            if (!have_base) baseline[i][slot] = ReadGuestF32(base, e + off);
            const float orig = baseline[i][slot];
            if (is_absolute) {
                // Neutral for this group: leave the shipped value in place.
                if (v < 0.0) {
                    WriteGuestF32(base, e + off, orig);
                    return;
                }
                WriteGuestF32(base, e + off, float(v));
                return;
            }
            double out = double(orig) * v;
            // Throttles are normalised — measured 0.75..1.0 across the 11 levels
            // — so do not push them past 1.0 when scaling up.
            if (is_throttle && orig <= 1.0f && out > 1.0) out = 1.0;
            WriteGuestF32(base, e + off, float(out));
        });
    }
    have_base = true;

    if (!was_scaled) {
        std::string what;
        for (size_t g = 0; g < std::size(kRubberBandScales); ++g) {
            const auto& grp = kRubberBandScales[g];
            if (RubberBandGroupNeutral(grp, scale[g])) continue;
            char buf[96];
            std::snprintf(buf, sizeof(buf), "%s%s %s%g", what.empty() ? "" : ", ",
                          grp.cvar + 11,  // skip "rubberband_"
                          grp.absolute ? "= " : "x", scale[g]);
            what += buf;
        }
        LARECOMP_APP_INFO("[RubberBand] {} on {} entries", what, kRubberBandCount);
    }
    was_scaled = true;
}
#else // REXGLUE_HAS_XEO3_TARGET
// XEO3 stubs: empty implementations so the linker resolves codegen calls.

#include <rex/ppc/context.h>

bool Patch_DisableRubberBanding() { return false; }
#endif // REXGLUE_HAS_XEO3_TARGET
