// SPDX-License-Identifier: GPL-3.0-or-later
#include "controls.h"

#include <linux/videodev2.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "v4l2_util.h"

namespace fv {

namespace {

const char* typeName(uint32_t t) {
    switch (t) {
        case V4L2_CTRL_TYPE_INTEGER: return "int";
        case V4L2_CTRL_TYPE_BOOLEAN: return "bool";
        case V4L2_CTRL_TYPE_MENU: return "menu";
        case V4L2_CTRL_TYPE_BUTTON: return "button";
        case V4L2_CTRL_TYPE_INTEGER64: return "int64";
        case V4L2_CTRL_TYPE_CTRL_CLASS: return "class";
        case V4L2_CTRL_TYPE_STRING: return "string";
        case V4L2_CTRL_TYPE_BITMASK: return "bitmask";
        case V4L2_CTRL_TYPE_INTEGER_MENU: return "intmenu";
        default: return "other";
    }
}

bool simpleValueType(uint32_t t) {
    return t == V4L2_CTRL_TYPE_INTEGER || t == V4L2_CTRL_TYPE_BOOLEAN || t == V4L2_CTRL_TYPE_MENU ||
           t == V4L2_CTRL_TYPE_INTEGER_MENU || t == V4L2_CTRL_TYPE_BITMASK;
}

bool readValue(int fd, const ControlInfo& c, int64_t& out) {
    if (c.type == V4L2_CTRL_TYPE_INTEGER64) {
        v4l2_ext_control e{};
        e.id = c.id;
        v4l2_ext_controls es{};
        es.which = V4L2_CTRL_WHICH_CUR_VAL;
        es.count = 1;
        es.controls = &e;
        if (xioctl(fd, VIDIOC_G_EXT_CTRLS, &es) < 0) return false;
        out = e.value64;
        return true;
    }
    if (!simpleValueType(c.type)) return false;
    v4l2_control v{};
    v.id = c.id;
    if (xioctl(fd, VIDIOC_G_CTRL, &v) < 0) return false;
    out = v.value;
    return true;
}

// Lower case, non-alphanumerics collapsed to single spaces.
std::string normalize(const std::string& s) {
    std::string out;
    bool space = true;
    for (unsigned char ch : s) {
        if (std::isalnum(ch)) {
            out += char(std::tolower(ch));
            space = false;
        } else if (!space) {
            out += ' ';
            space = true;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

bool parseInt(const std::string& s, int64_t& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    errno = 0;
    long long v = std::strtoll(s.c_str(), &end, 0);
    if (errno != 0 || *end != '\0') return false;
    out = v;
    return true;
}

}  // namespace

namespace {

// Fills a ControlInfo from a QUERYCTRL result (menus and the current value included).
ControlInfo describe(int fd, const v4l2_queryctrl& q) {
    ControlInfo c;
    c.id = q.id;
    c.name = reinterpret_cast<const char*>(q.name);
    c.type = q.type;
    c.min = q.minimum;
    c.max = q.maximum;
    c.step = q.step;
    c.def = q.default_value;
    c.flags = q.flags;

    if (q.type == V4L2_CTRL_TYPE_MENU || q.type == V4L2_CTRL_TYPE_INTEGER_MENU) {
        for (int64_t i = q.minimum; i <= q.maximum; ++i) {
            v4l2_querymenu m{};
            m.id = q.id;
            m.index = uint32_t(i);
            if (xioctl(fd, VIDIOC_QUERYMENU, &m) < 0) continue;
            if (q.type == V4L2_CTRL_TYPE_MENU)
                c.menu.emplace_back(i, reinterpret_cast<const char*>(m.name));
            else
                c.menu.emplace_back(i, std::to_string(static_cast<long long>(m.value)));
        }
    }
    // Inactive controls refuse to be read (EINVAL); that is not an error.
    if (!(q.flags & V4L2_CTRL_FLAG_WRITE_ONLY)) c.hasValue = readValue(fd, c, c.value);
    return c;
}

}  // namespace

// Two passes. V4L2_CTRL_FLAG_NEXT_CTRL is the standard way, but the development
// driver answers it with nothing although it has 27 controls; asking for every
// ID in the kernel's user and private ranges (what VCS does) finds them. Names,
// ranges and menus always come from the driver at runtime.
std::vector<ControlInfo> enumerateControls(int fd) {
    std::vector<ControlInfo> out;
    auto have = [&](uint32_t id) {
        return std::any_of(out.begin(), out.end(), [&](const ControlInfo& c) { return c.id == id; });
    };

    v4l2_queryctrl q{};
    q.id = V4L2_CTRL_FLAG_NEXT_CTRL;
    while (xioctl(fd, VIDIOC_QUERYCTRL, &q) == 0) {
        out.push_back(describe(fd, q));
        q.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
    }

    for (uint32_t base : {uint32_t(V4L2_CID_BASE), uint32_t(V4L2_CID_PRIVATE_BASE)}) {
        for (uint32_t id = base; id < base + 0x100; ++id) {
            if (have(id)) continue;
            v4l2_queryctrl e{};
            e.id = id;
            if (xioctl(fd, VIDIOC_QUERYCTRL, &e) == 0 && e.type != V4L2_CTRL_TYPE_CTRL_CLASS)
                out.push_back(describe(fd, e));
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const ControlInfo& a, const ControlInfo& b) { return a.id < b.id; });
    return out;
}

void printControls(const std::vector<ControlInfo>& controls) {
    std::printf("controls (%zu):\n", controls.size());
    for (const auto& c : controls) {
        if (c.type == V4L2_CTRL_TYPE_CTRL_CLASS) {
            std::printf("  [class] %s\n", c.name.c_str());
            continue;
        }
        std::printf("  0x%08x  %-32s %-7s", c.id, c.name.c_str(), typeName(c.type));
        if (c.type == V4L2_CTRL_TYPE_BUTTON) {
            std::printf(" (button)");
        } else if (c.type != V4L2_CTRL_TYPE_STRING) {
            std::printf(" min %lld max %lld step %lld def %lld", (long long)c.min, (long long)c.max,
                        (long long)c.step, (long long)c.def);
        }
        if (c.hasValue) std::printf(" value %lld", (long long)c.value);
        if (c.flags & V4L2_CTRL_FLAG_DISABLED) std::printf(" [disabled]");
        if (c.flags & V4L2_CTRL_FLAG_READ_ONLY) std::printf(" [read-only]");
        if (c.flags & V4L2_CTRL_FLAG_INACTIVE) std::printf(" [inactive]");
        if (c.flags & V4L2_CTRL_FLAG_GRABBED) std::printf(" [grabbed]");
        std::printf("\n");
        for (const auto& m : c.menu) std::printf("      %lld = %s\n", (long long)m.first, m.second.c_str());
    }
    std::printf("\n");
}

bool controlIsDetectedOnly(const ControlInfo& c) { return c.name == "Signal Type"; }

bool controlIsSettable(const ControlInfo& c) {
    return !(c.flags & (V4L2_CTRL_FLAG_DISABLED | V4L2_CTRL_FLAG_READ_ONLY | V4L2_CTRL_FLAG_GRABBED)) &&
           !controlIsDetectedOnly(c);
}

bool setControlByName(int fd, const std::vector<ControlInfo>& controls, const std::string& name,
                      const std::string& value, std::string& msg) {
    const std::string want = normalize(name);
    if (want.empty()) {
        msg = "empty control name";
        return false;
    }

    const ControlInfo* found = nullptr;
    std::vector<const ControlInfo*> partial;
    for (const auto& c : controls) {
        if (c.type == V4L2_CTRL_TYPE_CTRL_CLASS) continue;
        std::string n = normalize(c.name);
        if (n == want) {
            found = &c;
            break;
        }
        if (n.find(want) != std::string::npos) partial.push_back(&c);
    }
    if (!found) {
        if (partial.size() == 1) {
            found = partial[0];
        } else if (partial.empty()) {
            msg = "no control matches '" + name + "' (use --info to list the controls)";
            return false;
        } else {
            msg = "'" + name + "' is ambiguous:";
            for (auto* c : partial) msg += " '" + c->name + "'";
            return false;
        }
    }
    const ControlInfo& c = *found;

    if (!controlIsSettable(c)) {
        msg = "control '" + c.name + "' is disabled, read-only or grabbed" +
              ((c.flags & V4L2_CTRL_FLAG_INACTIVE) ? " (inactive for the current signal, e.g. an analog-only control on a digital input)" : "");
        return false;
    }

    int64_t v = 0;
    if (c.type == V4L2_CTRL_TYPE_BUTTON) {
        v = 1;
    } else if (c.type == V4L2_CTRL_TYPE_BOOLEAN) {
        std::string s = normalize(value);
        if (s == "1" || s == "on" || s == "true" || s == "yes") v = 1;
        else if (s == "0" || s == "off" || s == "false" || s == "no") v = 0;
        else {
            msg = "'" + value + "' is not a boolean (0/1/on/off)";
            return false;
        }
    } else if (c.type == V4L2_CTRL_TYPE_MENU && !parseInt(value, v)) {
        const std::string wantItem = normalize(value);
        const std::pair<int64_t, std::string>* hit = nullptr;
        int partialHits = 0;
        for (const auto& m : c.menu) {
            std::string n = normalize(m.second);
            if (n == wantItem) {
                hit = &m;
                partialHits = 1;
                break;
            }
            if (n.find(wantItem) != std::string::npos) {
                hit = &m;
                ++partialHits;
            }
        }
        if (!hit || partialHits != 1) {
            msg = "'" + value + "' does not match exactly one menu item of '" + c.name + "'";
            return false;
        }
        v = hit->first;
    } else if (!parseInt(value, v)) {
        msg = "'" + value + "' is not a number";
        return false;
    }

    if (c.type != V4L2_CTRL_TYPE_BUTTON && c.type != V4L2_CTRL_TYPE_BITMASK && (v < c.min || v > c.max)) {
        msg = "value " + std::to_string(static_cast<long long>(v)) + " outside " + std::to_string(static_cast<long long>(c.min)) +
              ".." + std::to_string(static_cast<long long>(c.max)) + " for '" + c.name + "'";
        return false;
    }

    int rc;
    if (c.type == V4L2_CTRL_TYPE_INTEGER64) {
        v4l2_ext_control e{};
        e.id = c.id;
        e.value64 = v;
        v4l2_ext_controls es{};
        es.which = V4L2_CTRL_WHICH_CUR_VAL;
        es.count = 1;
        es.controls = &e;
        rc = xioctl(fd, VIDIOC_S_EXT_CTRLS, &es);
    } else {
        v4l2_control ctl{};
        ctl.id = c.id;
        ctl.value = int32_t(v);
        rc = xioctl(fd, VIDIOC_S_CTRL, &ctl);
    }
    if (rc < 0) {
        msg = std::string("setting '") + c.name + "' failed: " + std::strerror(errno);
        return false;
    }

    ControlInfo after = c;
    int64_t now = 0;
    bool ok = readValue(fd, after, now);
    msg = "'" + c.name + "': " + (c.hasValue ? std::to_string(static_cast<long long>(c.value)) : std::string("?")) + " -> " +
          (ok ? std::to_string(static_cast<long long>(now)) : std::string("(not readable)"));
    return true;
}

}  // namespace fv
