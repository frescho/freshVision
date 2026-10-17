// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform.h"

#include <cstdlib>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fv {

namespace fs = std::filesystem;

std::string programDir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(sizeof buf / sizeof buf[0]));
    if (n == 0) return {};
    return fs::path(std::wstring(buf, n)).parent_path().u8string();
#else
    std::error_code ec;
    const fs::path self = fs::read_symlink("/proc/self/exe", ec);
    return ec ? std::string() : self.parent_path().string();
#endif
}

std::string settingsBaseDir() {
#ifdef _WIN32
    std::error_code ec;
    const fs::path prog = fs::u8path(programDir());
    if (!prog.empty() && fs::exists(prog / "portable.txt", ec)) return (prog / "settings").u8string();
    if (const wchar_t* appdata = _wgetenv(L"APPDATA"); appdata && *appdata) return (fs::path(appdata) / "freshVision").u8string();
    return (prog / "settings").u8string();
#else
    const char* cfg = std::getenv("XDG_CONFIG_HOME");
    if (cfg && *cfg) return (fs::path(cfg) / "freshvision").string();
    const char* home = std::getenv("HOME");
    if (!home || !*home) return {};
    return (fs::path(home) / ".config" / "freshvision").string();
#endif
}

void hideOwnConsole() {
#ifdef _WIN32
    DWORD procs[2];
    if (GetConsoleProcessList(procs, 2) == 1) FreeConsole();  // only we use it: Explorer made it for us
#endif
}

}  // namespace fv
