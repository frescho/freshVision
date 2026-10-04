// The desktop's file-chooser portal (org.freedesktop.portal.FileChooser over
// D-Bus): folder and file dialogs that work the same in a normal install and
// inside a Flatpak sandbox, where `zenity` doesn't exist (release 0.1.0,
// step A7). desktop.cpp falls back to zenity when no portal is running.
#pragma once

#include <atomic>
#include <string>

namespace fv {

struct PortalRequest {
    std::string title;
    bool directory = false;   // choose a folder (OpenFile with directory=true)
    bool save = false;        // SaveFile: a new name, the desktop asks before overwriting
    std::string currentFolder;
    std::string currentName;  // SaveFile: the proposed file name
    std::string filterName;   // e.g. "Matroska"; empty: no filter
    std::string filterGlob;   // e.g. "*.mkv"
};

enum class PortalResult { Chosen, Cancelled, Unavailable, Failed };

// True if a portal service is running on the session bus.
bool portalAvailable();

// Shows the dialog and blocks until it closes (run it on a worker thread).
// `cancel` set to true closes the dialog early (Cancelled). Unavailable: the
// portal couldn't be asked (the caller falls back); Failed: it answered with
// an error (`err`).
PortalResult portalChoose(const PortalRequest& r, std::string& path, std::string& err, const std::atomic<bool>& cancel);

}  // namespace fv
