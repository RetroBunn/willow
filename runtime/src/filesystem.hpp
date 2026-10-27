// Human68k drives backed by host folders: A: and B: are the extracted system
// and data disks. Names match case-insensitively, as on Human68k. Anything the
// game writes (scores, replays) goes to an overlay folder instead, so the
// extracted disks are never modified.
//
// A file is looked for in layers, front to back: the overlay (what the game wrote),
// the enabled mods' files from the last mod to the first, then the disk itself. The
// overlay and the mods hold a folder per disk (system, data), keyed by the disk rather
// than the drive letter, since bootcot.r swaps the letters with _DRVXCHG. Loading a
// program reads only the disk: the programs always come from the recompiled game.
//
// The calls also add up how long the floppy drives would have taken (Floppy),
// which Human68k then waits out with interrupts running, as on the real machine.
#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "memory.hpp"

namespace willow {

namespace err {
constexpr int32_t kFileNotFound = -2, kDirNotFound = -3, kTooManyFiles = -4, kBadHandle = -6,
                  kBadName = -13, kBadDrive = -15, kNoMoreFiles = -18;
}

// The X68000's 2HD floppy drives, for timing. A disk turns at 360 rpm with 8 sectors of
// 1024 bytes per track. A transfer waits for its first sector to come round, half a turn on
// average, then reads a cylinder (16 KB on its two tracks) every three turns: two to read it
// and one lost stepping to the next, which lets its first sector go by. That is about
// 32 KB/s. Where the files lie on the disk would move this by some tens of percent.
//
// By default Willow's drive is kSpeedup times faster than that: at the real speed, Cotton's
// startup keeps the EA logo up for about 75 seconds while it reads both disks in. The
// player's settings choose (FileSystem::set_speedup).
struct Floppy {
    static constexpr uint64_t kTurnNs = 60'000'000'000ull / 360;
    static constexpr uint64_t kSectorBytes = 1024, kCylinderBytes = 2 * 8 * kSectorBytes;
    static constexpr uint32_t kSpeedup = 3;
    static constexpr uint64_t transfer_ns(uint64_t bytes) { return kTurnNs / 2 + bytes * 3 * kTurnNs / kCylinderBytes; }
};

class FileSystem {
public:
    explicit FileSystem(Memory &mem) : mem_(mem) {}
    ~FileSystem();

    void mount(int drive, std::filesystem::path root);  // drive 0 = A:
    void set_overlay(std::filesystem::path dir) { overlay_ = std::move(dir); }
    void add_mod(std::filesystem::path files) { mods_.push_back(std::move(files)); }  // a mod's files/; later ones win
    void set_speedup(uint32_t speedup) { speedup_ = speedup; }  // how much faster than a real drive; 0 is instant

    int current_drive() const { return current_; }
    int32_t chgdrv(int drive);
    int32_t drvctrl(int mode, int drive);  // drive 0 = current, 1 = A:
    int32_t drvxchg(int a, int b);         // 1 = A:
    int32_t chdir(const std::string &path);

    int32_t open(const std::string &path, int mode, uint32_t owner);
    int32_t create(const std::string &path, int attr, uint32_t owner);
    int32_t close(int handle);
    int32_t read(int handle, uint32_t buffer, uint32_t length);
    int32_t write(int handle, uint32_t buffer, uint32_t length);
    int32_t seek(int handle, int32_t offset, int whence);
    int32_t files(uint32_t buffer, const std::string &pattern, int attr);
    int32_t nfiles(uint32_t buffer);
    void close_owned(uint32_t owner);

    // A whole file read in, as a program loader does, for its disk time only: returns its size.
    int32_t charge_load(const std::string &path);
    // The drive time the calls so far have added up, which Human68k waits out; resets it.
    uint64_t take_disk_time() { return std::exchange(disk_ns_, 0); }

private:
    struct Path {
        int drive;
        std::vector<std::string> parts;  // lowercased, from the drive root
    };
    struct Entry {
        std::string name;
        std::filesystem::path host;
        bool dir;
        uint32_t size;
    };
    struct Handle {
        std::FILE *file = nullptr;
        uint32_t owner = 0;
    };
    struct Search {
        std::vector<Entry> entries;
        size_t next = 0;
    };

    std::optional<Path> parse(const std::string &path) const;
    std::vector<std::filesystem::path> layers(int drive) const;    // where the drive's files are, front first
    std::optional<std::filesystem::path> find(const Path &p) const;  // the frontmost existing file or folder
    std::filesystem::path overlay(const Path &p) const;              // where a write to p goes
    std::vector<Entry> list(const Path &dir) const;
    void fill(uint32_t buffer, const Entry &e, int drive);
    int32_t add_handle(std::FILE *f, uint32_t owner);
    void search(const Path &p);  // the disk time of finding p: each directory on the way, read once
    void transfer(uint64_t bytes)
    {
        if (speedup_) disk_ns_ += Floppy::transfer_ns(bytes) / speedup_;
    }

    Memory &mem_;
    std::array<std::optional<std::filesystem::path>, 26> drives_;
    std::array<std::vector<std::string>, 26> cwd_;
    int current_ = 0;
    std::filesystem::path overlay_;
    std::vector<std::filesystem::path> mods_;  // in load order
    std::vector<Handle> handles_;  // Human68k handle = index + 5 (0-4 are devices)
    std::vector<Search> searches_;
    uint64_t disk_ns_ = 0;
    uint32_t speedup_ = Floppy::kSpeedup;
    std::set<std::string> directories_read_;  // Human68k keeps them in its buffers (BUFFERS = 20)
};

}  // namespace willow
