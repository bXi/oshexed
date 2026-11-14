#include "core/appdata.h"

#include <cstdlib>
#include <filesystem>

namespace hx {

namespace {

std::filesystem::path base() {
#if defined(_WIN32)
    if (const char* appdata = std::getenv("APPDATA")) return std::filesystem::path(appdata);
#else
    if (const char* home = std::getenv("HOME")) return std::filesystem::path(home) / ".local" / "share";
#endif
    return {};
}

} // namespace

std::string appDataDir() {
    const std::filesystem::path root = base();
    if (root.empty()) return {};
    const std::filesystem::path dir = root / "oshexed" / "oshexed";
    const std::filesystem::path old = root / "helix-editor" / "helix-editor";
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) {
        std::filesystem::create_directories(dir, ec);
        // Files only (settings.ini, unit_state.txt); the old folder stays as it was.
        if (std::filesystem::is_directory(old, ec))
            for (const auto& e : std::filesystem::directory_iterator(old, ec))
                if (e.is_regular_file(ec))
                    std::filesystem::copy_file(e.path(), dir / e.path().filename(),
                                               std::filesystem::copy_options::skip_existing, ec);
    }
    return dir.string();
}

} // namespace hx
