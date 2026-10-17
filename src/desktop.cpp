// SPDX-License-Identifier: GPL-3.0-or-later
#include "desktop.h"
#include "portal.h"

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace fv {

namespace {

// Starts argv[0] from PATH with the given arguments and no shell. stdin and
// stderr go to /dev/null (zenity prints GTK warnings there); stdout goes to
// `stdoutFd` if >= 0, else to /dev/null.
bool spawn(const std::vector<std::string>& args, int stdoutFd, pid_t& pid, std::string& err) {
    std::vector<char*> argv;
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    if (stdoutFd >= 0) posix_spawn_file_actions_adddup2(&fa, stdoutFd, STDOUT_FILENO);
    else posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    int rc = posix_spawnp(&pid, argv[0], &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) {
        err = "could not start " + args[0] + ": " + std::strerror(rc);
        return false;
    }
    return true;
}

}  // namespace

FolderPicker::~FolderPicker() {
    pid_t pid;
    {
        std::lock_guard<std::mutex> lk(mu_);
        pid = running_ ? pid_ : -1;
    }
    cancel_ = true;                   // app is closing: close a portal dialog
    if (pid > 0) kill(pid, SIGTERM);  // ... or the zenity one
    if (thread_.joinable()) thread_.join();
}

bool FolderPicker::start(const std::string& initialFolder, std::string& err) {
    std::string start = initialFolder.empty() ? "" : initialFolder + "/";
    std::vector<std::string> args = {"zenity", "--file-selection", "--directory", "--title=Recording folder"};
    if (!start.empty()) args.push_back("--filename=" + start);
    PortalRequest req;
    req.title = "Recording folder";
    req.directory = true;
    req.currentFolder = initialFolder;
    return startPortal(req, args, err);
}

bool FolderPicker::startFile(const std::string& initialPath, const std::string& title, const std::string& filter,
                             bool save, std::string& err) {
    std::vector<std::string> args = {"zenity", "--file-selection", "--title=" + title};
    if (save) args.push_back("--save"), args.push_back("--confirm-overwrite");
    if (!filter.empty()) args.push_back("--file-filter=" + filter), args.push_back("--file-filter=All files | *");
    if (!initialPath.empty()) args.push_back("--filename=" + initialPath);
    PortalRequest req;
    req.title = title;
    req.save = save;
    if (!initialPath.empty()) {
        std::error_code ec;
        const std::filesystem::path ip(initialPath);
        if (std::filesystem::is_directory(ip, ec)) req.currentFolder = initialPath;
        else req.currentFolder = ip.parent_path().string(), req.currentName = save ? ip.filename().string() : "";
    }
    if (!filter.empty()) {  // "Matroska | *.mkv"
        const size_t bar = filter.find('|');
        auto trim = [](std::string t) {
            while (!t.empty() && t.front() == ' ') t.erase(0, 1);
            while (!t.empty() && t.back() == ' ') t.pop_back();
            return t;
        };
        req.filterName = trim(bar == std::string::npos ? filter : filter.substr(0, bar));
        req.filterGlob = trim(bar == std::string::npos ? filter : filter.substr(bar + 1));
    }
    return startPortal(req, args, err);
}

bool FolderPicker::startPortal(const PortalRequest& req, const std::vector<std::string>& fallback, std::string& err) {
    if (!portalAvailable()) return startWith(fallback, err);
    std::lock_guard<std::mutex> lk(mu_);
    if (running_) {
        err = "the dialog is already open";
        return false;
    }
    if (thread_.joinable()) thread_.join();  // previous dialog finished
    pid_ = -1;
    running_ = true;
    done_ = false;
    cancel_ = false;
    chosen_.clear();
    err_.clear();
    thread_ = std::thread([this, req, fallback] {
        std::string path, e;
        const PortalResult r = portalChoose(req, path, e, cancel_);
        if (r == PortalResult::Unavailable && !cancel_) {  // the portal didn't answer: zenity
            int fds[2];
            pid_t pid = -1;
            std::string se;
            if (pipe2(fds, O_CLOEXEC) != 0 || !spawn(fallback, fds[1], pid, se)) {
                std::lock_guard<std::mutex> l(mu_);
                err_ = se.empty() ? "the dialog could not be opened" : se;
                running_ = false;
                done_ = true;
                return;
            }
            close(fds[1]);
            {
                std::lock_guard<std::mutex> l(mu_);
                pid_ = pid;
            }
            wait(pid, fds[0]);
            return;
        }
        std::lock_guard<std::mutex> l(mu_);
        if (r == PortalResult::Chosen) chosen_ = path;
        else if (r == PortalResult::Failed) err_ = e;
        running_ = false;
        done_ = true;
    });
    return true;
}

bool FolderPicker::startWith(const std::vector<std::string>& args, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    if (running_) {
        err = "the dialog is already open";
        return false;
    }
    if (thread_.joinable()) thread_.join();  // previous dialog finished
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) {
        err = std::string("pipe: ") + std::strerror(errno);
        return false;
    }
    // A trailing '/' makes zenity open *inside* the folder.
    pid_t pid = -1;
    if (!spawn(args, fds[1], pid, err)) {
        close(fds[0]);
        close(fds[1]);
        return false;
    }
    close(fds[1]);
    pid_ = pid;
    running_ = true;
    done_ = false;
    chosen_.clear();
    err_.clear();
    thread_ = std::thread(&FolderPicker::wait, this, pid, fds[0]);
    return true;
}

