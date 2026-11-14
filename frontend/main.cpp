#include "frontend/app.h"
#include "luminoveau.h"

#if defined(HXED_HAS_CLI)
#include "cli/hxed_dump.h"

#include <cstdlib>
#endif
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

editor::App *g_app = nullptr;

#if defined(HXED_HAS_CLI)
// Command-line mode (cli/hxed_dump.h): `--command args` runs the command and exits without a window. Anything else
// (a file to open, no arguments) starts the editor. oshexed.exe is a console program, so the terminal that started
// it waits and gets the output in order.
bool runCommandLine(int argc, char *argv[], int &exitCode) {
    if (!isCliCommand(argc, argv)) return false;
    exitCode = runCli(argc, argv);
    return true;
}
#endif

#if defined(_WIN32)
// Started from Explorer or a shortcut, Windows gives a console program a console window of its own: the only process
// on it is us. Let it go so only the editor's window shows. Started from a terminal, the console is shared and stays
// (the editor's log goes there).
void dropOwnConsole() {
    DWORD ids[2];
    if (GetConsoleProcessList(ids, 2) == 1) FreeConsole();
}
#endif

} // namespace

Lumi::Result AppInit(void ** /*appstate*/, int argc, char *argv[]) {
#if defined(HXED_HAS_CLI)
    // Before anything opens a window. A non-zero code exits right here (the callbacks only know success / failure).
    if (int code = 0; runCommandLine(argc, argv, code)) {
        if (code != 0) std::exit(code);
        return Lumi::Result::Success;
    }
#endif
#if defined(_WIN32)
    dropOwnConsole();
#endif
    g_app = new editor::App();
    return g_app->init(argc, argv) ? Lumi::Result::Continue : Lumi::Result::Failure;
}

Lumi::Result AppIterate(void * /*appstate*/) {
    return g_app->iterate() ? Lumi::Result::Continue : Lumi::Result::Success;
}

Lumi::Result AppEvent(void * /*appstate*/, SDL_Event *event) {
    if (event && g_app) g_app->handleEvent(*event);
    return Lumi::Result::Continue;
}

void AppQuit(void * /*appstate*/, Lumi::Result /*result*/) {
    if (!g_app) return;  // command-line mode: no window was opened
    g_app->shutdown();
    delete g_app;
    g_app = nullptr;
    Window::Close();
}
