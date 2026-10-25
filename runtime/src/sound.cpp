#include "sound.hpp"

#include <algorithm>
#include <cstdlib>
#include <string>

#include "common.hpp"
#include "ymfm_opm.h"

namespace willow {

// ---- DMA controller ----

// Channel 3 register offsets.
enum : uint32_t { CSR = 0x00, CER = 0x01, OCR = 0x05, CCR = 0x07, MTC = 0x0A, MAR = 0x0C, BTC = 0x1A, BAR = 0x1C, NIV = 0x25, EIV = 0x27 };

Dmac::Dmac(Memory &mem) : mem_(mem)
{
    regs_[kChannel3 + NIV] = 0x6A;  // as IOCS sets them at boot
    regs_[kChannel3 + EIV] = 0x6B;
}

uint32_t Dmac::get(uint32_t off, int bytes) const
{
    uint32_t v = 0;
    for (int i = 0; i < bytes; i++) v = v << 8 | regs_[(kChannel3 + off + uint32_t(i)) & 0xFF];
    return v;
}

void Dmac::put(uint32_t off, int bytes, uint32_t value)
{
    for (int i = bytes - 1; i >= 0; i--, value >>= 8) regs_[(kChannel3 + off + uint32_t(i)) & 0xFF] = uint8_t(value);
}

uint8_t Dmac::read8(uint32_t addr) { return regs_[addr & 0xFF]; }

void Dmac::write8(uint32_t addr, uint8_t value)
{
    uint32_t off = addr & 0xFF;
    if (off < kChannel3) {
        regs_[off] = value;
        return;
    }
    switch (off - kChannel3) {
    case CSR:
        regs_[off] &= uint8_t(~value);  // writing 1 clears a status bit
        break;
    case CCR:
        regs_[off] = uint8_t(value & ~0x90);
        if (value & 0x10) {  // software abort
            active_ = false;
            regs_[kChannel3 + CSR] = uint8_t((regs_[kChannel3 + CSR] & ~0x08) | 0x90);
            regs_[kChannel3 + CER] = 0x11;
        }
        if (value & 0x80) {  // start
            int chain = (regs_[kChannel3 + OCR] >> 2) & 3;
            if (chain == 1 || chain == 3) log_once("dmac-chain", "DMA channel 3 chain mode %d is not emulated", chain);
            regs_[kChannel3 + CSR] = uint8_t((regs_[kChannel3 + CSR] & ~0xF0) | 0x08);
            active_ = chain == 2 ? load_next() : get(MTC, 2) != 0;
        }
        break;
    default:
        regs_[off] = value;
    }
}

bool Dmac::load_next()
{
    while (get(BTC, 2) != 0) {
        uint32_t entry = get(BAR, 4);
        put(MAR, 4, mem_.read32(entry));
        put(MTC, 2, mem_.read16(entry + 4));
        put(BAR, 4, entry + 6);
        put(BTC, 2, get(BTC, 2) - 1);
        if (get(MTC, 2)) return true;
    }
    return false;
}

bool Dmac::fetch(uint8_t &byte)
{
    if (!active_) return false;
    uint32_t mar = get(MAR, 4);
    byte = mem_.read8(mar);
    put(MAR, 4, mar + 1);
    uint32_t count = get(MTC, 2) - 1;
    put(MTC, 2, count);
    bool chain = ((regs_[kChannel3 + OCR] >> 2) & 3) == 2;
    if (count == 0 && !(chain && load_next())) {
        static const bool trace = std::getenv("WILLOW_TRACE") != nullptr;
        if (trace) log("DMA channel 3 finished at %06x", mar + 1);
        active_ = false;
        regs_[kChannel3 + CSR] = uint8_t((regs_[kChannel3 + CSR] & ~0x08) | 0x80);  // operation complete
        if (regs_[kChannel3 + CCR] & 0x08) irq_ = true;
    }
    return true;
}

int Dmac::acknowledge()
{
    irq_ = false;
    return regs_[kChannel3 + NIV];
}

// ---- MSM6258 ----

static const int kStepSize[49] = {16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73,
                                  80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337,
                                  371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552};
static const int kStepShift[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

uint8_t Adpcm::read8(uint32_t addr) { return (addr & 3) == 1 ? uint8_t(playing_ ? 0x00 : 0x80) : 0x00; }

void Adpcm::write8(uint32_t addr, uint8_t value)
{
    if ((addr & 3) == 3) {
        data_ = value;
    } else if ((addr & 3) == 1) {
        if (value & 1) playing_ = false;
        if (value & 2) {
            static const bool trace = std::getenv("WILLOW_TRACE") != nullptr;
            if (trace) log("ADPCM voice %u starts at %.0f Hz", voices_started + 1, rate());
            voices_started++;
            playing_ = true;
            high_nibble_ = false;
            signal_ = 0;
            step_ = 0;
        }
        if (value & 4) log_once("adpcm-rec", "ADPCM recording is not emulated");
    }
}

double Adpcm::rate() const
{
    static const int dividers[4] = {1024, 768, 512, 512};
    double clock = (opm_.reg(0x1B) & 0x80) ? 4'000'000.0 : 8'000'000.0;  // CT1 selects 4 MHz
    return clock / dividers[(ppi_.port_c() >> 2) & 3];
}

int Adpcm::next()
{
    if (!playing_) return 0;
    if (!high_nibble_) {  // low nibble first; each byte comes by DMA when a transfer is running
        uint8_t b;
        if (dmac_.fetch(b)) data_ = b;
        byte_ = data_;
    }
    int nibble = high_nibble_ ? byte_ >> 4 : byte_ & 15;
    high_nibble_ = !high_nibble_;
    int step = kStepSize[step_], delta = step >> 3;
    if (nibble & 1) delta += step >> 2;
    if (nibble & 2) delta += step >> 1;
    if (nibble & 4) delta += step;
    signal_ = std::clamp(nibble & 8 ? signal_ - delta : signal_ + delta, -2048, 2047);
    step_ = std::clamp(step_ + kStepShift[nibble & 7], 0, 48);
    return signal_;
}

// ---- mixer ----

struct Sound::Chip : ymfm::ymfm_interface {
    ymfm::ym2151 opm{*this};
};

Sound::Sound(Adpcm &adpcm) : chip_(std::make_unique<Chip>()), adpcm_(adpcm) { chip_->opm.reset(); }

Sound::~Sound() = default;

uint32_t Sound::rate() const { return chip_->opm.sample_rate(kOpmClock); }

void Sound::write_opm(uint8_t reg, uint8_t value)
{
    // Debugging aid: WILLOW_SOLO=<0-7> keeps one FM channel audible by turning the others'
    // operator levels (TL, $60-$7F) all the way down.
    static const int solo = std::getenv("WILLOW_SOLO") ? std::atoi(std::getenv("WILLOW_SOLO")) : -1;
    if (solo >= 0 && reg >= 0x60 && reg < 0x80 && (reg & 7) != solo) value |= 0x7F;
    chip_->opm.write_address(reg);
    chip_->opm.write_data(value);
    // A write keeps the real chip busy for one sample (64 clocks), and drivers wait it out
    // before the next. Run the chip for that sample too: otherwise a key-off followed by a
    // key-on in the same tick, which is how notes restart, never shows the chip the key-off.
    render(1);
    produced_++;
}

void Sound::advance(uint64_t now_ns)
{
    uint64_t target = now_ns * rate() / 1'000'000'000ull;
    if (target <= produced_) return;  // writes may have run the chip a little ahead
    uint64_t frames = target - produced_;
    if (frames > rate() / 4) frames = rate() / 4;  // after a long stall, don't try to catch up
    produced_ = target;
    render(frames);
}

void Sound::render(uint64_t frames)
{
    double step = adpcm_.rate() / rate();
    bool left = adpcm_.left_on(), right = adpcm_.right_on();
    // Debugging aid: WILLOW_MUTE=fm or WILLOW_MUTE=adpcm silences one chip.
    static const std::string mute = std::getenv("WILLOW_MUTE") ? std::getenv("WILLOW_MUTE") : "";
    int fm_gain = mute == "fm" ? 0 : 1, adpcm_gain = mute == "adpcm" ? 0 : 8;
    buffer_.resize(size_t(frames) * 2);
    for (uint64_t i = 0; i < frames; i++) {
        ymfm::ym2151::output_data out;
        chip_->opm.generate(&out);
        phase_ += step;
        while (phase_ >= 1.0) {
            phase_ -= 1.0;
            previous_ = current_;
            current_ = adpcm_.next();
        }
        double voice = previous_ + (current_ - previous_) * phase_;  // linear interpolation up to 62.5 kHz
        dc_out_ = voice - dc_in_ + 0.999 * dc_out_;
        dc_in_ = voice;
        int adpcm = int(dc_out_ * adpcm_gain);
        buffer_[size_t(i) * 2] = int16_t(std::clamp(out.data[0] * fm_gain + (left ? adpcm : 0), -32768, 32767));
        buffer_[size_t(i) * 2 + 1] = int16_t(std::clamp(out.data[1] * fm_gain + (right ? adpcm : 0), -32768, 32767));
    }
    if (sink) sink(buffer_.data(), size_t(frames));
}

}  // namespace willow
