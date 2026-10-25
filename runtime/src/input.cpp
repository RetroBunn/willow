#include "input.hpp"

namespace willow {

void Input::key(uint8_t code, bool down)
{
    if (!code || code >= 0x80) return;
    std::lock_guard<std::mutex> lock(mutex_);
    keys_.push_back(uint8_t(down ? code : code | 0x80));
}

void Input::joystick(int port, uint8_t pressed) { joy_[port & 1] = pressed; }

bool Input::next_key(uint8_t &byte)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (keys_.empty()) return false;
    byte = keys_.front();
    keys_.pop_front();
    return true;
}

}  // namespace willow
