// A reader for the part of TOML that Willow's own files use (willow.toml, mod.toml):
// comments, bare keys, [tables] and [[arrays of tables]] one level deep, and values that
// are strings ("basic" or 'literal'), integers (decimal or 0x hex, with _ separators),
// booleans, or arrays of those, which may span lines.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace willow {

struct TomlValue {
    enum Kind { String, Integer, Boolean, Array } kind = String;
    std::string text;
    int64_t number = 0;
    bool flag = false;
    std::vector<TomlValue> items;
};

using TomlTable = std::map<std::string, TomlValue>;

struct TomlDocument {
    TomlTable root;                                        // keys before any [table]
    std::map<std::string, TomlTable> tables;               // [name]
    std::map<std::string, std::vector<TomlTable>> arrays;  // [[name]], in order
};

// Parses a document, or returns nothing and sets `error` to "line N: what went wrong".
std::optional<TomlDocument> parse_toml(const std::string &text, std::string *error);

}  // namespace willow
