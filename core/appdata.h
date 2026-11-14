#pragma once

#include <string>

namespace hx {

// The app's data folder (settings.ini, unit_state.txt), the same one SDL's pref path gives the app for org / app
// "oshexed": %APPDATA%\oshexed\oshexed on Windows, ~/.local/share/oshexed/oshexed elsewhere. Created if missing.
// The first call copies the files of the old placeholder name's folder (helix-editor/helix-editor) into a new
// one, so settings and the unit state survive the rename. "" when there is no home / APPDATA.
std::string appDataDir();

} // namespace hx
