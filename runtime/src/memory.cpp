#include "memory.hpp"

#include "common.hpp"

namespace willow {

Memory::Memory(uint32_t ram_size) : bytes_(kSize), ram_size_(ram_size) {}

void Memory::map(uint32_t start, uint32_t end, IoDevice *device)
{
    for (uint32_t a = start; a < end; a += kIoPage) io_[(a - kIoStart) / kIoPage] = device;
}

void Memory::storage(uint32_t start, uint32_t end)
{
    for (uint32_t a = start; a < end; a += kIoPage) storage_[(a - kIoStart) / kIoPage] = true;
}

uint8_t Memory::read8(uint32_t addr)
{
    addr &= 0xFFFFFF;
    if (addr >= kIoStart && addr < kIoEnd) {
        size_t page = (addr - kIoStart) / kIoPage;
        if (IoDevice *d = io_[page]) return d->read8(addr);
        if (!storage_[page])
            log_once(format("io-r-%x", addr & ~(kIoPage - 1)), "unhandled I/O read at %06x (page logged once)", addr);
    } else if (unmapped(addr)) {
        log_once(format("bus-r-%x", addr >> 16), "read from unmapped address %06x", addr);
    }
    return bytes_[addr];
}

uint16_t Memory::read16(uint32_t addr) { return uint16_t(read8(addr) << 8 | read8(addr + 1)); }

uint32_t Memory::read32(uint32_t addr) { return uint32_t(read16(addr)) << 16 | read16(addr + 2); }

void Memory::write8(uint32_t addr, uint8_t value)
{
    addr &= 0xFFFFFF;
    if (addr >= kIoStart && addr < kIoEnd) {
        size_t page = (addr - kIoStart) / kIoPage;
        if (IoDevice *d = io_[page]) {
            d->write8(addr, value);
            return;
        }
        if (!storage_[page])
            log_once(format("io-w-%x", addr & ~(kIoPage - 1)), "unhandled I/O write at %06x (page logged once)", addr);
    } else if (unmapped(addr)) {
        log_once(format("bus-w-%x", addr >> 16), "write to unmapped address %06x", addr);
        return;
    } else if (addr >= kRomStart) {
        log_once(format("rom-w-%x", addr >> 16), "write to ROM at %06x", addr);
        return;
    } else if (addr >= 0xE00000 && addr < 0xE80000) {
        text_write(addr, value);
        return;
    }
    bytes_[addr] = value;
    if (addr == watch) on_watch(value);
}

void Memory::text_write(uint32_t addr, uint8_t value)
{
    // CRTC R21: bit 8 writes the same byte to every plane selected in bits 7-4 at once; bit 9
    // protects the bits set in R23 (the mask's high byte covers even addresses).
    uint16_t r21 = peek16(0xE8002A);
    uint8_t mask = (r21 & 0x200) ? uint8_t(addr & 1 ? peek16(0xE8002E) : peek16(0xE8002E) >> 8) : 0;
    auto put = [&](uint32_t at) { bytes_[at] = uint8_t((bytes_[at] & mask) | (value & ~mask)); };
    if (!(r21 & 0x100)) {
        put(addr);
        return;
    }
    uint32_t offset = addr & 0x1FFFF;
    for (int plane = 0; plane < 4; plane++)
        if (r21 & (0x10 << plane)) put(0xE00000 + uint32_t(plane) * 0x20000 + offset);
}

void Memory::write16(uint32_t addr, uint16_t value)
{
    write8(addr, uint8_t(value >> 8));
    write8(addr + 1, uint8_t(value));
}

void Memory::write32(uint32_t addr, uint32_t value)
{
    write16(addr, uint16_t(value >> 16));
    write16(addr + 2, uint16_t(value));
}

void Memory::poke16(uint32_t addr, uint16_t v)
{
    poke8(addr, uint8_t(v >> 8));
    poke8(addr + 1, uint8_t(v));
}

void Memory::poke32(uint32_t addr, uint32_t v)
{
    poke16(addr, uint16_t(v >> 16));
    poke16(addr + 2, uint16_t(v));
}

std::string Memory::cstring(uint32_t addr, size_t max) const
{
    std::string s;
    for (size_t i = 0; i < max; i++) {
        char c = char(peek8(addr + uint32_t(i)));
        if (!c) break;
        s += c;
    }
    return s;
}

void Memory::put_cstring(uint32_t addr, const std::string &s)
{
    for (size_t i = 0; i < s.size(); i++) poke8(addr + uint32_t(i), uint8_t(s[i]));
    poke8(addr + uint32_t(s.size()), 0);
}

}  // namespace willow
