#include "portal.h"

#include <dbus/dbus.h>

#include <cstdio>
#include <cstdlib>
#include <random>

namespace fv {

namespace {

constexpr const char* kService = "org.freedesktop.portal.Desktop";
constexpr const char* kPath = "/org/freedesktop/portal/desktop";
constexpr const char* kChooser = "org.freedesktop.portal.FileChooser";
constexpr const char* kRequest = "org.freedesktop.portal.Request";

// A private connection: the portal's Response signal is read on it by the
// calling thread only, so nothing else in the program sees or steals it.
DBusConnection* connect(std::string& err) {
    DBusError e;
    dbus_error_init(&e);
    DBusConnection* c = dbus_bus_get_private(DBUS_BUS_SESSION, &e);
    if (!c) {
        err = e.message ? e.message : "no session bus";
        dbus_error_free(&e);
        return nullptr;
    }
    dbus_connection_set_exit_on_disconnect(c, FALSE);
    return c;
}

void disconnect(DBusConnection* c) {
    dbus_connection_close(c);
    dbus_connection_unref(c);
}

// a{sv} entries
void addString(DBusMessageIter* dict, const char* key, const std::string& value) {
    DBusMessageIter entry, var;
    const char* v = value.c_str();
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &var);
    dbus_message_iter_append_basic(&var, DBUS_TYPE_STRING, &v);
    dbus_message_iter_close_container(&entry, &var);
    dbus_message_iter_close_container(dict, &entry);
}

void addBool(DBusMessageIter* dict, const char* key, bool value) {
    DBusMessageIter entry, var;
    dbus_bool_t v = value ? TRUE : FALSE;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "b", &var);
    dbus_message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN, &v);
    dbus_message_iter_close_container(&entry, &var);
    dbus_message_iter_close_container(dict, &entry);
}

// A path as the portal wants it: a byte array with the terminating zero.
void addBytes(DBusMessageIter* dict, const char* key, const std::string& value) {
    DBusMessageIter entry, var, arr;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "ay", &var);
    dbus_message_iter_open_container(&var, DBUS_TYPE_ARRAY, "y", &arr);
    const unsigned char* p = reinterpret_cast<const unsigned char*>(value.c_str());
    dbus_message_iter_append_fixed_array(&arr, DBUS_TYPE_BYTE, &p, int(value.size() + 1));
    dbus_message_iter_close_container(&var, &arr);
    dbus_message_iter_close_container(&entry, &var);
    dbus_message_iter_close_container(dict, &entry);
}

// filters: a(sa(us)) with one named filter of one glob pattern, plus "All files".
void addFilters(DBusMessageIter* dict, const std::string& name, const std::string& glob) {
    DBusMessageIter entry, var, arr;
    const char* key = "filters";
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "a(sa(us))", &var);
    dbus_message_iter_open_container(&var, DBUS_TYPE_ARRAY, "(sa(us))", &arr);
    auto one = [&](const std::string& n, const std::string& g) {
        DBusMessageIter st, pats, pat;
        const char* nm = n.c_str();
        const char* gl = g.c_str();
        dbus_uint32_t kind = 0;  // 0 = glob pattern
        dbus_message_iter_open_container(&arr, DBUS_TYPE_STRUCT, nullptr, &st);
        dbus_message_iter_append_basic(&st, DBUS_TYPE_STRING, &nm);
        dbus_message_iter_open_container(&st, DBUS_TYPE_ARRAY, "(us)", &pats);
        dbus_message_iter_open_container(&pats, DBUS_TYPE_STRUCT, nullptr, &pat);
        dbus_message_iter_append_basic(&pat, DBUS_TYPE_UINT32, &kind);
        dbus_message_iter_append_basic(&pat, DBUS_TYPE_STRING, &gl);
        dbus_message_iter_close_container(&pats, &pat);
        dbus_message_iter_close_container(&st, &pats);
        dbus_message_iter_close_container(&arr, &st);
    };
    one(name, glob);
    one("All files", "*");
    dbus_message_iter_close_container(&var, &arr);
    dbus_message_iter_close_container(&entry, &var);
    dbus_message_iter_close_container(dict, &entry);
}

// file:///a%20b -> /a b
std::string uriToPath(const std::string& uri) {
    std::string s = uri.rfind("file://", 0) == 0 ? uri.substr(7) : uri;
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            out += char(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

// The first entry of "uris" in the Response's results (a{sv}).
std::string firstUri(DBusMessageIter* results) {
    DBusMessageIter dict;
    dbus_message_iter_recurse(results, &dict);
    while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry, var, arr;
        dbus_message_iter_recurse(&dict, &entry);
        const char* key = nullptr;
        dbus_message_iter_get_basic(&entry, &key);
        dbus_message_iter_next(&entry);
        if (key && std::string(key) == "uris") {
            dbus_message_iter_recurse(&entry, &var);
            if (dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_ARRAY) {
                dbus_message_iter_recurse(&var, &arr);
                if (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRING) {
                    const char* u = nullptr;
                    dbus_message_iter_get_basic(&arr, &u);
                    return u ? u : "";
                }
            }
        }
        dbus_message_iter_next(&dict);
    }
    return {};
}

}  // namespace

