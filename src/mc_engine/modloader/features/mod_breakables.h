#pragma once
//
// Props a mod baked into its city sectors that break off when the car hits
// them fast enough (models/<mod>/breakables.bin, see mod_breakables.cpp).
//
//   cvars: mod_breakables, mod_breakable_speed_scale
//

#include <cstdint>

// Once per frame on the game thread (Patch_DeltaTimePre).
void TickModBreakables();

// True while prop `id` of mod folder `mod` (FindModDataFiles' index) lies
// knocked over; mod_glows skips the glows linked to it.
bool ModBreakableDown(int mod, uint32_t id);
