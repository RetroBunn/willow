// Devices with behavior the game depends on. The rest of the I/O area is plain
// register storage until it gets an emulation of its own.
#pragma once

#include <cstdint>
#include <functional>

#include "memory.hpp"

namespace willow {

// MC68901 MFP: the interrupt controller for VSYNC, raster, the YM2151 and timers.
// Channels 0-15 in priority order; on the X68000 its vectors start at $40.
class Mfp : public IoDevice {
public:
    enum Channel { kOpm = 3, kVdisp = 6, kReceive = 12, kRaster = 14 };
    static constexpr uint32_t kBase = 0xE88000;

    uint8_t read8(uint32_t addr) override;
    void write8(uint32_t addr, uint8_t value) override;

    void raise(int channel);               // an input or timer fired
    int pending_vector() const;            // highest-priority deliverable vector, or -1
    void acknowledge(int vector);          // the CPU took it
    std::function<uint8_t()> gpip_inputs;  // live state of the GPIP lines

    // The serial receiver, which the keyboard is wired to.
    void receive(uint8_t byte);
    bool receive_full() const { return rx_full_; }

private:
    enum Reg { GPIP, AER, DDR, IERA, IERB, IPRA, IPRB, ISRA, ISRB, IMRA, IMRB, VR,
               TACR, TBCR, TCDCR, TADR, TBDR, TCDR, TDDR, SCR, UCR, RSR, TSR, UDR, kRegs };
    static int index(uint32_t addr) { return int((addr & 0x3F) >> 1); }
    bool bit(Reg a, Reg b, int ch) const { return ch >= 8 ? reg_[a] >> (ch - 8) & 1 : reg_[b] >> ch & 1; }
    void set(Reg a, Reg b, int ch, bool on);

    uint8_t reg_[kRegs]{};
    uint8_t rx_byte_ = 0;
    bool rx_full_ = false;
};

// YM2151 (OPM): register file and timers. Sound output comes later; register
// writes are handed to `on_write` so an audio backend can follow them.
class Opm : public IoDevice {
public:
    static constexpr uint32_t kBase = 0xE90000;

    uint8_t read8(uint32_t addr) override;
    void write8(uint32_t addr, uint8_t value) override;
    void write_reg(uint8_t reg, uint8_t value);  // also used by IOCS _OPMSET
    uint8_t reg(uint8_t r) const { return regs_[r]; }

    void update(uint64_t now_ns);  // run the timers up to `now`
    bool irq() const { return irq_; }
    uint64_t next_event(uint64_t now_ns) const;

    std::function<void()> on_irq;  // the IRQ line went active
    std::function<void(uint8_t, uint8_t)> on_write;
    std::function<uint64_t()> clock;  // the machine's time, for the busy flag and timer starts

private:
    struct Timer {
        bool running = false, flag = false;
        uint64_t next = 0, period = 0;
    };
    void restart(Timer &t, uint64_t period);
    void refresh_irq();

    uint8_t latch_ = 0, regs_[256]{};
    Timer a_, b_;
    uint64_t time() const { return clock ? clock() : now_; }

    uint64_t now_ = 0, busy_until_ = 0;
    bool irq_ = false;
};

// 8255 PPI: joystick ports (active low, so idle reads $FF) and the ADPCM
// pan and clock-divider bits in port C.
class Ppi : public IoDevice {
public:
    static constexpr uint32_t kBase = 0xE9A000;
    uint8_t read8(uint32_t addr) override;
    void write8(uint32_t addr, uint8_t value) override;
    uint8_t port_c() const { return port_c_; }
    uint8_t joystick[2] = {0xFF, 0xFF};

private:
    uint8_t port_c_ = 0x0B;
};

}  // namespace willow
