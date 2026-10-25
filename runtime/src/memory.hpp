// The X68000 address space: 24 bits, big-endian.
//
//   000000-(RAM)    main RAM
//   C00000-DFFFFF   graphic VRAM
//   E00000-E7FFFF   text VRAM
//   E80000-EFFFFF   I/O, in 8 KB pages; a device can claim a page
//   F00000-FFFFFF   ROM (fonts and IPL); empty here, vectors point into it by default
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace willow {

class IoDevice {
public:
    virtual ~IoDevice() = default;
    virtual uint8_t read8(uint32_t addr) = 0;
    virtual void write8(uint32_t addr, uint8_t value) = 0;
};

class Memory {
public:
    static constexpr uint32_t kSize = 1u << 24;
    static constexpr uint32_t kIoStart = 0xE80000, kIoEnd = 0xF00000, kIoPage = 0x2000;
    static constexpr uint32_t kRomStart = 0xF00000;

    explicit Memory(uint32_t ram_size);

    uint32_t ram_size() const { return ram_size_; }
    void map(uint32_t start, uint32_t end, IoDevice *device);
    void storage(uint32_t start, uint32_t end);  // I/O pages that are plain registers on purpose: don't log them

    // What the CPU sees: devices get their registers, unmapped areas are logged.
    uint8_t read8(uint32_t addr);
    uint16_t read16(uint32_t addr);
    uint32_t read32(uint32_t addr);
    void write8(uint32_t addr, uint8_t value);
    void write16(uint32_t addr, uint16_t value);
    void write32(uint32_t addr, uint32_t value);

    // Direct storage access for the runtime itself (loaders, system calls, the renderer).
    uint8_t *data() { return bytes_.data(); }
    uint8_t peek8(uint32_t addr) const { return bytes_[addr & 0xFFFFFF]; }
    uint16_t peek16(uint32_t addr) const { return uint16_t(peek8(addr) << 8 | peek8(addr + 1)); }
    uint32_t peek32(uint32_t addr) const { return uint32_t(peek16(addr)) << 16 | peek16(addr + 2); }
    void poke8(uint32_t addr, uint8_t v) { bytes_[addr & 0xFFFFFF] = v; }
    void poke16(uint32_t addr, uint16_t v);
    void poke32(uint32_t addr, uint32_t v);
    std::string cstring(uint32_t addr, size_t max = 256) const;
    void put_cstring(uint32_t addr, const std::string &s);

    // Debugging aid: writes by the CPU to this one RAM address are reported.
    uint32_t watch = 0xFFFFFFFF;
    std::function<void(uint8_t value)> on_watch;

private:
    bool unmapped(uint32_t addr) const { return addr >= ram_size_ && addr < 0xC00000; }
    void text_write(uint32_t addr, uint8_t value);  // text VRAM, under the CRTC's R21/R23 access modes

    std::vector<uint8_t> bytes_;
    std::array<IoDevice *, (kIoEnd - kIoStart) / kIoPage> io_{};
    std::array<bool, (kIoEnd - kIoStart) / kIoPage> storage_{};
    uint32_t ram_size_;
};

}  // namespace willow
