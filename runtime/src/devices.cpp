#include "devices.hpp"

#include <algorithm>

#include "common.hpp"

namespace willow {

// ---- MFP ----

uint8_t Mfp::read8(uint32_t addr)
{
    if (!(addr & 1)) return 0xFF;
    int i = index(addr);
    if (i == GPIP) return gpip_inputs ? gpip_inputs() : 0xFF;
    if (i == TSR) return 0x80;                   // the keyboard serial line is always ready to send
    if (i == RSR) return rx_full_ ? 0x80 : 0x00; // buffer full: a key code is waiting
    if (i == UDR) {
        rx_full_ = false;
        return rx_byte_;  // the last code received stays readable
    }
    return i < kRegs ? reg_[i] : 0xFF;
}

void Mfp::write8(uint32_t addr, uint8_t value)
{
    if (!(addr & 1)) return;
    int i = index(addr);
    switch (i) {
    case IERA:
    case IERB:
        reg_[i] = value;
        reg_[i + 2] &= value;  // disabling a channel drops its pending request
        break;
    case IPRA:
    case IPRB:
    case ISRA:
    case ISRB:
        reg_[i] &= value;  // writing 0 clears a bit, 1 leaves it
        break;
    case UDR:
        // A byte for the keyboard: LED states ($80 | bits) and similar commands. No keyboard
        // to light up, so it is only noted.
        log_once("keyboard-out", "keyboard command bytes sent (e.g. %02x): keyboard LEDs are not shown", value);
        break;
    default:
        if (i >= 12 && i < kRegs && value) log_once(format("mfp-%d", i), "MFP register %d written (%02x): timers and USART are not emulated yet", i, value);
        if (i < kRegs) reg_[i] = value;
    }
}

void Mfp::set(Reg a, Reg b, int ch, bool on)
{
    Reg r = ch >= 8 ? a : b;
    uint8_t mask = uint8_t(1 << (ch & 7));
    reg_[r] = on ? reg_[r] | mask : reg_[r] & ~mask;
}

void Mfp::raise(int channel)
{
    if (bit(IERA, IERB, channel)) set(IPRA, IPRB, channel, true);
}

void Mfp::receive(uint8_t byte)
{
    rx_byte_ = byte;
    rx_full_ = true;
    raise(kReceive);
}

int Mfp::pending_vector() const
{
    for (int ch = 15; ch >= 0; ch--) {
        if (bit(ISRA, ISRB, ch)) return -1;  // something at this priority or above is in service
        if (bit(IPRA, IPRB, ch) && bit(IMRA, IMRB, ch)) return (reg_[VR] & 0xF0) | ch;
    }
    return -1;
}

void Mfp::acknowledge(int vector)
{
    int ch = vector & 0x0F;
    set(IPRA, IPRB, ch, false);
    if (reg_[VR] & 0x08) set(ISRA, ISRB, ch, true);  // software end-of-interrupt mode
}

// ---- OPM ----

uint8_t Opm::read8(uint32_t addr)
{
    if ((addr & 3) == 3) return uint8_t((b_.flag ? 2 : 0) | (a_.flag ? 1 : 0));  // never busy
    return 0xFF;
}

void Opm::write8(uint32_t addr, uint8_t value)
{
    if ((addr & 3) == 1)
        latch_ = value;
    else if ((addr & 3) == 3)
        write_reg(latch_, value);
}

void Opm::restart(Timer &t, uint64_t period)
{
    t.period = period;
    t.next = now_ + period;
}

void Opm::write_reg(uint8_t reg, uint8_t value)
{
    uint8_t old = regs_[reg];
    regs_[reg] = value;
    if (reg == 0x14) {
        if (value & 0x10) a_.flag = false;
        if (value & 0x20) b_.flag = false;
        bool run_a = value & 1, run_b = value & 2;
        // Timer A counts 64 clocks per step from NA up to 1024; timer B 1024 clocks from NB to 256. 4 MHz.
        if (run_a && !(old & 1)) restart(a_, 16000ull * (1024 - ((regs_[0x10] << 2) | (regs_[0x11] & 3))));
        if (run_b && !(old & 2)) restart(b_, 256000ull * (256 - regs_[0x12]));
        a_.running = run_a;
        b_.running = run_b;
        refresh_irq();
    }
    if (on_write) on_write(reg, value);
}

void Opm::update(uint64_t now_ns)
{
    now_ = now_ns;
    for (Timer *t : {&a_, &b_}) {
        if (!t->running || now_ns < t->next) continue;
        // Skip straight past any overflows missed while the game thread was busy: they all
        // set the same flag, so replaying them one by one would change nothing.
        t->next += ((now_ns - t->next) / t->period + 1) * t->period;
        bool enabled = t == &a_ ? regs_[0x14] & 4 : regs_[0x14] & 8;
        if (enabled) t->flag = true;
    }
    refresh_irq();
}

uint64_t Opm::next_event(uint64_t now_ns) const
{
    uint64_t next = UINT64_MAX;
    for (const Timer *t : {&a_, &b_})
        if (t->running) next = std::min(next, std::max(t->next, now_ns));
    return next;
}

void Opm::refresh_irq()
{
    bool irq = (a_.flag && (regs_[0x14] & 4)) || (b_.flag && (regs_[0x14] & 8));
    bool rising = irq && !irq_;
    irq_ = irq;
    if (rising && on_irq) on_irq();
}

// ---- PPI ----

uint8_t Ppi::read8(uint32_t addr)
{
    switch (addr & 7) {
    case 1: return joystick[0];
    case 3: return joystick[1];
    case 5: return port_c_;
    default: return 0xFF;
    }
}

void Ppi::write8(uint32_t addr, uint8_t value)
{
    if ((addr & 7) == 5) {
        port_c_ = value;
    } else if ((addr & 7) == 7 && !(value & 0x80)) {  // bit set/reset of port C
        int bit = (value >> 1) & 7;
        port_c_ = value & 1 ? port_c_ | (1 << bit) : port_c_ & ~(1 << bit);
    }
}

}  // namespace willow
