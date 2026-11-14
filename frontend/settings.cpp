#include "frontend/settings.h"

#include "core/text.h"

#include <charconv>
#include <filesystem>
#include <fstream>

namespace editor {

namespace {

int toInt(const std::string &s, int def) {
    int v = def;
    const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    return ec == std::errc() && p == s.data() + s.size() ? v : def;
}

} // namespace

void Settings::load(const std::string &path) {
    std::ifstream in(hx::pathFromUtf8(path));
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "firmware") firmware = value;
        else if (key == "base") base = value;
        else if (key == "setlist") setlist = toInt(value, setlist);
        else if (key == "preset") preset = toInt(value, preset);
        else if (key == "page") page = value;
        else if (key == "rail") railOpen = toInt(value, railOpen ? 1 : 0) != 0;
        else if (key == "online") online = toInt(value, online ? 1 : 0) != 0;
        else if (key == "unit") unit = value;
        else if (key == "confirm_save") confirmSave = toInt(value, confirmSave ? 1 : 0) != 0;
        else if (key == "firmware_notice") firmwareNotice = toInt(value, firmwareNotice ? 1 : 0) != 0;
    }
}

// Written beside the target and renamed over it, so a crash mid-write never leaves half a file.
bool Settings::save(const std::string &path) const {
    const std::filesystem::path target = hx::pathFromUtf8(path);
    std::filesystem::path temp = target;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::trunc);
        if (!out) return false;
        out << "firmware=" << firmware << "\n" << "base=" << base << "\n" << "setlist=" << setlist << "\n" << "preset=" << preset << "\n"
            << "page=" << page << "\n" << "rail=" << (railOpen ? 1 : 0) << "\n" << "online=" << (online ? 1 : 0) << "\n"
            << "unit=" << unit << "\n" << "confirm_save=" << (confirmSave ? 1 : 0) << "\n"
            << "firmware_notice=" << (firmwareNotice ? 1 : 0) << "\n";
        if (!out.flush()) return false;
    }
    std::error_code ec;
    std::filesystem::rename(temp, target, ec);
    return !ec;
}

} // namespace editor