bool portalAvailable() {
    if (std::getenv("FRESHVISION_NO_PORTAL")) return false;
    std::string err;
    DBusConnection* c = connect(err);
    if (!c) return false;
    DBusError e;
    dbus_error_init(&e);
    const bool has = dbus_bus_name_has_owner(c, kService, &e);
    dbus_error_free(&e);
    disconnect(c);
    return has;
}

PortalResult portalChoose(const PortalRequest& r, std::string& path, std::string& err, const std::atomic<bool>& cancel) {
    path.clear();
    DBusConnection* c = connect(err);
    if (!c) return PortalResult::Unavailable;
    // The Request object path the portal will use, known before the call so
    // the Response signal can't arrive before we listen for it.
    std::string sender = dbus_bus_get_unique_name(c);
    if (!sender.empty() && sender[0] == ':') sender.erase(0, 1);
    for (char& ch : sender)
        if (ch == '.') ch = '_';
    std::random_device rd;
    const std::string token = "freshvision" + std::to_string(rd() % 1000000000u);
    const std::string requestPath = std::string(kPath) + "/request/" + sender + "/" + token;
    const std::string rule = "type='signal',interface='" + std::string(kRequest) + "',member='Response',path='" + requestPath + "'";
    DBusError e;
    dbus_error_init(&e);
    dbus_bus_add_match(c, rule.c_str(), &e);
    if (dbus_error_is_set(&e)) {
        err = e.message;
        dbus_error_free(&e);
        disconnect(c);
        return PortalResult::Unavailable;
    }

    DBusMessage* m = dbus_message_new_method_call(kService, kPath, kChooser, r.save ? "SaveFile" : "OpenFile");
    DBusMessageIter it, dict;
    dbus_message_iter_init_append(m, &it);
    const char* parent = "";
    const char* title = r.title.c_str();
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &parent);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &title);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &dict);
    addString(&dict, "handle_token", token);
    addBool(&dict, "modal", true);
    if (r.directory) addBool(&dict, "directory", true);
    if (!r.currentFolder.empty()) addBytes(&dict, "current_folder", r.currentFolder);
    if (r.save && !r.currentName.empty()) addString(&dict, "current_name", r.currentName);
    if (!r.directory && !r.filterGlob.empty()) addFilters(&dict, r.filterName.empty() ? r.filterGlob : r.filterName, r.filterGlob);
    dbus_message_iter_close_container(&it, &dict);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(c, m, 10000, &e);
    dbus_message_unref(m);
    if (!reply) {
        err = e.message ? e.message : "no reply from the file-chooser portal";
        dbus_error_free(&e);
        disconnect(c);
        return PortalResult::Unavailable;
    }
    const char* handle = nullptr;
    std::string actualPath = requestPath;
    if (dbus_message_get_args(reply, &e, DBUS_TYPE_OBJECT_PATH, &handle, DBUS_TYPE_INVALID) && handle && handle != requestPath) {
        // Older portals choose the path themselves: listen there too.
        actualPath = handle;
        const std::string rule2 = "type='signal',interface='" + std::string(kRequest) + "',member='Response',path='" + actualPath + "'";
        dbus_bus_add_match(c, rule2.c_str(), nullptr);
    }
    dbus_error_free(&e);
    dbus_message_unref(reply);

    PortalResult result = PortalResult::Failed;
    bool done = false;
    while (!done) {
        if (cancel) {
            DBusMessage* close = dbus_message_new_method_call(kService, actualPath.c_str(), kRequest, "Close");
            dbus_connection_send(c, close, nullptr);
            dbus_connection_flush(c);
            dbus_message_unref(close);
            result = PortalResult::Cancelled;
            break;
        }
        if (!dbus_connection_read_write(c, 200)) {
            err = "the session bus closed";
            break;
        }
        while (DBusMessage* msg = dbus_connection_pop_message(c)) {
            if (dbus_message_is_signal(msg, kRequest, "Response") && dbus_message_get_path(msg) &&
                actualPath == dbus_message_get_path(msg)) {
                DBusMessageIter a;
                dbus_uint32_t code = 2;
                if (dbus_message_iter_init(msg, &a) && dbus_message_iter_get_arg_type(&a) == DBUS_TYPE_UINT32) {
                    dbus_message_iter_get_basic(&a, &code);
                    dbus_message_iter_next(&a);
                }
                if (code == 0) {
                    path = uriToPath(firstUri(&a));
                    result = path.empty() ? PortalResult::Failed : PortalResult::Chosen;
                    if (path.empty()) err = "the portal returned no file";
                } else if (code == 1) {
                    result = PortalResult::Cancelled;
                } else {
                    err = "the file dialog failed";
                }
                done = true;
            }
            dbus_message_unref(msg);
        }
    }
    disconnect(c);
    return result;
}

}  // namespace fv
