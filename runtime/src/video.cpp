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

int Video::graphic_pixel(int x, int y, uint32_t *color) const
{
    uint16_t mode = vc(0) & 3, on = vc(2) & 0x0F, order = vc(1) & 0xFF;
    if (mode == 3) {  // 65536 colors: one page, direct color
        if (!(on & 1)) return 0;
        int gx = (x + crtc(12)) & 511, gy = (y + crtc(13)) & 511;
        uint16_t v = mem_.peek16(kGvram + (uint32_t(gy) * 512 + uint32_t(gx)) * 2);
        if (!v) return 0;
        *color = rgb(v);
        return 1;
    }
    int pages = mode == 1 ? 2 : 4;
    for (int pos = 0; pos < 4; pos++) {  // front to back
        for (int page = 0; page < pages; page++) {
            int slot = mode == 1 ? page * 2 : page;  // in 256-color mode a page uses two priority slots
            if (((order >> (slot * 2)) & 3) != pos || !(on & (1 << slot))) continue;
            int gx = (x + crtc(12 + 2 * slot)) & 511, gy = (y + crtc(13 + 2 * slot)) & 511;
            uint32_t base = kGvram + uint32_t(page) * 0x80000u;  // 512x512 words per page
            uint16_t v = mem_.peek16(base + (uint32_t(gy) * 512 + uint32_t(gx)) * 2);
            int index = mode == 1 ? (v & 0xFF) : (v & 0x0F);
            if (index) {
                *color = rgb(mem_.peek16(kGraphicPalette + uint32_t(index) * 2));
                return 1;
            }
        }
    }
    return 0;
}

void Video::sprite_line(int y, uint16_t *out) const
{
    // Back to front: sprites with priority 1, BG1, priority 2, BG0, priority 3. Lower sprite
    // numbers are in front of higher ones.
    uint16_t control = mem_.peek16(0xEB0808);
    bool big_cells = mem_.peek16(0xEB0810) & 3;  // 512-dot mode: 16x16 BG cells
    std::fill(out, out + width_, uint16_t(0));
    auto sprites = [&](int priority) {
        for (int n = 127; n >= 0; n--) {
            uint32_t s = kSprites + uint32_t(n) * 8;
            if ((mem_.peek16(s + 6) & 3) != priority) continue;
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
    uint32_t backdrop = rgb(mem_.peek16(kTextPalette));
    const unsigned contrast = unsigned(contrast_), full = 15 * kContrastScale;
    std::vector<uint16_t> sprite_row(static_cast<size_t>(width_));
    if (sprites_on) sprite_line(y, sprite_row.data());
    for (int x = 0; x < width_; x++) {
        uint32_t color = backdrop;
        for (const Layer &layer : layers) {
            if (layer.kind == 0) {
                if (graphic_pixel(x, y, &color)) break;
            } else if (layer.kind == 1) {
                int index = text_on ? text_pixel(x, y) : 0;
                if (index) {
                    color = rgb(mem_.peek16(kTextPalette + uint32_t(index) * 2));
                    break;
                }
            } else if (sprites_on && sprite_row[size_t(x)]) {
                color = rgb(mem_.peek16(kTextPalette + uint32_t(sprite_row[size_t(x)] - 1) * 2));
                break;
            }
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
