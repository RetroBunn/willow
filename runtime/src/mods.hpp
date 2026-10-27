// Drop-in mods (docs/mods.md): folders in mods/ beside the extracted disks, each with a
// mod.toml and files that replace the game's. The player's settings list the enabled ones
// in load order; a later mod's files win over an earlier one's.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace willow {

struct Mod {
    std::string id;              // its folder name
    std::filesystem::path dir;   // mods/<id>
    std::string name, version, description;
    std::vector<std::string> authors;
};

// Reads the enabled mods' manifests, in load order. `programs` are the file names of the
// recompiled programs, which a mod's files can't replace. Each problem goes into `problems`
// as one message naming the mod; a mod that can't be used (no folder, no readable mod.toml)
// is left out, and lesser problems leave it in.
std::vector<Mod> load_mods(const std::filesystem::path &mods_dir, const std::vector<std::string> &enabled,
                           const std::vector<std::string> &programs, std::vector<std::string> &problems);

}  // namespace willow
