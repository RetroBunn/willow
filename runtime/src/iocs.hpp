// IOCS (trap #15), reimplemented natively: the calls the game's programs make.
#pragma once

#include <cstdint>

namespace willow {

class Machine;

class Iocs {
public:
    explicit Iocs(Machine &m) : m_(m) {}
    void call(uint8_t number);  // registers in, registers out, as IOCS does

private:
    Machine &m_;
    uint8_t text_color_ = 3;
    uint16_t cursor_x_ = 0, cursor_y_ = 0;
};

}  // namespace willow
