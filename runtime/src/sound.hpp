// Sound: the YM2151 (through ymfm), the MSM6258 ADPCM chip, and the DMA channel
// that feeds it. Everything here runs on the game thread: Machine::service()
// asks for samples up to the current time, and the mixed result goes to `sink`
// (the SDL audio stream in the game, a WAV file in willow_dump).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "devices.hpp"
#include "memory.hpp"

namespace willow {

// HD63450 DMA controller. Channel 3 moves ADPCM data from memory to the MSM6258,
// one byte per request; the other channels are register storage.
class Dmac : public IoDevice {
public:
    static constexpr uint32_t kBase = 0xE84000, kChannel3 = 0xC0;

    explicit Dmac(Memory &mem);
    uint8_t read8(uint32_t addr) override;
    void write8(uint32_t addr, uint8_t value) override;

    bool fetch(uint8_t &byte);  // the next byte for the ADPCM chip, if a transfer is running
    bool irq_pending() const { return irq_; }
    int acknowledge();          // clears the request; returns the vector (channel 3's NIV)
    bool trace = false;         // log each transfer's end (WILLOW_TRACE)

private:
    uint32_t get(uint32_t off, int bytes) const;
    void put(uint32_t off, int bytes, uint32_t value);
    bool load_next();  // array chaining: the next {address, count} entry

    Memory &mem_;
    uint8_t regs_[256]{};
    bool active_ = false, irq_ = false;
};

// MSM6258: 4-bit OKI ADPCM. Its clock comes from the YM2151's CT1 pin (4 or 8 MHz),
// its divider and output enables from PPI port C.
class Adpcm : public IoDevice {
public:
    static constexpr uint32_t kBase = 0xE92000;

    Adpcm(Opm &opm, Ppi &ppi, Dmac &dmac) : opm_(opm), ppi_(ppi), dmac_(dmac) {}
    uint8_t read8(uint32_t addr) override;
    void write8(uint32_t addr, uint8_t value) override;

    double rate() const;  // samples per second
    int next();           // decode one sample (12-bit signed), or 0 when stopped
    bool left_on() const { return !(ppi_.port_c() & 2); }
    bool right_on() const { return !(ppi_.port_c() & 1); }
    uint32_t voices_started = 0;  // for diagnostics
    bool trace = false;           // log each voice's start (WILLOW_TRACE)

private:
    Opm &opm_;
    Ppi &ppi_;
    Dmac &dmac_;
    bool playing_ = false, high_nibble_ = false;
    uint8_t data_ = 0x88, byte_ = 0x88;
    int signal_ = 0, step_ = 0;
};

class Sound {
public:
    static constexpr uint32_t kOpmClock = 4'000'000;  // the X68000's YM2151 clock

    explicit Sound(Adpcm &adpcm);
    ~Sound();
    uint32_t rate() const;  // output sample rate: the YM2151's own, 62.5 kHz
    void write_opm(uint8_t reg, uint8_t value);
    void advance(uint64_t now_ns);  // generate everything up to `now`

    std::function<void(const int16_t *samples, size_t frames)> sink;  // interleaved stereo
    // For listening to parts of the music (WILLOW_MUTE, WILLOW_SOLO): silence a chip, or keep
    // only one FM channel audible by turning the others' operator levels all the way down.
    bool mute_fm = false, mute_adpcm = false;
    int solo = -1;

private:
    void render(uint64_t frames);  // generate and hand over `frames` samples

    struct Chip;
    std::unique_ptr<Chip> chip_;
    Adpcm &adpcm_;
    uint64_t produced_ = 0;
    double phase_ = 0;
    int previous_ = 0, current_ = 0;
    double dc_in_ = 0, dc_out_ = 0;  // high-pass on the ADPCM output, as the hardware's coupling does
    std::vector<int16_t> buffer_;
};

}  // namespace willow
