// SPDX-License-Identifier: GPL-3.0-or-later
// Small bridges to the desktop: a folder picker dialog and "open this folder
// in the file browser". Linux only for now: the dialogs go through the
// desktop's file-chooser portal (portal.h; also inside a Flatpak), with
// `zenity` as the fallback when no portal runs; folders open with `xdg-open`.
// Programs are started directly (no shell, so folder names are never
// re-interpreted), and every child is reaped so none is left as a zombie.
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <sys/types.h>
#include <thread>
#include <vector>

namespace fv {

// A folder picker running in the background, so the caller's loop (video,
// GUI) keeps going while the dialog is open.
class FolderPicker {
public:
    ~FolderPicker();
    // Opens the dialog at `initialFolder`. False (with `err`) if one is
    // already open or the dialog program could not be started.
    bool start(const std::string& initialFolder, std::string& err);
    // The same dialog for a file: `save` asks for a new name (with overwrite
    // confirmation), `filter` is e.g. "Matroska | *.mkv" (empty: all files).
    bool startFile(const std::string& initialPath, const std::string& title, const std::string& filter, bool save,
                   std::string& err);
    bool running() const;
    // True once after the dialog closed: `chosen` is the picked folder,
    // empty if cancelled; `err` is set if the dialog failed.
    bool poll(std::string& chosen, std::string& err);

private:
    bool startWith(const std::vector<std::string>& args, std::string& err);
    // Portal first (on the worker thread), the zenity `fallback` if no portal answers.
    bool startPortal(const struct PortalRequest& req, const std::vector<std::string>& fallback, std::string& err);
    std::atomic<bool> cancel_{false};
    void wait(pid_t pid, int readFd);

    mutable std::mutex mu_;
    std::thread thread_;
    pid_t pid_ = -1;
    bool running_ = false, done_ = false;
    std::string chosen_, err_;
};

// Opens `folder` in the desktop's default file browser (`xdg-open`); returns
// at once, the browser runs on its own.
bool openInFileBrowser(const std::string& folder, std::string& err);

// The user's Videos folder as the desktop defines it (XDG_VIDEOS_DIR in
// ~/.config/user-dirs.dirs, i.e. what `xdg-user-dir VIDEOS` prints), if it
// exists; otherwise ~/Videos, then the home folder, then the current one.
std::string defaultVideoFolder();

// Starts a program (argv[0] looked up in PATH unless it has a '/') with the
// given arguments and returns at once; the child is reaped in the background.
bool launchDetached(const std::vector<std::string>& args, std::string& err);

}  // namespace fv
