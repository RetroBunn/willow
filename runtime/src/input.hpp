// Input as the X68000 sees it: the keyboard sends key codes (1-$7F, with bit 7 set
// on release) over the MFP's serial line, and joysticks pull the PPI's port lines
// low. The frontend reports keys and joystick state from its own thread; the game
// thread hands them over in Machine::service().
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>

namespace willow {

class Input {
public:
    enum Joystick : uint8_t { kUp = 1, kDown = 2, kLeft = 4, kRight = 8, kA = 0x20, kB = 0x40 };

    // Any thread.
    void key(uint8_t code, bool down);            // an X68000 key code
    void joystick(int port, uint8_t pressed);     // Joystick bits, set while held

    // Game thread.
    bool next_key(uint8_t &byte);
    uint8_t port(int port) const { return uint8_t(~joy_[port & 1].load()); }  // the lines read active low

private:
    std::mutex mutex_;
    std::deque<uint8_t> keys_;
    std::atomic<uint8_t> joy_[2] = {0, 0};
};

}  // namespace willow
