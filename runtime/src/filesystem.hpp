// Human68k drives backed by host folders: A: and B: are the extracted system
// and data disks. Names match case-insensitively, as on Human68k. Anything the
// game writes (scores, replays) goes to an overlay folder instead, so the
// extracted disks are never modified; reads look in the overlay first.
#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "memory.hpp"

namespace willow {

namespace err {
constexpr int32_t kFileNotFound = -2, kDirNotFound = -3, kTooManyFiles = -4, kBadHandle = -6,
                  kBadName = -13, kBadDrive = -15, kNoMoreFiles = -18;
}

class FileSystem {
public:
    explicit FileSystem(Memory &mem) : mem_(mem) {}
    ~FileSystem();

    void mount(int drive, std::filesystem::path root);  // drive 0 = A:
    void set_overlay(std::filesystem::path dir) { overlay_ = std::move(dir); }

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
    std::optional<std::filesystem::path> host(const Path &p) const;  // existing host file or folder
    std::filesystem::path overlay(const Path &p) const;
    std::vector<Entry> list(const Path &dir) const;
    void fill(uint32_t buffer, const Entry &e, int drive);
    int32_t add_handle(std::FILE *f, uint32_t owner);

    Memory &mem_;
    std::array<std::optional<std::filesystem::path>, 26> drives_;
    std::array<std::vector<std::string>, 26> cwd_;
    int current_ = 0;
    std::filesystem::path overlay_;
    std::vector<Handle> handles_;  // Human68k handle = index + 5 (0-4 are devices)
    std::vector<Search> searches_;
};

}  // namespace willow
