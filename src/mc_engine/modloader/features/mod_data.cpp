#include "mod_data.h"

#if defined(_WIN32)
#include <windows.h>
#endif

#include <algorithm>
#include <system_error>

namespace {

std::filesystem::path ModelsDir() {
    std::error_code ec;
#if defined(_WIN32)
    wchar_t exe[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH)) return std::filesystem::path(exe).parent_path() / L"models";
#endif
    return std::filesystem::current_path(ec) / "models";
}

}  // namespace

std::vector<ModDataFile> FindModDataFiles(const char* name) {
    std::vector<ModDataFile> out;
    std::error_code ec;
    const std::filesystem::path models = ModelsDir();
    if (!std::filesystem::is_directory(models, ec)) return out;
    std::vector<std::filesystem::path> mods;
    for (const auto& entry : std::filesystem::directory_iterator(models, ec)) {
        if (entry.is_directory(ec) && entry.path().filename().string().rfind('.', 0) != 0)
            mods.push_back(entry.path());
    }
    std::sort(mods.begin(), mods.end());
    for (size_t i = 0; i < mods.size(); ++i) {
        const std::filesystem::path file = mods[i] / name;
        if (std::filesystem::is_regular_file(file, ec))
            out.push_back({int(i), mods[i].filename().string(), file});
    }
    return out;
}
