// The X68000 display: reads the video registers and memory, composes frames.
//
//   CRTC      $E80000  timing, display window, raster interrupt line (R09), scroll,
//                      display mode (R20)
//   VC        $E82000  palettes ($E82000 graphic, $E82200 text/sprite), R0 mode,
//                      R1 priorities, R2 layer enables
//   sprites   $EB0000  128 sprites, BG scroll and control, PCG and BG data at $EB8000
//   graphics  $C00000  pages of 512x512 pixels
//   text      $E00000  four 1024x1024 bitplanes
//
// Three layers, ordered by VC R1: text, graphics (pages ordered among themselves),
// and the sprite layer (sprites interleaved with BG0 and BG1 by their priority).
//
// Frames are drawn line by line as the beam advances (Machine drives this), so a
// register change made by a raster interrupt handler affects only the lines below it.
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "memory.hpp"

namespace willow {

class Video {
public:
    explicit Video(Memory &mem) : mem_(mem) {}

    // CRTC timing, in raster lines from the start of vertical sync.
    int total_lines() const;
    int display_start() const { return crtc(6); }
    int display_end() const { return crtc(7); }
    int raster_irq_line() const { return crtc(9); }

    void begin_frame();                   // latch the display size for this frame
    void render_raster(int from, int to); // draw the displayed lines among raster lines [from, to)
    void end_frame();                     // publish the finished frame

    // The last finished frame, RGB; safe to call from another thread.
    bool latest(std::vector<uint8_t> &rgb, int &width, int &height, float &aspect) const;
    // Render a whole frame immediately from the current registers (for tests).
    const std::vector<uint8_t> &render();
    std::string describe() const;

private:
    uint16_t crtc(int n) const { return mem_.peek16(0xE80000 + 2u * n); }
    uint16_t vc(int r) const { return mem_.peek16(0xE82400 + 0x100u * r); }
    uint32_t rgb(uint16_t c) const;  // a palette word, GGGGGRRRRRBBBBBI, as 0xRRGGBB
    void geometry();
    void render_line(int y);
    void sprite_line(int y, uint16_t *out) const;  // palette index + 1, or 0 if transparent
    int graphic_pixel(int x, int y, uint32_t *color) const;
    int text_pixel(int x, int y) const;

    Memory &mem_;
    int width_ = 512, height_ = 512, scan_ = 1, last_line_ = -1;
    float aspect_ = 1.0f;  // shape of the picture on the monitor, width / height
    std::vector<uint8_t> rgb_;

    mutable std::mutex mutex_;
    std::vector<uint8_t> done_;
    int done_width_ = 0, done_height_ = 0;
    float done_aspect_ = 1.0f;
};

}  // namespace willow
