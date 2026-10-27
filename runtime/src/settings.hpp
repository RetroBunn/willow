// The player's settings, from willow.toml next to the extracted disks (docs/mods.md).
// Missing keys keep their defaults; unknown keys and bad values are reported and ignored,
// so a typo doesn't pass silently.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "filesystem.hpp"

namespace willow {

struct Settings {
    uint32_t disk_speedup = Floppy::kSpeedup;  // loading: "real" 1, "fast" 3, "instant" 0 (no disk time)
    bool sprite_limit = true;                  // the hardware's 32 sprites a line
    bool fullscreen = false;
    bool smooth = false;                       // smooth scaling instead of sharp pixels
    std::vector<std::string> mods;             // enabled mods, by folder name, in load order
};

// Each problem goes into `problems` as one message.
Settings parse_settings(const std::string &text, std::vector<std::string> &problems);
// Reads a settings file; with `create`, first writes kDefaultSettings if there is none.
Settings read_settings(const std::filesystem::path &path, bool create, std::vector<std::string> &problems);

extern const char *const kDefaultSettings;  // a commented file with the defaults

}  // namespace willow
