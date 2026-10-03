// One Firefox process per open view: kiosk mode, its own profile, drawing on
// the display of the module that asked for it (through the connected
// Wayland socket that module passed over).
#pragma once

#include <sys/types.h>

#include <string>
#include <vector>

namespace ffmod {

struct LaunchSpec {
    std::string app;      // the unpacked Firefox
    std::string runtime;  // bundled libraries and fonts
    std::string profile;  // profile directory of this view
    std::string url;
    std::string lang;     // "ru", "en-US"
    int wayland_fd = -1;  // connected Wayland client socket of the app module
    // Where Firefox's other processes find the app's display by name (the
    // module relays their connections): <run_dir>/wayland-0.
    std::string run_dir;
    std::string gl;       // Facet's OpenGL package (Mesa), "" without the gpu permission
};

// Prepares the profile (preferences) and starts Firefox; -1 on failure.
pid_t launch_firefox(const LaunchSpec& spec);
// SIGTERM to the whole process group, SIGKILL after 3 s.
void stop_process(pid_t& pid);

// The environment Firefox runs with (exposed for tests).
std::vector<std::string> firefox_environment(const LaunchSpec& spec, int wayland_fd_number);
// The preferences written to the profile's user.js.
std::string firefox_prefs();
// http(s) URLs only, no control characters, at most 4 KB.
bool acceptable_url(const std::string& url);
// View ids: [a-z0-9_-], 1-32 characters.
bool acceptable_id(const std::string& id);

}  // namespace ffmod
