#pragma once
//
// Host-side data a mod carries next to its files/ folder, where the modloader
// will not pack it into the archive:
//
//   <exe>/models/<mod>/<name>
//
// Used by the features a mod can feed without code of its own (mod_glows,
// mod_breakables, mod_lights).
//

#include <filesystem>
#include <string>
#include <vector>

struct ModDataFile {
    int mod;                     // stable index of the mod folder (sorted by name)
    std::string mod_name;
    std::filesystem::path path;
};

// Every <exe>/models/<mod>/<name> that exists, in mod-name order.
std::vector<ModDataFile> FindModDataFiles(const char* name);
