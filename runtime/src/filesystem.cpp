#include "filesystem.hpp"

#include <algorithm>
#include <cctype>
#include <map>

#include "common.hpp"

namespace fs = std::filesystem;

namespace willow {

static constexpr int kFirstHandle = 5;
static constexpr uint16_t kDosDate = (13 << 9) | (8 << 5) | 27;  // 1993-08-27, the disks' own date

static std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

static bool match(const std::string &pattern, const std::string &name)
{
    // '*' and '?' wildcards; "*.*" also matches names without an extension.
    if (pattern == "*.*") return true;
    size_t p = 0, n = 0, star = std::string::npos, mark = 0;
    while (n < name.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == name[n])) {
            p++, n++;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++, mark = n;
        } else if (star != std::string::npos) {
            p = star + 1, n = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') p++;
    return p == pattern.size();
}

FileSystem::~FileSystem()
{
    for (Handle &h : handles_)
        if (h.file) std::fclose(h.file);
}

void FileSystem::mount(int drive, fs::path root) { drives_[drive] = std::move(root); }

int32_t FileSystem::chgdrv(int drive)
{
    if (drive >= 0 && drive < 26 && drives_[drive]) current_ = drive;
    return 26;
}

int32_t FileSystem::drvctrl(int mode, int drive)
{
    int d = drive == 0 ? current_ : drive - 1;
    if (mode != 0) log_once(format("drvctrl-%d", mode), "_DRVCTRL mode %d treated as a status query", mode);
    return d >= 0 && d < 26 && drives_[d] ? 0x02 : 0x04;  // media present and ready, or not ready
}

int32_t FileSystem::drvxchg(int a, int b)
{
    if (a < 1 || a > 26 || b < 1 || b > 26) return err::kBadDrive;
    std::swap(drives_[a - 1], drives_[b - 1]);
    std::swap(cwd_[a - 1], cwd_[b - 1]);
    return 0;
}

std::optional<FileSystem::Path> FileSystem::parse(const std::string &path) const
{
    Path p{current_, {}};
    std::string rest = path;
    if (rest.size() >= 2 && rest[1] == ':') {
        p.drive = std::tolower(static_cast<unsigned char>(rest[0])) - 'a';
        rest = rest.substr(2);
    } else if (!rest.empty() && rest[0] == ':') {
        rest = rest.substr(1);
    }
    if (p.drive < 0 || p.drive >= 26) return std::nullopt;
    if (rest.empty() || (rest[0] != '\\' && rest[0] != '/')) p.parts = cwd_[p.drive];
    std::string part;
    for (size_t i = 0; i <= rest.size(); i++) {
        if (i == rest.size() || rest[i] == '\\' || rest[i] == '/') {
            if (part == "..") {
                if (!p.parts.empty()) p.parts.pop_back();
            } else if (!part.empty() && part != ".") {
                p.parts.push_back(lower(part));
            }
            part.clear();
        } else {
            part += rest[i];
        }
    }
    return p;
}

// The host file or folder at `parts` (lowercased) under `root`, matching names case-insensitively.
static std::optional<fs::path> resolve(const fs::path &root, const std::vector<std::string> &parts)
{
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return std::nullopt;
    fs::path at = root;
    for (const std::string &part : parts) {
        fs::path found;
        for (const auto &entry : fs::directory_iterator(at, ec))
            if (lower(entry.path().filename().string()) == part) found = entry.path();
        if (found.empty()) return std::nullopt;
        at = found;
    }
    return at;
}

std::vector<fs::path> FileSystem::layers(int drive) const
{
    std::vector<fs::path> out;
    if (!drives_[drive]) return out;
    std::string disk = lower(drives_[drive]->filename().string());
    if (!overlay_.empty()) out.push_back(overlay_ / disk);
    for (auto mod = mods_.rbegin(); mod != mods_.rend(); ++mod)
        if (auto at = resolve(*mod, {disk})) out.push_back(*at);
    out.push_back(*drives_[drive]);
    return out;
}

std::optional<fs::path> FileSystem::find(const Path &p) const
{
    for (const fs::path &layer : layers(p.drive))
        if (auto at = resolve(layer, p.parts)) return at;
    return std::nullopt;
}

fs::path FileSystem::overlay(const Path &p) const
{
    fs::path at = overlay_ / (drives_[p.drive] ? lower(drives_[p.drive]->filename().string()) : std::string(1, char('a' + p.drive)));
    for (const std::string &part : p.parts) at /= part;
    return at;
}

int32_t FileSystem::chdir(const std::string &path)
{
    auto p = parse(path);
    if (!p) return err::kBadName;
    std::error_code ec;
    auto at = find(*p);
    if (!at || !fs::is_directory(*at, ec)) return err::kDirNotFound;
    cwd_[p->drive] = p->parts;
    return 0;
}

int32_t FileSystem::add_handle(std::FILE *f, uint32_t owner)
{
    for (size_t i = 0; i < handles_.size(); i++) {
        if (!handles_[i].file) {
            handles_[i] = {f, owner};
            return int32_t(i) + kFirstHandle;
        }
    }
    if (handles_.size() >= 90) {
        std::fclose(f);
        return err::kTooManyFiles;
    }
    handles_.push_back({f, owner});
    return int32_t(handles_.size() - 1) + kFirstHandle;
}

void FileSystem::search(const Path &p)
{
    if (!drives_[p.drive]) return;  // no disk in the drive: Human68k fails at once
    std::string dir = drives_[p.drive]->string();  // keyed by the disk, not the letter
    for (const std::string &part : p.parts) {      // the root, then each folder above the last part
        if (directories_read_.insert(dir).second) transfer(Floppy::kSectorBytes);
        dir += '\\' + part;
    }
}

int32_t FileSystem::charge_load(const std::string &path)
{
    auto p = parse(path);
    if (!p || p->parts.empty()) return err::kBadName;
    search(*p);
    auto at = drives_[p->drive] ? resolve(*drives_[p->drive], p->parts) : std::nullopt;  // the disk only
    std::error_code ec;
    uintmax_t size = at ? fs::file_size(*at, ec) : 0;
    if (!at || ec) return err::kFileNotFound;
    transfer(size);
    return int32_t(size);
}

int32_t FileSystem::open(const std::string &path, int mode, uint32_t owner)
{
    auto p = parse(path);
    if (!p || p->parts.empty()) return err::kBadName;
    search(*p);
    std::error_code ec;
    auto found = find(*p);
    if (!found || fs::is_directory(*found, ec)) return err::kFileNotFound;
    fs::path target = *found;
    if (mode != 0 && !overlay_.empty()) {
        // Writes go to the overlay, starting from the file as the game sees it.
        target = overlay(*p);
        if (!fs::exists(target, ec)) {
            fs::create_directories(target.parent_path(), ec);
            fs::copy_file(*found, target, fs::copy_options::overwrite_existing, ec);
            if (ec) return err::kFileNotFound;
        }
    }
    std::FILE *f = std::fopen(target.string().c_str(), mode == 0 ? "rb" : "r+b");
    return f ? add_handle(f, owner) : err::kFileNotFound;
}

int32_t FileSystem::create(const std::string &path, int, uint32_t owner)
{
    auto p = parse(path);
    if (!p || p->parts.empty() || overlay_.empty()) return err::kBadName;
    search(*p);
    fs::path over = overlay(*p);
    std::error_code ec;
    fs::create_directories(over.parent_path(), ec);
    std::FILE *f = std::fopen(over.string().c_str(), "w+b");
    return f ? add_handle(f, owner) : err::kDirNotFound;
}

int32_t FileSystem::close(int handle)
{
    size_t i = size_t(handle - kFirstHandle);
    if (handle < kFirstHandle || i >= handles_.size() || !handles_[i].file) return err::kBadHandle;
    std::fclose(handles_[i].file);
    handles_[i] = {};
    return 0;
}

int32_t FileSystem::read(int handle, uint32_t buffer, uint32_t length)
{
    size_t i = size_t(handle - kFirstHandle);
    if (handle < kFirstHandle || i >= handles_.size() || !handles_[i].file) return err::kBadHandle;
    std::vector<uint8_t> tmp(length);
    size_t n = std::fread(tmp.data(), 1, length, handles_[i].file);
    for (size_t k = 0; k < n; k++) mem_.poke8(buffer + uint32_t(k), tmp[k]);
    if (n) transfer(n);
    return int32_t(n);
}

int32_t FileSystem::write(int handle, uint32_t buffer, uint32_t length)
{
    size_t i = size_t(handle - kFirstHandle);
    if (handle < kFirstHandle || i >= handles_.size() || !handles_[i].file) return err::kBadHandle;
    std::vector<uint8_t> tmp(length);
    for (uint32_t k = 0; k < length; k++) tmp[k] = mem_.peek8(buffer + k);
    size_t n = std::fwrite(tmp.data(), 1, length, handles_[i].file);
    if (n) transfer(n);
    return int32_t(n);
}

int32_t FileSystem::seek(int handle, int32_t offset, int whence)
{
    size_t i = size_t(handle - kFirstHandle);
    if (handle < kFirstHandle || i >= handles_.size() || !handles_[i].file) return err::kBadHandle;
    std::fseek(handles_[i].file, offset, whence == 0 ? SEEK_SET : whence == 1 ? SEEK_CUR : SEEK_END);
    return int32_t(std::ftell(handles_[i].file));
}

void FileSystem::close_owned(uint32_t owner)
{
    for (size_t i = 0; i < handles_.size(); i++)
        if (handles_[i].file && handles_[i].owner == owner) close(int(i) + kFirstHandle);
}

std::vector<FileSystem::Entry> FileSystem::list(const Path &dir) const
{
    std::map<std::string, Entry> entries;  // a front layer hides the copies of a file behind it
    std::error_code ec;
    std::vector<fs::path> back_to_front = layers(dir.drive);
    std::reverse(back_to_front.begin(), back_to_front.end());
    for (const fs::path &layer : back_to_front) {
        auto at = resolve(layer, dir.parts);
        if (!at) continue;
        for (const auto &e : fs::directory_iterator(*at, ec)) {
            std::string name = lower(e.path().filename().string());
            bool is_dir = e.is_directory(ec);
            entries[name] = {name, e.path(), is_dir, is_dir ? 0u : uint32_t(e.file_size(ec))};
        }
    }
    std::vector<Entry> out;
    for (auto &kv : entries) out.push_back(kv.second);
    return out;
}

void FileSystem::fill(uint32_t buffer, const Entry &e, int drive)
{
    // Human68k FILBUF: search state up to +20, then attribute, time, date, size, name.
    mem_.poke8(buffer + 1, uint8_t(drive));
    mem_.poke8(buffer + 21, e.dir ? 0x10 : 0x20);
    mem_.poke16(buffer + 22, 0);
    mem_.poke16(buffer + 24, kDosDate);
    mem_.poke32(buffer + 26, e.size);
    mem_.put_cstring(buffer + 30, e.name.substr(0, 22));
}

int32_t FileSystem::files(uint32_t buffer, const std::string &pattern, int attr)
{
    auto p = parse(pattern);
    if (!p || p->parts.empty()) return err::kBadName;
    search(*p);
    std::string name = p->parts.back();
    p->parts.pop_back();
    Search s;
    for (Entry &e : list(*p)) {
        bool wanted = e.dir ? (attr & 0x10) : (attr & 0x20) || attr == 0;
        if (wanted && match(name, e.name)) s.entries.push_back(e);
    }
    if (s.entries.empty()) return err::kFileNotFound;
    mem_.poke8(buffer, uint8_t(attr));
    mem_.poke16(buffer + 2, uint16_t(searches_.size()));
    fill(buffer, s.entries[0], p->drive);
    s.next = 1;
    searches_.push_back(std::move(s));
    return 0;
}

int32_t FileSystem::nfiles(uint32_t buffer)
{
    size_t id = mem_.peek16(buffer + 2);
    if (id >= searches_.size()) return err::kNoMoreFiles;
    Search &s = searches_[id];
    if (s.next >= s.entries.size()) return err::kNoMoreFiles;
    fill(buffer, s.entries[s.next++], mem_.peek8(buffer + 1));
    return 0;
}

}  // namespace willow
