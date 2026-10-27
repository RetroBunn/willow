#include "toml.hpp"

#include <cctype>

#include "common.hpp"

namespace willow {

namespace {

std::string trim(const std::string &s)
{
    size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

bool bare(const std::string &key)
{
    if (key.empty()) return false;
    for (char c : key)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') return false;
    return true;
}

// Calls `f` for each character outside strings, with its index.
template <typename F> void unquoted(const std::string &s, F f)
{
    char quote = 0;
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (quote) {
            if (c == '\\' && quote == '"') i++;
            else if (c == quote) quote = 0;
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (!f(i, c)) {
            return;
        }
    }
}

std::string strip_comment(const std::string &line)
{
    size_t end = line.size();
    unquoted(line, [&](size_t i, char c) {
        if (c != '#') return true;
        end = i;
        return false;
    });
    return line.substr(0, end);
}

int open_brackets(const std::string &s)
{
    int depth = 0;
    unquoted(s, [&](size_t, char c) {
        depth += c == '[' ? 1 : c == ']' ? -1 : 0;
        return true;
    });
    return depth;
}

struct Parser {
    explicit Parser(const std::string &text) : s(text) {}
    const std::string &s;
    size_t i = 0;
    std::string error;

    void space()
    {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) i++;
    }
    bool fail(const std::string &what)
    {
        error = what;
        return false;
    }
    bool value(TomlValue &v);
};

bool Parser::value(TomlValue &v)
{
    space();
    if (i >= s.size()) return fail("a value is missing");
    char c = s[i];
    if (c == '"' || c == '\'') {
        v.kind = TomlValue::String;
        for (i++; i < s.size() && s[i] != c; i++) {
            if (s[i] == '\n') return fail("a string runs past the end of its line");
            if (c == '"' && s[i] == '\\' && i + 1 < s.size()) {
                char e = s[++i];
                if (e == 'n') v.text += '\n';
                else if (e == 't') v.text += '\t';
                else if (e == '"' || e == '\\') v.text += e;
                else return fail(format("\\%c is not an escape Willow reads", e));
            } else {
                v.text += s[i];
            }
        }
        if (i >= s.size()) return fail("a string is not closed");
        i++;
        return true;
    }
    if (c == '[') {
        v.kind = TomlValue::Array;
        for (i++;;) {
            space();
            if (i < s.size() && s[i] == ']') break;
            TomlValue item;
            if (!value(item)) return false;
            v.items.push_back(std::move(item));
            space();
            if (i < s.size() && s[i] == ',') i++;
            else if (i >= s.size() || s[i] != ']') return fail("expected , or ] in an array");
        }
        i++;
        return true;
    }
    size_t start = i;
    while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_' || s[i] == '+' || s[i] == '-')) i++;
    std::string word = s.substr(start, i - start);
    if (word == "true" || word == "false") {
        v.kind = TomlValue::Boolean;
        v.flag = word == "true";
        return true;
    }
    std::string digits;
    for (char d : word)
        if (d != '_') digits += d;
    size_t p = 0;
    bool negative = !digits.empty() && digits[0] == '-';
    if (!digits.empty() && (digits[0] == '-' || digits[0] == '+')) p++;
    int base = digits.compare(p, 2, "0x") == 0 ? 16 : 10;
    if (base == 16) p += 2;
    if (p >= digits.size()) return fail(format("'%s' is not a value Willow reads", word.c_str()));
    int64_t n = 0;
    for (; p < digits.size(); p++) {
        char d = char(std::tolower(static_cast<unsigned char>(digits[p])));
        int digit = d >= '0' && d <= '9' ? d - '0' : d >= 'a' && d <= 'f' ? d - 'a' + 10 : 99;
        if (digit >= base) return fail(format("'%s' is not a value Willow reads", word.c_str()));
        n = n * base + digit;
    }
    v.kind = TomlValue::Integer;
    v.number = negative ? -n : n;
    return true;
}

}  // namespace

std::optional<TomlDocument> parse_toml(const std::string &text, std::string *error)
{
    std::vector<std::string> lines(1);
    for (char c : text) {
        if (c == '\n') lines.emplace_back();
        else lines.back() += c;
    }
    TomlDocument doc;
    TomlTable *table = &doc.root;
    for (size_t n = 0; n < lines.size(); n++) {
        size_t first = n;
        auto fail = [&](const std::string &what) {
            if (error) *error = format("line %zu: %s", first + 1, what.c_str());
            return std::nullopt;
        };
        std::string line = trim(strip_comment(lines[n]));
        if (line.empty()) continue;
        if (line[0] == '[') {
            bool array = line.compare(0, 2, "[[") == 0;
            size_t open = array ? 2 : 1, close = line.find(array ? "]]" : "]");
            if (close == std::string::npos || close + open != line.size()) return fail("a table header is not closed");
            std::string name = trim(line.substr(open, close - open));
            if (!bare(name)) return fail(format("'%s' is not a table name Willow reads", name.c_str()));
            if (array) {
                doc.arrays[name].emplace_back();
                table = &doc.arrays[name].back();
            } else {
                if (doc.tables.count(name)) return fail(format("[%s] appears twice", name.c_str()));
                table = &doc.tables[name];
            }
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) return fail("expected key = value");
        std::string key = trim(line.substr(0, eq)), rest = line.substr(eq + 1);
        if (!bare(key)) return fail(format("'%s' is not a key Willow reads", key.c_str()));
        if (table->count(key)) return fail(format("%s appears twice", key.c_str()));
        while (open_brackets(rest) > 0 && n + 1 < lines.size()) rest += "\n" + strip_comment(lines[++n]);
        Parser p(rest);
        TomlValue v;
        if (!p.value(v)) return fail(p.error);
        p.space();
        if (p.i != rest.size()) return fail("unexpected text after the value");
        (*table)[key] = std::move(v);
    }
    return doc;
}

}  // namespace willow
