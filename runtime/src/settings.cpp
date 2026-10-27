#include "settings.hpp"

#include <fstream>
#include <sstream>

#include "common.hpp"
#include "toml.hpp"

namespace willow {

const char *const kDefaultSettings =
    "# Willow's settings. Edit this file and restart Willow; delete it to get the defaults back.\n"
    "\n"
    "[options]\n"
    "# How long loading takes: \"real\" (the X68000's floppy drives), \"fast\" (3x) or \"instant\".\n"
    "loading = \"fast\"\n"
    "# The X68000 shows at most 32 sprites on a line, so parts of sprites drop out in busy\n"
    "# scenes. false shows every sprite.\n"
    "sprite_limit = true\n"
    "fullscreen = false\n"
    "# Smooth scaling instead of sharp pixels.\n"
    "smooth = false\n"
    "\n"
    "[mods]\n"
    "# The mods to use, by their folder names in mods/, in load order: a later mod wins.\n"
    "enabled = []\n";

Settings parse_settings(const std::string &text, std::vector<std::string> &problems)
{
    Settings s;
    std::string error;
    auto doc = parse_toml(text, &error);
    if (!doc) {
        problems.push_back(error);
        return s;
    }
    for (const auto &kv : doc->root) problems.push_back(format("%s belongs under [options] or [mods]", kv.first.c_str()));
    for (const auto &kv : doc->arrays) problems.push_back(format("[[%s]] is not a settings section", kv.first.c_str()));
    for (const auto &kv : doc->tables)
        if (kv.first != "options" && kv.first != "mods") problems.push_back(format("[%s] is not a settings section", kv.first.c_str()));
    auto is = [&](const std::string &key, const TomlValue &v, TomlValue::Kind kind, const char *what) {
        if (v.kind != kind) problems.push_back(format("%s should be %s", key.c_str(), what));
        return v.kind == kind;
    };

    for (const auto &[key, v] : doc->tables["options"]) {
        std::string name = "options." + key;
        if (key == "loading") {
            if (!is(name, v, TomlValue::String, "\"real\", \"fast\" or \"instant\"")) continue;
            if (v.text == "real") s.disk_speedup = 1;
            else if (v.text == "fast") s.disk_speedup = Floppy::kSpeedup;
            else if (v.text == "instant") s.disk_speedup = 0;
            else problems.push_back(format("%s should be \"real\", \"fast\" or \"instant\", not \"%s\"", name.c_str(), v.text.c_str()));
        } else if (key == "sprite_limit" || key == "fullscreen" || key == "smooth") {
            if (!is(name, v, TomlValue::Boolean, "true or false")) continue;
            (key == "sprite_limit" ? s.sprite_limit : key == "fullscreen" ? s.fullscreen : s.smooth) = v.flag;
        } else {
            problems.push_back(format("%s is not a setting", name.c_str()));
        }
    }
    for (const auto &[key, v] : doc->tables["mods"]) {
        if (key != "enabled") {
            problems.push_back(format("mods.%s is not a setting", key.c_str()));
        } else if (is("mods.enabled", v, TomlValue::Array, "a list of mod folder names")) {
            for (const TomlValue &item : v.items)
                if (is("each of mods.enabled", item, TomlValue::String, "a folder name in quotes")) s.mods.push_back(item.text);
        }
    }
    return s;
}

Settings read_settings(const std::filesystem::path &path, bool create, std::vector<std::string> &problems)
{
    std::error_code ec;
    if (create && !std::filesystem::exists(path, ec)) {
        std::ofstream(path, std::ios::binary) << kDefaultSettings;
        log("wrote the default settings to %s", path.string().c_str());
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) return Settings{};
    std::stringstream text;
    text << in.rdbuf();
    return parse_settings(text.str(), problems);
}

}  // namespace willow
