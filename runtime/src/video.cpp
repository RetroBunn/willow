#include "video.hpp"

#include <algorithm>

#include "common.hpp"

namespace willow {

static constexpr uint32_t kGraphicPalette = 0xE82000, kTextPalette = 0xE82200;
static constexpr uint32_t kSprites = 0xEB0000, kPcg = 0xEB8000;
static constexpr uint32_t kGvram = 0xC00000, kTvram = 0xE00000;

uint32_t Video::rgb(uint16_t c) const
{
    auto scale = [&](int five) { return uint32_t((five * 2 + (c & 1)) * 255 / 63); };
    return scale((c >> 6) & 31) << 16 | scale(c >> 11) << 8 | scale((c >> 1) & 31);
}

int Video::total_lines() const
{
    int total = crtc(4) + 1;
    return total > 16 && total <= 1100 ? total : 568;
}

void Video::geometry()
{
    // Display window from the CRTC: horizontal in 8-dot units, vertical in lines. In the
    // 31 kHz modes a 256-line picture is scanned twice, so its window counts each line twice.
    uint16_t r20 = crtc(20);
    bool khz31 = r20 & 0x10, lines256 = ((r20 >> 2) & 3) == 0;
    scan_ = khz31 && lines256 ? 2 : 1;
    int w = (crtc(3) - crtc(2)) * 8, h = (crtc(7) - crtc(6)) / scan_;
    width_ = w > 0 && w <= 1024 ? w : 512;
    height_ = h > 0 && h <= 1024 ? h : 512;
    // On the monitor, a full line is 4 units wide at 256 or 512 dots (by mode) and the
    // full height is 3 units at 256 or 512 lines.
    float dots = (r20 & 3) == 0 ? 256.0f : 512.0f, lines = lines256 ? 256.0f : 512.0f;
    aspect_ = (width_ * 4.0f / dots) / (height_ * 3.0f / lines);
}

std::string Video::describe() const
{
    std::string s = "CRTC";
    for (int n = 0; n <= 23; n++) s += format(" %04x", crtc(n));
    s += format(" | VC %04x %04x %04x | BG %04x %04x %04x %04x ctl %04x res %04x %04x %04x %04x", vc(0), vc(1), vc(2),
                mem_.peek16(0xEB0800), mem_.peek16(0xEB0802), mem_.peek16(0xEB0804), mem_.peek16(0xEB0806),
                mem_.peek16(0xEB0808), mem_.peek16(0xEB080A), mem_.peek16(0xEB080C), mem_.peek16(0xEB080E),
                mem_.peek16(0xEB0810));
    return s;
}

int Video::text_pixel(int x, int y) const
{
    int tx = (x + crtc(10)) & 1023, ty = (y + crtc(11)) & 1023;
    uint32_t at = kTvram + uint32_t(ty) * 128 + uint32_t(tx >> 3);
    int bit = 7 - (tx & 7), index = 0;
    for (int plane = 0; plane < 4; plane++) index |= ((mem_.peek8(at + plane * 0x20000u) >> bit) & 1) << plane;
    return index;
}

int Video::graphic_codes(int x, int y, int codes[4]) const
{
    uint16_t mode = vc(0) & 3, on = vc(2) & 0x0F, order = vc(1) & 0xFF;
    if (mode == 3) {  // 65536 colors: one page, direct color
        int gx = (x + crtc(12)) & 511, gy = (y + crtc(13)) & 511;
        codes[0] = (on & 1) ? mem_.peek16(kGvram + (uint32_t(gy) * 512 + uint32_t(gx)) * 2) : 0;
        return 1;
    }
    int pages = mode == 1 ? 2 : 4, n = 0;
    for (int pos = 0; pos < 4; pos++) {  // front to back
        for (int page = 0; page < pages; page++) {
            int slot = mode == 1 ? page * 2 : page;  // in 256-color mode a page uses two priority slots
            if (((order >> (slot * 2)) & 3) != pos) continue;
            int code = 0;
            if (on & (1 << slot)) {
                int gx = (x + crtc(12 + 2 * slot)) & 511, gy = (y + crtc(13 + 2 * slot)) & 511;
                uint32_t base = kGvram + uint32_t(page) * 0x80000u;  // 512x512 words per page
                uint16_t v = mem_.peek16(base + (uint32_t(gy) * 512 + uint32_t(gx)) * 2);
                code = mode == 1 ? (v & 0xFF) : (v & 0x0F);
            }
            codes[n++] = code;
        }
    }
    return n;
}

// Translucency mixes two color words: each of G, R and B becomes the average of the two, and
// the intensity bit is the back color's (XEiJ's vcnMix2).
static uint16_t mix(uint16_t front, uint16_t back)
{
    return uint16_t(((((front & 0xF83E) + (back & 0xF83E)) >> 1) & 0xF83E) |
                    (((((front & 0x07C0) | 1) + (back & 0x07C1)) >> 1) & 0x07C1));
}

void Video::sprite_line(int y, uint16_t *out) const
{
    // Back to front: sprites with priority 1, BG1, priority 2, BG0, priority 3. Lower sprite
    // numbers are in front of higher ones.
    uint16_t control = mem_.peek16(0xEB0808);
    bool big_cells = mem_.peek16(0xEB0810) & 3;  // 512-dot mode: 16x16 BG cells
    std::fill(out, out + width_, uint16_t(0));
    // The hardware shows at most kSpritesPerLine sprites on a line, the first ones in sprite
    // number order; the rest are not drawn. Sprites with priority 0 (hidden) don't count,
    // sprites off the sides of the picture do (XEiJ, as checked on an X68030).
    bool shown[128] = {};
    for (int n = 0, count = 0; n < 128 && count < kSpritesPerLine; n++) {
        uint32_t s = kSprites + uint32_t(n) * 8;
        int row = y - ((mem_.peek16(s + 2) & 0x3FF) - 16);
        if ((mem_.peek16(s + 6) & 3) && row >= 0 && row < 16) shown[n] = true, count++;
    }
    auto sprites = [&](int priority) {
        for (int n = 127; n >= 0; n--) {
            uint32_t s = kSprites + uint32_t(n) * 8;
            if (!shown[n] || (mem_.peek16(s + 6) & 3) != priority) continue;
            int sy = (mem_.peek16(s + 2) & 0x3FF) - 16, row = y - sy;
            if (row < 0 || row >= 16) continue;
            int sx = (mem_.peek16(s) & 0x3FF) - 16;
            uint16_t ctl = mem_.peek16(s + 4);
            if (ctl & 0x8000) row = 15 - row;
            uint32_t pattern = kPcg + (ctl & 0xFFu) * 128;
            for (int col = 0; col < 16; col++) {
                int x = sx + col;
                if (x < 0 || x >= width_) continue;
                int c = ctl & 0x4000 ? 15 - col : col;
                uint32_t at = pattern + uint32_t((c >> 3) * 2 + (row >> 3)) * 32 + uint32_t(row & 7) * 4 + uint32_t((c & 7) >> 1);
                int index = (c & 1) ? mem_.peek8(at) & 15 : mem_.peek8(at) >> 4;
                if (index) out[x] = uint16_t(((ctl >> 8) & 15) * 16 + index + 1);
            }
        }
    };
    auto bg = [&](int which) {
        if (!(control & (which ? 0x08 : 0x01))) return;
        uint32_t page = 0xEBC000 + ((control >> (which ? 4 : 1)) & 1) * 0x2000u;
        int sx = mem_.peek16(0xEB0800 + 4u * which), sy = mem_.peek16(0xEB0802 + 4u * which);
        int cell = big_cells ? 16 : 8, span = 64 * cell;
        int by = (y + sy) & (span - 1);
        for (int x = 0; x < width_; x++) {
            int bx = (x + sx) & (span - 1);
            uint16_t v = mem_.peek16(page + uint32_t((by / cell) * 64 + bx / cell) * 2);
            int px = bx % cell, py = by % cell;
            if (v & 0x4000) px = cell - 1 - px;
            if (v & 0x8000) py = cell - 1 - py;
            uint32_t at = big_cells ? kPcg + (v & 0xFFu) * 128 + uint32_t((px >> 3) * 2 + (py >> 3)) * 32 + uint32_t(py & 7) * 4 + uint32_t((px & 7) >> 1)
                                    : kPcg + (v & 0xFFu) * 32 + uint32_t(py) * 4 + uint32_t(px >> 1);
            int index = (px & 1) ? mem_.peek8(at) & 15 : mem_.peek8(at) >> 4;
            if (index) out[x] = uint16_t(((v >> 8) & 15) * 16 + index + 1);
        }
    };
    sprites(1);
    bg(1);
    sprites(2);
    bg(0);
    sprites(3);
}

void Video::render_line(int y)
{
    uint16_t r1 = vc(1), r2 = vc(2);
    bool text_on = r2 & 0x20, sprites_on = (r2 & 0x40) && (mem_.peek16(0xEB0808) & 0x200);
    // Layers sorted front to back by their R1 priority fields (lower is in front).
    struct Layer {
        int kind, priority;
    } layers[3] = {{0, (r1 >> 8) & 3}, {1, (r1 >> 10) & 3}, {2, (r1 >> 12) & 3}};  // graphics, text, sprites
    std::stable_sort(layers, layers + 3, [](const Layer &a, const Layer &b) { return a.priority < b.priority; });
    // VC R2 bit 12 turns on the extended modes, and bit 11 picks one: special priority (off) or
    // translucency (on). Both act on marked graphic pixels. With bit 10 (B/P) on, the mark is an
    // odd palette code of 3 or more on the front plane, which counts as the even code below it;
    // unmarked, every plane's code loses its low bit, so code 1 turns transparent. With B/P off,
    // a code is marked when the color word of the even code below it is odd.
    //   Special priority: a marked pixel shows in front of the text and sprites, whatever R1 says.
    //   Translucency: a marked pixel is mixed with the 2nd plane's code made odd (bit 9) and with
    //   the text and sprites behind the graphic screen (bit 8), or black if there are none.
    // The rules are XEiJ's. Translucency marked by color and the 65,536-color mode's extended
    // modes are drawn as if off.
    bool direct = (vc(0) & 3) == 3, extended = (r2 & 0x1000) && !direct, translucent = r2 & 0x800;
    bool by_code = r2 & 0x400;
    if ((r2 & 0x1000) && (r2 & 0x1F) && (direct || (translucent && !by_code)))
        log_once(format("vc-r2-%04x", r2), "VC R2 %04x: this extended mode is drawn as if off", r2);
    auto palette = [&](int code) { return mem_.peek16(kGraphicPalette + uint32_t(code) * 2); };
    auto text_word = [&](int x, uint16_t *w) {  // the text screen's color here, if any
        int index = text_on ? text_pixel(x, y) : 0;
        if (index) *w = mem_.peek16(kTextPalette + uint32_t(index) * 2);
        return index != 0;
    };
    uint32_t backdrop = rgb(mem_.peek16(kTextPalette));
    const unsigned contrast = unsigned(contrast_), full = 15 * kContrastScale;
    std::vector<uint16_t> sprite_row(static_cast<size_t>(width_));
    if (sprites_on) sprite_line(y, sprite_row.data());
    auto layer_word = [&](int kind, int x, uint16_t *w) {  // the text or sprite screen's color, if any
        if (kind == 1) return text_word(x, w);
        if (!sprites_on || !sprite_row[size_t(x)]) return false;
        *w = mem_.peek16(kTextPalette + uint32_t(sprite_row[size_t(x)] - 1) * 2);
        return true;
    };
    for (int x = 0; x < width_; x++) {
        // The graphic screen here: its color word if it isn't transparent, and whether it goes
        // in front of everything or is mixed with what's behind it.
        int codes[4], planes = graphic_codes(x, y, codes);
        uint16_t word = 0;
        bool opaque = false, front = false, mix_behind = false;
        if (direct) {
            word = uint16_t(codes[0]);
            opaque = word != 0;
        } else if (extended && by_code && (codes[0] & 1) && codes[0] >= 3) {
            word = palette(codes[0] & ~1);
            if (!translucent) {
                front = true;
            } else {
                if (r2 & 0x200) word = mix(word, palette(codes[1] | 1));
                mix_behind = r2 & 0x100;
            }
            opaque = word != 0 || front;
        } else {
            int mask = extended && by_code ? ~1 : ~0;
            for (int i = 0; i < planes && !opaque; i++) {
                if (int code = codes[i] & mask) {
                    word = palette(code);
                    opaque = true;
                    front = extended && !translucent && !by_code && (palette(code & ~1) & 1);
                }
            }
        }
        uint32_t color = backdrop;
        for (int i = 0; i < 3; i++) {
            uint16_t w;
            if (front || layers[i].kind == 0) {
                if (!opaque) continue;
                w = word;
                if (mix_behind) {
                    uint16_t behind = 0;
                    for (int j = i + 1; j < 3; j++)
                        if (layer_word(layers[j].kind, x, &behind)) break;
                    w = mix(word, behind);
                }
            } else if (!layer_word(layers[i].kind, x, &w)) {
                continue;
            }
            color = rgb(w);
            break;
        }
        uint8_t *p = &rgb_[(size_t(y) * width_ + x) * 3];
        p[0] = uint8_t(((color >> 16) & 0xFF) * contrast / full);
        p[1] = uint8_t(((color >> 8) & 0xFF) * contrast / full);
        p[2] = uint8_t((color & 0xFF) * contrast / full);
    }
}

void Video::update_contrast(uint64_t now_ns)
{
    int target = (mem_.peek8(0xE8E001) & 15) * kContrastScale;  // the system port's setting
    if (!fading_) {
        if (target == contrast_) return;
        fading_ = true;
        next_step_ = now_ns + kContrastStepNs;
    }
    while (contrast_ != target && now_ns >= next_step_) {
        contrast_ += target > contrast_ ? 1 : -1;
        next_step_ += kContrastStepNs;
    }
    if (contrast_ == target) fading_ = false;
}

void Video::begin_frame()
{
    geometry();
    rgb_.assign(size_t(width_) * height_ * 3, 0);
    last_line_ = -1;
}

void Video::render_raster(int from, int to)
{
    from = std::max(from, display_start());
    to = std::min(to, display_end());
    for (int r = from; r < to; r++) {
        int y = (r - display_start()) / scan_;
        if (y != last_line_ && y < height_) render_line(y);
        last_line_ = y;
    }
}

void Video::end_frame()
{
    std::lock_guard<std::mutex> lock(mutex_);
    done_ = rgb_;
    done_width_ = width_;
    done_height_ = height_;
    done_aspect_ = aspect_;
}

bool Video::latest(std::vector<uint8_t> &rgb, int &width, int &height, float &aspect) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (done_.empty()) return false;
    rgb = done_;
    width = done_width_;
    height = done_height_;
    aspect = done_aspect_;
    return true;
}

const std::vector<uint8_t> &Video::render()
{
    begin_frame();
    for (int y = 0; y < height_; y++) render_line(y);
    return rgb_;
}

}  // namespace willow