void FolderPicker::wait(pid_t pid, int readFd) {
    std::string out;
    char buf[512];
    ssize_t n;
    while ((n = read(readFd, buf, sizeof buf)) > 0 || (n < 0 && errno == EINTR))
        if (n > 0) out.append(buf, size_t(n));
    close(readFd);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    std::lock_guard<std::mutex> lk(mu_);
    // zenity: exit 0 = chosen, 1 = cancelled; anything else is a failure.
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) chosen_ = out;
    else if (!(WIFEXITED(status) && WEXITSTATUS(status) == 1)) err_ = "the dialog failed";
    running_ = false;
    done_ = true;
}

bool FolderPicker::running() const {
    std::lock_guard<std::mutex> lk(mu_);
    return running_;
}

bool FolderPicker::poll(std::string& chosen, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!done_) return false;
    done_ = false;
    chosen = chosen_;
    err = err_;
    return true;
}

bool openInFileBrowser(const std::string& folder, std::string& err) {
    pid_t pid = -1;
    if (!spawn({"xdg-open", folder}, -1, pid, err)) return false;
    // xdg-open exits once it has handed the folder to the file browser;
    // reap it in the background so it doesn't linger as a zombie.
    std::thread([pid] {
        int status;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
    }).detach();
    return true;
}

std::string defaultVideoFolder() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const char* homeEnv = std::getenv("HOME");
    const std::string home = homeEnv ? homeEnv : "";
    const char* cfg = std::getenv("XDG_CONFIG_HOME");
    const std::string dirsFile = (cfg && *cfg ? std::string(cfg) : home + "/.config") + "/user-dirs.dirs";
    std::ifstream in(dirsFile);
    std::string line;
    while (std::getline(in, line)) {
        const std::string key = "XDG_VIDEOS_DIR=";
        if (line.rfind(key, 0) != 0) continue;
        std::string v = line.substr(key.size());
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
        if (v.rfind("$HOME", 0) == 0) v = home + v.substr(5);
        if (!v.empty() && fs::is_directory(v, ec)) return v;
    }
    if (!home.empty() && fs::is_directory(home + "/Videos", ec)) return home + "/Videos";
    if (!home.empty()) return home;
    return fs::current_path(ec).string();
}

bool launchDetached(const std::vector<std::string>& args, std::string& err) {
    if (args.empty()) {
        err = "nothing to start";
        return false;
    }
    pid_t pid = -1;
    if (!spawn(args, -1, pid, err)) return false;
    std::thread([pid] {
        int status;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
    }).detach();
    return true;
}

}  // namespace fv
