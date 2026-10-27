#include "mods.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>

#include "common.hpp"
#include "toml.hpp"

namespace fs = std::filesystem;

namespace willow {

static std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

std::vector<Mod> load_mods(const fs::path &mods_dir, const std::vector<std::string> &enabled,
                           const std::vector<std::string> &programs, std::vector<std::string> &problems)
{
    std::vector<Mod> mods;
    std::set<std::string> seen;
    for (const std::string &id : enabled) {
        if (id.empty() || id == "." || id == ".." || id.find_first_of("/\\:") != std::string::npos) {
            problems.push_back(format("mods.enabled: \"%s\" is not a folder name", id.c_str()));
            continue;
        }
        if (!seen.insert(lower(id)).second) {
            problems.push_back(format("mods.enabled lists %s more than once; it loads where it first appears", id.c_str()));
            continue;
        }
        auto problem = [&](const std::string &what) { problems.push_back("mods/" + id + ": " + what); };
        Mod mod;
        mod.id = mod.name = id;
        mod.dir = mods_dir / id;
        std::error_code ec;
        if (!fs::is_directory(mod.dir, ec)) {
            problem("not found in " + mods_dir.string() + ", so it is not used");
            continue;
        }
        std::ifstream in(mod.dir / "mod.toml", std::ios::binary);
        if (!in) {
            problem("there is no mod.toml, so the mod is not used");
            continue;
        }
        std::stringstream text;
        text << in.rdbuf();
        std::string error;
        auto doc = parse_toml(text.str(), &error);
        if (!doc) {
            problem("mod.toml " + error + "; the mod is not used");
            continue;
        }

        for (const auto &[key, v] : doc->root) {
            auto text_of = [&](std::string &out) {
                if (v.kind == TomlValue::String) out = v.text;
                else problem(key + " should be text in quotes");
            };
            if (key == "name") {
                text_of(mod.name);
            } else if (key == "version") {
                text_of(mod.version);
            } else if (key == "description") {
                text_of(mod.description);
            } else if (key == "authors") {
                if (v.kind != TomlValue::Array) {
                    problem("authors should be a list of names in quotes");
                    continue;
                }
                for (const TomlValue &a : v.items) {
                    if (a.kind == TomlValue::String) mod.authors.push_back(a.text);
                    else problem("authors should be a list of names in quotes");
                }
            } else {
                problem(key + " is not a key Willow reads in mod.toml");
            }
        }
        if (!doc->root.count("name")) problem("mod.toml has no name");
        for (const auto &kv : doc->tables) problem("[" + kv.first + "] is not a section Willow reads in mod.toml");
        for (const auto &kv : doc->arrays) {
            if (kv.first == "patch") problem("Willow can't apply patches yet, so its files are used without them");
            else problem("[[" + kv.first + "]] is not a section Willow reads in mod.toml");
        }

        // files/ holds a folder for each disk. The programs come from the recompiled game,
        // and loading them reads only the real disk, so a mod's copy would do nothing.
        for (const auto &e : fs::directory_iterator(mod.dir / "files", ec)) {
            std::string disk = lower(e.path().filename().string());
            if (!e.is_directory(ec) || (disk != "system" && disk != "data")) {
                problem("files/" + e.path().filename().string() + " is not a disk: files go in files/system or files/data");
                continue;
            }
            if (disk != "system") continue;
            for (const auto &f : fs::directory_iterator(e.path(), ec)) {
                std::string name = f.path().filename().string();
                for (const std::string &program : programs)
                    if (lower(program) == lower(name))
                        problem("files/system/" + name + " would replace a program, which only patches can change; it is not used");
            }
        }

        std::string about = mod.name + (mod.version.empty() ? "" : " " + mod.version);
        for (size_t i = 0; i < mod.authors.size(); i++) about += (i ? ", " : " by ") + mod.authors[i];
        log("using mod %s: %s", id.c_str(), about.c_str());
        mods.push_back(std::move(mod));
    }
    return mods;
}

}  // namespace willow
