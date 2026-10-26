#include "iocs.hpp"

#include "common.hpp"
#include "human68k.hpp"
#include "machine.hpp"

namespace willow {

static constexpr uint32_t kRomVersion = 0x12931115;  // an X68000 ROM, older than the X68030's 1.3
static constexpr uint32_t kBootFloppy0 = 0x90;

void Iocs::call(uint8_t number)
{
    M68kContext &c = m_.ctx;
    Memory &mem = m_.mem;
    uint32_t r = 0;
    switch (number) {
    case 0x21: {  // _B_PRINT: no text screen yet, so the console gets it
        std::string s = mem.cstring(c.a[1]);
        log("text: %s", from_sjis(s).c_str());
        c.a[1] += uint32_t(s.size()) + 1;
        r = uint32_t(cursor_y_) << 16 | cursor_x_;
        break;
    }
    case 0x22:  // _B_COLOR
        r = text_color_;
        if (int32_t(c.d[1]) != -1) text_color_ = uint8_t(c.d[1]);
        break;
    case 0x23:  // _B_LOCATE
        r = uint32_t(cursor_y_) << 16 | cursor_x_;
        if (int32_t(c.d[1]) != -1) cursor_x_ = uint16_t(c.d[1]), cursor_y_ = uint16_t(c.d[2]);
        break;
    case 0x68:  // _OPMSET
        m_.opm.write_reg(uint8_t(c.d[1]), uint8_t(c.d[2]));
        break;
    case 0x6A: {  // _OPMINTST: the OPM interrupt is MFP channel 3, vector $43
        uint8_t ierb = mem.read8(Mfp::kBase + 0x09), imrb = mem.read8(Mfp::kBase + 0x15);
        if (c.a[1]) {
            mem.poke32(0x43 * 4, c.a[1]);
            mem.write8(Mfp::kBase + 0x09, uint8_t(ierb | 0x08));
            mem.write8(Mfp::kBase + 0x15, uint8_t(imrb | 0x08));
        } else {
            mem.write8(Mfp::kBase + 0x09, uint8_t(ierb & ~0x08));
            mem.write8(Mfp::kBase + 0x15, uint8_t(imrb & ~0x08));
            mem.poke32(0x43 * 4, Machine::kRomVectors + 0x43 * 4);
        }
        break;
    }
    case 0x6D: {  // _CRTCRAS: raster interrupt, MFP channel 14, vector $4E
        uint8_t iera = mem.read8(Mfp::kBase + 0x07), imra = mem.read8(Mfp::kBase + 0x13);
        if (c.a[1]) {
            mem.poke32(0x4E * 4, c.a[1]);
            mem.poke16(0xE80012, uint16_t(c.d[1]));  // CRTC R09, the raster line to interrupt at
            mem.write8(Mfp::kBase + 0x07, uint8_t(iera | 0x40));
            mem.write8(Mfp::kBase + 0x13, uint8_t(imra | 0x40));
        } else {
            mem.write8(Mfp::kBase + 0x07, uint8_t(iera & ~0x40));
            mem.write8(Mfp::kBase + 0x13, uint8_t(imra & ~0x40));
        }
        break;
    }
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
        break;  // the mouse calls: no mouse, nothing moves, no buttons
    case 0x7D:  // _SKEY_MOD: no software keyboard
        break;
    case 0x80:  // _B_INTVCS
        r = m_.dos->intvcs(uint16_t(c.d[1]), c.a[1]);
        break;
    case 0x82:  // _B_BPEEK
        r = (c.d[0] & 0xFFFFFF00) | mem.read8(c.a[1]);
        c.a[1] += 1;
        break;
    case 0x84:  // _B_LPEEK
        r = mem.read32(c.a[1]);
        c.a[1] += 4;
        break;
    case 0x87:  // _B_WPOKE
        mem.write16(c.a[1], uint16_t(c.d[1]));
        c.a[1] += 2;
        break;
    case 0x8E: r = kBootFloppy0; break;  // _BOOTINF
    case 0x8F: r = kRomVersion; break;   // _ROMVER
    case 0xAC: break;                    // _SYS_STAT: no caches to manage
    case 0xAE: case 0xAF: break;         // _OS_CURON, _OS_CUROF: no text cursor yet
    case 0x03:
        log_once("iocs-03", "IOCS $03 ignored");
        break;
    default:
        log_once(format("iocs-%02x", number), "IOCS call $%02X is not implemented", number);
        r = 0xFFFFFFFF;
    }
    c.d[0] = r;
}

}  // namespace willow
