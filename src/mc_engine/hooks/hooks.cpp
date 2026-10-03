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

// NOTE: the online cvars (online_ignore_content_check, online_diag) moved to
// src/mc_engine/online/system_link.cpp along with the hooks that use them.

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
#else // REXGLUE_HAS_XEO3_TARGET
// XEO3 stubs: empty implementations so the linker resolves codegen calls.

#include <rex/ppc/context.h>

void InitHooks() {}
void Patch_DeltaTimePre() {}
#endif // REXGLUE_HAS_XEO3_TARGET
