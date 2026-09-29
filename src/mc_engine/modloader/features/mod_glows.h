#pragma once
//
// Light glows a mod places in the world (models/<mod>/glows.bin), drawn by
// MCLA's own glow renderer (see mod_glows.cpp). The glows go in from the prop
// manager's glow pass; this only allocates the guest scratch they are passed
// through.
//
//   cvars: mod_glows, mod_glows_max, mod_glow_size, mod_glow_intensity
//

// Once per frame on the game thread (Patch_DeltaTimePre).
void TickModGlows();
