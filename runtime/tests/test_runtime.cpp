// Headless runtime tests. The "programs" here are hand-written stand-ins for
// recompiled code, built against the same interface; no game code is involved.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "boot.hpp"
#include "common.hpp"
#include "filesystem.hpp"
#include "human68k.hpp"
#include "iocs.hpp"
#include "machine.hpp"
#include "settings.hpp"
#include "toml.hpp"

namespace fs = std::filesystem;
using namespace willow;

static int checks = 0, failures = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        checks++;                                                                    \
        if (!(cond)) {                                                               \
            failures++;                                                              \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
        }                                                                            \
    } while (0)

#define CHECK_EQ(a, b)                                                                              \
    do {                                                                                            \
        checks++;                                                                                   \
        unsigned long long va = (unsigned long long)(a), vb = (unsigned long long)(b);              \
        if (va != vb) {                                                                             \
            failures++;                                                                             \
            std::fprintf(stderr, "%s:%d: %s == %s failed: %llx != %llx\n", __FILE__, __LINE__, #a, #b, va, vb); \
        }                                                                                           \
    } while (0)

// ---- stand-in programs ----

static void rte(M68kContext *c)
{
    uint16_t sr = m68k_read16(c->a[7]);
    c->ret = m68k_read32(c->a[7] + 2);
    c->a[7] += 6;
    m68k_set_sr(c, sr);
}

// addq.w #1,$5000.w ; rte
static void count_isr(M68kContext *c)
{
    m68k_write16(0x5000, uint16_t(m68k_read16(0x5000) + 1));
    rte(c);
}

// addq.w #1,$5000.w ; reset the OPM timer B flag ; rte
static void opm_isr(M68kContext *c)
{
    m68k_write16(0x5000, uint16_t(m68k_read16(0x5000) + 1));
    m68k_write8(0xE90001, 0x14);
    m68k_write8(0xE90003, 0x3A);
    rte(c);
}

static const uint8_t blank_image[64] = {};
static uint32_t isr_base;
static const M68kFuncEntry isr_funcs[] = {{0x00, count_isr}, {0x10, opm_isr}};
static const M68kProgram isr_program = {"isr.r", blank_image, sizeof blank_image, 0, 0, nullptr, 0, isr_funcs, 2, &isr_base};

static uint32_t child_regs[5];
static uint64_t child_cycles;
static void child_keeppr(M68kContext *c)  // record a0-a4 and the time, spend 500 cycles, _KEEPPR #$40 with code 7
{
    std::memcpy(child_regs, c->a, sizeof child_regs);
    child_cycles = c->cycles;
    c->cycles += 500;
    c->d[1] = 0xBAD;
    c->a[7] -= 2;
    m68k_write16(c->a[7], 7);
    m68k_push32(c, 0x40);
    m68k_line_f(c, 0xFF31, 0);
}
static void child_exit(M68kContext *c) { m68k_line_f(c, 0xFF00, 0); }  // _EXIT

static uint32_t keep_base, exit_base;
static const M68kFuncEntry keep_funcs[] = {{0x04, child_keeppr}};
static const M68kFuncEntry exit_funcs[] = {{0x00, child_exit}};
static const M68kProgram keep_program = {"keep.r", blank_image, sizeof blank_image, 0x20, 0x04, nullptr, 0, keep_funcs, 1, &keep_base};
static const M68kProgram exit_program = {"exit.r", blank_image, sizeof blank_image, 0, 0, nullptr, 0, exit_funcs, 1, &exit_base};

// A relocatable image: the long at offset 8 points at offset $20 before loading.
static const uint8_t reloc_image[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x20, 0, 0, 0, 0};
static const uint32_t reloc_offsets[] = {8};
static uint32_t reloc_base;
static const M68kProgram reloc_program = {"reloc.r", reloc_image, sizeof reloc_image, 0, 0, reloc_offsets, 1, exit_funcs, 1, &reloc_base};

// ---- helpers ----

struct FakeClock {
    uint64_t t = 0;
    std::function<uint64_t()> fn()
    {
        return [this] { return t; };
    }
};

static void enable_mfp_channel(Machine &m, int ch)
{
    uint32_t ier = ch >= 8 ? 0xE88007 : 0xE88009, imr = ch >= 8 ? 0xE88013 : 0xE88015;
    uint8_t bit = uint8_t(1 << (ch & 7));
    m.mem.write8(ier, uint8_t(m.mem.read8(ier) | bit));
    m.mem.write8(imr, uint8_t(m.mem.read8(imr) | bit));
}

// Cotton's display: 31 kHz, 568 lines, picture on lines 40-551 scanned twice, 384 dots wide.
static void crtc_cotton(Machine &m)
{
    m.mem.poke16(0xE80004, 12);   // R02 horizontal display start
    m.mem.poke16(0xE80006, 60);   // R03 horizontal display end
    m.mem.poke16(0xE80008, 567);  // R04 vertical total
    m.mem.poke16(0xE8000C, 40);   // R06 vertical display start
    m.mem.poke16(0xE8000E, 552);  // R07 vertical display end
    m.mem.poke16(0xE80028, 0x0111);
}

static void user_mode(Machine &m)
{
    m.ctx.sr_hi = 0x00;
    m.ctx.a[7] = 0x8000;      // USP
    m.ctx.other_sp = 0x6000;  // SSP
}

// ---- tests ----

// The TOML that Willow's own files use.
static void test_toml()
{
    std::string error;
    auto doc = parse_toml("title = \"a # not a comment\"  # a comment\n"
                          "[options]\n"
                          "speed = 0x1_0\n"
                          "on = true\n"
                          "names = [\n"
                          "  'one',  # first\n"
                          "  \"two\\n\",\n"
                          "]\n"
                          "[[patch]]\n"
                          "offset = -3\n"
                          "[[patch]]\n"
                          "offset = 1_000\n",
                          &error);
    CHECK(doc.has_value());
    if (!doc) return;
    CHECK(doc->root["title"].text == "a # not a comment");
    CHECK_EQ(doc->tables["options"]["speed"].number, 16);
    CHECK(doc->tables["options"]["on"].flag);
    const TomlValue &names = doc->tables["options"]["names"];
    CHECK_EQ(names.kind, TomlValue::Array);
    CHECK_EQ(names.items.size(), 2);
    CHECK(names.items.size() == 2 && names.items[0].text == "one" && names.items[1].text == "two\n");
    CHECK_EQ(doc->arrays["patch"].size(), 2);
    CHECK_EQ(doc->arrays["patch"][0]["offset"].number, uint64_t(-3));
    CHECK_EQ(doc->arrays["patch"][1]["offset"].number, 1000);

    CHECK(!parse_toml("a = 1\na = 2\n", &error) && error == "line 2: a appears twice");
    CHECK(!parse_toml("[x]\nb = \"open\n", &error) && error.rfind("line 2:", 0) == 0);
    CHECK(!parse_toml("c = maybe\n", &error) && error == "line 1: 'maybe' is not a value Willow reads");
}

// willow.toml: what isn't given keeps its default, and anything not understood is reported.
static void test_settings()
{
    std::vector<std::string> problems;
    Settings s = parse_settings(kDefaultSettings, problems);
    CHECK(problems.empty());
    CHECK_EQ(s.disk_speedup, Floppy::kSpeedup);
    CHECK(s.sprite_limit && !s.fullscreen && !s.smooth && s.mods.empty());

    s = parse_settings("[options]\nloading = \"instant\"\nsprite_limit = false\nsmooth = true\n"
                       "[mods]\nenabled = [\"english\", \"more-lives\"]\n",
                       problems);
    CHECK(problems.empty());
    CHECK_EQ(s.disk_speedup, 0);
    CHECK(!s.sprite_limit && s.smooth && !s.fullscreen);
    CHECK(s.mods == std::vector<std::string>({"english", "more-lives"}));

    s = parse_settings("[options]\nloading = \"slow\"\nfulscreen = true\nsmooth = 1\n", problems);
    CHECK_EQ(problems.size(), 3);  // a bad value, a typo, the wrong kind of value
    CHECK_EQ(s.disk_speedup, Floppy::kSpeedup);
    CHECK(!s.smooth);
}

// What the game prints is Shift-JIS; the log gets UTF-8.
static void test_sjis_text()
{
    CHECK(from_sjis("\x82\x62\x83\x6F.\r\n") == "\xEF\xBC\xA3\xE3\x83\x90.");  // full-width C, katakana ba
    CHECK(from_sjis("plain") == "plain");
}

static void test_memory()
{
    FakeClock clock;
    Machine m({}, clock.fn());
    m.mem.write32(0x1000, 0x11223344);
    CHECK_EQ(m.mem.peek8(0x1000), 0x11);
    CHECK_EQ(m.mem.read16(0x1002), 0x3344);
    m.mem.write8(0x01001000, 0x55);  // only 24 address bits
    CHECK_EQ(m.mem.peek8(0x1000), 0x55);
    CHECK_EQ(m.mem.read8(0xE9A001), 0xFF);          // idle joystick
    CHECK_EQ(m.mem.read8(0xE90003) & 0x80, 0);      // an idle OPM is not busy
    CHECK_EQ(m.mem.read8(0xE88017), 0x40);          // MFP vectors start at $40
    CHECK_EQ(m.mem.peek32(32 * 4), Machine::kRomVectors + 32 * 4);  // unclaimed trap #0
    m.load(reloc_program, 0x30000);
    CHECK_EQ(m.mem.peek32(0x30008), 0x30020);
    CHECK_EQ(reloc_base, 0x30000);
}

static void test_text_vram_access_modes()
{
    FakeClock clock;
    Machine m({}, clock.fn());
    m.mem.write16(0xE8002A, 0x01F3);  // R21: simultaneous access to all four planes
    m.mem.write8(0xE00010, 0xAA);
    for (uint32_t plane = 0; plane < 4; plane++) CHECK_EQ(m.mem.peek8(0xE00010 + plane * 0x20000), 0xAA);
    m.mem.write16(0xE8002A, 0x0033);  // back to plain access
    m.mem.write8(0xE00011, 0x55);
    CHECK_EQ(m.mem.peek8(0xE00011), 0x55);
    CHECK_EQ(m.mem.peek8(0xE20011), 0x00);
    m.mem.write16(0xE8002E, 0xF000);  // R23 mask: protect the high nibble of even bytes
    m.mem.write16(0xE8002A, 0x0200);  // R21: mask enabled
    m.mem.write8(0xE00020, 0xFF);
    CHECK_EQ(m.mem.peek8(0xE00020), 0x0F);
}

static void test_status_register_switches_stacks()
{
    FakeClock clock;
    Machine m({}, clock.fn());
    m.ctx.sr_hi = 0x27;
    m.ctx.a[7] = 0x6000;
    m.ctx.other_sp = 0x8000;
    m.set_sr(0x0000);
    CHECK_EQ(m.ctx.a[7], 0x8000);
    CHECK_EQ(m.ctx.other_sp, 0x6000);
    m.set_sr(0x2700);
    CHECK_EQ(m.ctx.a[7], 0x6000);
}

static void test_vsync_interrupt()
{
    FakeClock clock;
    Machine m({}, clock.fn());
    crtc_cotton(m);
    m.load(isr_program, 0x20000);
    m.mem.poke32(0x46 * 4, 0x20000);  // V-DISP is MFP channel 6, vector $46
    enable_mfp_channel(m, Mfp::kVdisp);
    user_mode(m);

    m.service(0x123456);  // nothing due yet
    CHECK_EQ(m.mem.peek16(0x5000), 0);
    clock.t = Machine::kFramePeriodNs + 1;
    m.service(0x123456);
    CHECK_EQ(m.mem.peek16(0x5000), 1);
    CHECK_EQ(m.frames(), 1);
    CHECK_EQ(m.ctx.ret, 0x123456);
    CHECK(!m.supervisor());  // rte went back to user mode and the user stack
    CHECK_EQ(m.ctx.a[7], 0x8000);
    CHECK_EQ(m.ctx.other_sp, 0x6000);
}

static void test_masked_interrupt_waits()
{
    FakeClock clock;
    Machine m({}, clock.fn());
    crtc_cotton(m);
    m.load(isr_program, 0x20000);
    m.mem.poke32(0x46 * 4, 0x20000);
    enable_mfp_channel(m, Mfp::kVdisp);
    m.ctx.sr_hi = 0x27;
    m.ctx.a[7] = 0x6000;
    clock.t = Machine::kFramePeriodNs + 1;
    m.service(0x1000);
    CHECK_EQ(m.mem.peek16(0x5000), 0);  // level 6 is masked at level 7
    m68k_pending = 0;
    m.set_sr(0x2000);
    CHECK_EQ(m68k_pending, 1);  // lowering the mask asks for delivery
    m.service(0x1000);
    CHECK_EQ(m.mem.peek16(0x5000), 1);
}

static void test_opm_timer_interrupt()
{
    FakeClock clock;
    Machine m({}, clock.fn());
    m.load(isr_program, 0x20000);
    user_mode(m);
    m.ctx.a[1] = 0x20010;
    m.iocs->call(0x6A);  // _OPMINTST
    CHECK_EQ(m.mem.peek32(0x43 * 4), 0x20010);
    m.opm.write_reg(0x12, 0xBF);  // timer B: 1024 * 65 / 4 MHz = 16.64 ms
    m.opm.write_reg(0x14, 0x3A);
    clock.t = 16'000'000;
    m.service(0x1000);
    CHECK_EQ(m.mem.peek16(0x5000), 0);
    clock.t = 16'700'000;
    m.service(0x1000);
    CHECK_EQ(m.mem.peek16(0x5000), 1);
    clock.t = 33'400'000;
    m.service(0x1000);
    CHECK_EQ(m.mem.peek16(0x5000), 2);  // the handler reset the flag, so the next tick interrupts again
}

static void test_opm_busy_after_data_write()
{
    FakeClock clock;
    Machine m({}, clock.fn());
    m.mem.write8(0xE90001, 0x20);
    CHECK_EQ(m.mem.read8(0xE90003) & 0x80, 0);  // writing the address doesn't
    m.mem.write8(0xE90003, 0xC7);
    CHECK_EQ(m.mem.read8(0xE90003) & 0x80, 0x80);
    clock.t = 15'900;
    CHECK_EQ(m.mem.read8(0xE90003) & 0x80, 0x80);
    clock.t = 16'000;  // 64 clocks at 4 MHz
    CHECK_EQ(m.mem.read8(0xE90003) & 0x80, 0);
}

// Without a clock to override it, time is the cycle count at 10 MHz.
static void test_time_follows_cycles()
{
    Machine m({});
    crtc_cotton(m);
    m.load(isr_program, 0x20000);
    m.mem.poke32(0x46 * 4, 0x20000);
    enable_mfp_channel(m, Mfp::kVdisp);
    user_mode(m);
    m.ctx.cycles = 100;
    m.service(0x1000);
    CHECK_EQ(m.now(), 100 * Machine::kNsPerCycle);
    CHECK_EQ(m.ctx.deadline, 100 + Machine::kSliceCycles);
    uint64_t vsync = Machine::kFramePeriodNs / 568 * 552 / Machine::kNsPerCycle;  // VSYNC starts at line 552
    m.ctx.cycles = vsync - 200;
    m.service(0x1000);
    CHECK_EQ(m.mem.peek16(0x5000), 0);
    m.ctx.cycles = vsync + 10;
    m.service(0x1000);
    CHECK_EQ(m.mem.peek16(0x5000), 1);
    CHECK_EQ(m.ctx.cycles, vsync + 10 + 44);  // taking the interrupt costs the 68000 44 cycles
}

// stop waits for an interrupt by letting time pass.
static void test_stop_idles_until_interrupt()
{
    Machine m({});
    crtc_cotton(m);
    m.load(isr_program, 0x20000);
    m.mem.poke32(0x46 * 4, 0x20000);
    enable_mfp_channel(m, Mfp::kVdisp);
    m.ctx.sr_hi = 0x27;
    m.ctx.a[7] = 0x6000;
    m.stop(0x2000, 0x1000);
    CHECK_EQ(m.mem.peek16(0x5000), 1);
    CHECK_EQ(m.ctx.ret, 0x1000);
    uint64_t vsync = Machine::kFramePeriodNs / 568 * 552 / Machine::kNsPerCycle;
    CHECK(m.ctx.cycles >= vsync && m.ctx.cycles < vsync + 2 * Machine::kSliceCycles);
}

static void test_raster_interrupt()
{
    FakeClock clock;
    Machine m({}, clock.fn());
    crtc_cotton(m);
    m.mem.poke16(0xE80012, 100);       // R09: interrupt at raster line 100
    m.load(isr_program, 0x20000);
    m.mem.poke32(0x4E * 4, 0x20000);  // CRTC raster is MFP channel 14, vector $4E
    enable_mfp_channel(m, Mfp::kRaster);
    user_mode(m);
    uint64_t line = Machine::kFramePeriodNs / 568;
    clock.t = 99 * line;
    m.service(0x1000);
    CHECK_EQ(m.mem.peek16(0x5000), 0);
    clock.t = 101 * line;
    m.service(0x1000);
    CHECK_EQ(m.mem.peek16(0x5000), 1);
    clock.t = 300 * line;
    m.service(0x1000);
    CHECK_EQ(m.mem.peek16(0x5000), 1);  // once per frame
    clock.t = Machine::kFramePeriodNs + 101 * line;
    m.service(0x1000);
    CHECK_EQ(m.mem.peek16(0x5000), 2);
}

// The backdrop is the first text palette entry: red from VSYNC, blue from a raster split.
static void backdrop_red(M68kContext *c)
{
    m68k_write16(0xE82200, 0x07C0);
    rte(c);
}
static void backdrop_blue(M68kContext *c)
{
    m68k_write16(0xE82200, 0x003E);
    rte(c);
}
static uint32_t split_base;
static const M68kFuncEntry split_funcs[] = {{0x00, backdrop_red}, {0x10, backdrop_blue}};
static const M68kProgram split_program = {"split.r", blank_image, sizeof blank_image, 0, 0, nullptr, 0, split_funcs, 2, &split_base};

static void test_raster_split_drawn_line_by_line()
{
    FakeClock clock;
    Machine m({}, clock.fn());
    crtc_cotton(m);
    m.mem.poke16(0xE80012, 140);  // raster line 140 = display line (140 - 40) / 2 = 50
    m.load(split_program, 0x20000);
    m.mem.poke32(0x46 * 4, 0x20000);
    m.mem.poke32(0x4E * 4, 0x20010);
    enable_mfp_channel(m, Mfp::kVdisp);
    enable_mfp_channel(m, Mfp::kRaster);
    user_mode(m);
    for (uint64_t t = 0; t <= 2 * Machine::kFramePeriodNs + 1; t += 200'000) {  // service every 0.2 ms
        clock.t = t;
        m.service(0x1000);
    }
    std::vector<uint8_t> rgb;
    int w = 0, h = 0;
    float aspect = 0;
    CHECK(m.video.latest(rgb, w, h, aspect));
    CHECK_EQ(w, 384);
    CHECK_EQ(h, 256);
    CHECK(aspect > 0.99f && aspect < 1.01f);  // Cotton's picture is square on the monitor
    auto pixel = [&](int x, int y) { return uint32_t(rgb[(size_t(y) * w + x) * 3]) << 16 | uint32_t(rgb[(size_t(y) * w + x) * 3 + 2]); };
    CHECK_EQ(pixel(10, 49), 0xFA0000);  // red above the split
    CHECK_EQ(pixel(10, 50), 0x0000FA);  // blue from the split down
    CHECK_EQ(pixel(10, 255), 0x0000FA);
}

// Writing the contrast starts a fade the hardware runs by itself: a third of a level per 1/60 s.
// At most 32 sprites show on a line, the lowest-numbered first. Hidden ones (priority 0)
// don't count; ones off the side of the picture do.
static void test_sprites_per_line()
{
    FakeClock clock;
    Machine m({}, clock.fn());
    crtc_cotton(m);
    m.mem.poke16(0xE82600, 0x0040);  // VC R2: sprite layer on
    m.mem.poke16(0xEB0808, 0x0200);  // sprites shown, no BG
    for (uint32_t i = 0; i < 128; i++) m.mem.poke8(0xEB8000 + 128 + i, 0x11);  // pattern 1: solid color 1
    m.mem.poke16(0xE82200 + 17 * 2, 0xFFFE);                                 // palette block 1, color 1: white
    auto sprite = [&](int n, int x, int y, int priority) {
        uint32_t s = 0xEB0000 + uint32_t(n) * 8;
        m.mem.poke16(s, uint16_t(x + 16));
        m.mem.poke16(s + 2, uint16_t(y + 16));
        m.mem.poke16(s + 4, 0x0101);  // palette block 1, pattern 1
        m.mem.poke16(s + 6, uint16_t(priority));
    };
    sprite(0, 100, 10, 0);                          // hidden: doesn't count
    for (int n = 1; n < 32; n++) sprite(n, -16, 10, 3);  // off the left edge: 31 that count
    sprite(32, 100, 10, 3);                         // the 32nd: shown
    sprite(33, 200, 10, 3);                         // the 33rd: dropped
    sprite(34, 200, 40, 3);                         // alone on its lines: shown
    const std::vector<uint8_t> &rgb = m.video.render();
    auto lit = [&](int x, int y) { return rgb[(size_t(y) * 384 + size_t(x)) * 3] != 0; };
    CHECK(lit(100, 10));
    CHECK(!lit(200, 10));
    CHECK(lit(200, 40));
}

// VC R2 special priority: with B/P on, a graphic pixel with an odd palette code of 3 or more
// shows in front of the text, in the color of the code below it; code 1 turns transparent.
static void test_special_priority()
{
    FakeClock clock;
    Machine m({}, clock.fn());
    crtc_cotton(m);
    m.mem.poke16(0xE82400, 0x0000);  // VC R0: 16 colors
    m.mem.poke16(0xE82500, 0x12E4);  // VC R1: text in front, then sprites, then graphics
    m.mem.poke16(0xE82000 + 2 * 2, 0x07C1);  // graphic palette 2: red
    m.mem.poke16(0xE82000 + 3 * 2, 0xF801);  // 3: green, never shown as such
    m.mem.poke16(0xE82000 + 4 * 2, 0x003F);  // 4: blue
    m.mem.poke16(0xE82200 + 1 * 2, 0xFFFF);  // text palette 1: white
    m.mem.poke8(0xE00000, 0xC0);             // text color 1 at x = 0 and 1
    const uint16_t codes[4] = {3, 4, 1, 4};
    for (uint32_t x = 0; x < 4; x++) m.mem.poke16(0xC00000 + x * 2, codes[x]);
    auto pixel = [&](int x) {
        const std::vector<uint8_t> &rgb = m.video.render();
        return uint32_t(rgb[size_t(x) * 3] << 16 | rgb[size_t(x) * 3 + 1] << 8 | rgb[size_t(x) * 3 + 2]);
    };
    m.mem.poke16(0xE82600, 0x0021);  // text and graphic page 0 on, no special priority
    CHECK_EQ(pixel(0), 0xFFFFFF);    // the text is in front
    CHECK_EQ(pixel(2), 0x000000);    // code 1 is not drawn here: palette 1 is black
    m.mem.poke16(0xE82600, 0x1421);  // EXON and B/P on
    CHECK_EQ(pixel(0), 0xFF0404);    // code 3 is marked: in front of the text, as code 2 (red)
    CHECK_EQ(pixel(1), 0xFFFFFF);    // code 4 is not: the text stays in front
    CHECK_EQ(pixel(2), 0x000000);    // code 1 loses its low bit: transparent
    CHECK_EQ(pixel(3), 0x0404FF);    // code 4 (blue) where there's no text
}

// VC R2 translucency between graphic pages, marked by palette code (Cotton's stage 1): an odd
// code of 3 or more on the front page is mixed with the back page's code made odd.
static void test_translucency()
{
    FakeClock clock;
    Machine m({}, clock.fn());
    crtc_cotton(m);
    m.mem.poke16(0xE82400, 0x0000);          // VC R0: 16 colors
    m.mem.poke16(0xE82500, 0x12E4);          // VC R1: page 0 in front of page 1
    m.mem.poke16(0xE82600, 0x1E03);          // VC R2: translucency by code between pages; pages 0 and 1 on
    m.mem.poke16(0xE82000 + 2 * 2, 0x07C1);  // graphic palette 2: red
    m.mem.poke16(0xE82000 + 4 * 2, 0xF801);  // 4: green
    m.mem.poke16(0xE82000 + 5 * 2, 0x003F);  // 5: blue
    const uint16_t front[4] = {3, 2, 1, 0}, back[4] = {4, 4, 4, 3};
    for (uint32_t x = 0; x < 4; x++) {
        m.mem.poke16(0xC00000 + x * 2, front[x]);
        m.mem.poke16(0xC80000 + x * 2, back[x]);
    }
    const std::vector<uint8_t> &rgb = m.video.render();
    auto pixel = [&](int x) { return uint32_t(rgb[size_t(x) * 3] << 16 | rgb[size_t(x) * 3 + 1] << 8 | rgb[size_t(x) * 3 + 2]); };
    CHECK_EQ(pixel(0), 0x7D047D);  // 3 counts as 2 (red), mixed with 4 made odd (5, blue): G, R, B averaged
    CHECK_EQ(pixel(1), 0xFF0404);  // 2 isn't marked: red
    CHECK_EQ(pixel(2), 0x04FF04);  // 1 loses its low bit, so the back page shows: green
    CHECK_EQ(pixel(3), 0xFF0404);  // 3 on the back page isn't marked; it counts as 2
}

static void test_contrast_fades_gradually()
{
    FakeClock clock;
    Machine m({}, clock.fn());
    crtc_cotton(m);
    m.mem.poke16(0xE82200, 0x07C0);  // red backdrop
    auto red = [&] { return m.video.render()[(10 * 384 + 10) * 3]; };
    uint8_t full = red();
    CHECK(full > 0);
    const uint64_t step = Video::kContrastStepNs;
    m.mem.write8(0xE8E001, 0);
    m.video.update_contrast(0);
    CHECK_EQ(red(), full);  // nothing yet
    m.video.update_contrast(step);
    CHECK_EQ(red(), full * 44 / 45);
    m.video.update_contrast(15 * step);
    CHECK_EQ(red(), full * 30 / 45);  // 10 of 15 levels after a quarter second
    m.video.update_contrast(45 * step - 1);
    CHECK_EQ(red(), full * 1 / 45);
    m.video.update_contrast(45 * step);
    CHECK_EQ(red(), 0);  // dark after 0.75 s
    m.mem.write8(0xE8E001, 15);
    m.video.update_contrast(50 * step);
    CHECK_EQ(red(), 0);  // and back up, one step per 1/60 s from the write
    m.video.update_contrast(51 * step);
    CHECK_EQ(red(), full * 1 / 45);
}

static void test_dma_array_chaining()
{
    FakeClock clock;
    Machine m({}, clock.fn());
    // Two chain entries at $9000: 2 bytes from $9100, then 1 byte from $9200.
    m.mem.poke32(0x9000, 0x9100), m.mem.poke16(0x9004, 2);
    m.mem.poke32(0x9006, 0x9200), m.mem.poke16(0x900A, 1);
    m.mem.poke8(0x9100, 0x11), m.mem.poke8(0x9101, 0x22), m.mem.poke8(0x9200, 0x33);
    m.mem.write8(0xE840C5, 0x3A);  // OCR: memory to device, byte, array chaining
    m.mem.write32(0xE840DC, 0x9000);
    m.mem.write16(0xE840DA, 2);
    m.mem.write8(0xE840C7, 0x88);  // start, interrupt when done
    uint8_t b = 0;
    CHECK(m.dmac.fetch(b) && b == 0x11);
    CHECK(m.dmac.fetch(b) && b == 0x22);
    CHECK(!m.dmac.irq_pending());
    CHECK(m.dmac.fetch(b) && b == 0x33);
    CHECK(m.dmac.irq_pending());
    CHECK_EQ(m.mem.read8(0xE840C0) & 0x88, 0x80);  // complete, no longer active
    CHECK(!m.dmac.fetch(b));
    CHECK_EQ(m.dmac.acknowledge(), 0x6A);
    m.mem.write8(0xE840C0, 0xFF);  // writing 1s clears the status
    CHECK_EQ(m.mem.read8(0xE840C0), 0);
}

static void test_adpcm_decoding()
{
    FakeClock clock;
    Machine m({}, clock.fn());
    m.opm.write_reg(0x1B, 0x80);   // CT1: 4 MHz
    m.mem.write8(0xE9A005, 0x08);  // PPI port C: divide by 512, both outputs on
    CHECK(m.adpcm.rate() > 7812.4 && m.adpcm.rate() < 7812.6);
    CHECK(m.adpcm.left_on() && m.adpcm.right_on());
    m.mem.write8(0xE92003, 0x77);  // no DMA running: the chip plays the data register
    m.mem.write8(0xE92001, 0x02);
    CHECK_EQ(m.adpcm.next(), 30);  // step 16: 2 + 4 + 8 + 16
    CHECK_EQ(m.adpcm.next(), 93);  // step 34 after +8: 30 + 4 + 8 + 17 + 34
    m.mem.write8(0xE92001, 0x01);
    CHECK_EQ(m.adpcm.next(), 0);   // stopped
}

static void test_adpcm_dma_interrupt()
{
    FakeClock clock;
    Machine m({}, clock.fn());
    m.load(isr_program, 0x20000);
    m.mem.poke32(0x6A * 4, 0x20000);  // DMA channel 3 done: vector $6A, level 3
    user_mode(m);
    size_t frames = 0;
    m.sound.sink = [&](const int16_t *, size_t n) { frames += n; };
    m.opm.write_reg(0x1B, 0x80);
    m.mem.write8(0xE9A005, 0x08);  // 7812.5 Hz: each byte is two samples, 256 us
    m.mem.poke32(0x9000, 0x9100), m.mem.poke16(0x9004, 4);  // one 4-byte entry: about 1 ms
    m.mem.write8(0xE840C5, 0x3A);
    m.mem.write32(0xE840DC, 0x9000);
    m.mem.write16(0xE840DA, 1);
    m.mem.write8(0xE840C7, 0x88);
    m.mem.write8(0xE92001, 0x02);
    clock.t = 500'000;
    m.service(0x1000);
    CHECK_EQ(m.mem.peek16(0x5000), 0);  // still playing
    clock.t = 1'500'000;
    m.service(0x1000);
    CHECK_EQ(m.mem.peek16(0x5000), 1);  // the transfer finished and interrupted
    CHECK_EQ(frames, 1'500'000ull * 62500 / 1'000'000'000);
}

static void test_note_restarts_within_one_tick()
{
    FakeClock clock;
    Machine m({}, clock.fn());
    std::vector<int16_t> out;
    m.sound.sink = [&](const int16_t *s, size_t n) { out.insert(out.end(), s, s + n * 2); };
    auto loudest = [&](size_t from) {
        int peak = 0;
        for (size_t i = from; i < out.size(); i++) peak = std::max(peak, std::abs(int(out[i])));
        return peak;
    };
    // Channel 0, all four operators audible, an instant attack that decays to silence in a few ms.
    m.opm.write_reg(0x20, 0xC7);
    m.opm.write_reg(0x28, 0x4A);
    for (uint8_t op = 0; op < 32; op += 8) {
        m.opm.write_reg(uint8_t(0x40 + op), 0x01);
        m.opm.write_reg(uint8_t(0x60 + op), 0x00);
        m.opm.write_reg(uint8_t(0x80 + op), 0x1F);
        m.opm.write_reg(uint8_t(0xA0 + op), 0x1F);
        m.opm.write_reg(uint8_t(0xC0 + op), 0x00);
        m.opm.write_reg(uint8_t(0xE0 + op), 0xFF);
    }
    m.opm.write_reg(0x08, 0x78);  // key on
    clock.t = 64'000'000;
    m.service(0x1000);
    CHECK(loudest(0) > 1000);            // it sounded
    CHECK(loudest(out.size() - 400) < 50);  // and died away
    size_t mark = out.size();
    m.opm.write_reg(0x08, 0x00);  // key off and straight back on, in the same instant,
    m.opm.write_reg(0x08, 0x78);  // as the sound driver restarts every note
    clock.t = 72'000'000;
    m.service(0x1000);
    CHECK(loudest(mark) > 1000);  // the note started again
}

// Reads the key code from the MFP and appends it to a list at $5100: what the game's handler does.
static void key_isr(M68kContext *c)
{
    uint16_t n = m68k_read16(0x5000);
    m68k_write8(0x5100 + n, m68k_read8(0xE8802F));
    m68k_write16(0x5000, uint16_t(n + 1));
    rte(c);
}
static uint32_t key_base;
static const M68kFuncEntry key_funcs[] = {{0x00, key_isr}};
static const M68kProgram key_program = {"key.r", blank_image, sizeof blank_image, 0, 0, nullptr, 0, key_funcs, 1, &key_base};

static void test_keyboard_and_joystick()
{
    FakeClock clock;
    Machine m({}, clock.fn());
    m.load(key_program, 0x20000);
    m.mem.poke32(0x4C * 4, 0x20000);  // MFP receive buffer full: channel 12, vector $4C
    enable_mfp_channel(m, Mfp::kReceive);
    user_mode(m);
    m.input.key(0x55, true);  // XF1 down, then up
    m.input.key(0x55, false);
    clock.t = 1'000'000;
    m.service(0x1000);
    CHECK_EQ(m.mem.peek16(0x5000), 1);
    CHECK_EQ(m.mem.peek8(0x5100), 0x55);
    CHECK_EQ(m.mem.read8(0xE8802B) & 0x80, 0);  // reading the data register emptied the buffer
    clock.t = 2'000'000;
    m.service(0x1000);
    CHECK_EQ(m.mem.peek16(0x5000), 1);  // the next code waits for the line: about 4 ms
    clock.t = 6'000'000;
    m.service(0x1000);
    CHECK_EQ(m.mem.peek16(0x5000), 2);
    CHECK_EQ(m.mem.peek8(0x5101), 0xD5);  // release: bit 7 set
    m.input.key(0x80, true);              // not a key code: ignored
    m.input.joystick(0, Input::kUp | Input::kA);
    clock.t = 7'000'000;
    m.service(0x1000);
    CHECK_EQ(m.mem.read8(0xE9A001), 0xDE);  // up and trigger A pull their lines low
    CHECK_EQ(m.mem.read8(0xE9A003), 0xFF);  // nothing on port B
}

static void test_memory_blocks()
{
    FakeClock clock;
    Machine m({}, clock.fn());
    m.dos->init(0x10000, 0x40000);
    uint32_t p = m.dos->malloc(0x1000, m.dos->current());
    CHECK_EQ(p, 0x10110);
    CHECK_EQ(m.mem.peek32(0x10100 + 8), 0x11110);   // the block header's end
    CHECK_EQ(m.mem.peek32(0x10000 + 0xC), 0x10100); // the shell's block links to it
    uint32_t too_big = m.dos->malloc(0xFFFFFF, m.dos->current());
    CHECK_EQ(too_big >> 24, 0x81);
    CHECK_EQ(too_big & 0xFFFFFF, 0x40000 - 0x11110 - 0x10);
    CHECK_EQ(m.dos->setblock(p, 0x2000), 0);
    CHECK_EQ(m.mem.peek32(0x10100 + 8), 0x12110);
    CHECK_EQ(m.dos->mfree(p), 0);
    CHECK_EQ(m.dos->mfree(p), -9);
}

static void test_exec_keeppr_and_wait()
{
    FakeClock clock;
    Machine m({}, clock.fn());
    m.add_program(&keep_program);
    m.dos->init(0x10000, 0x40000);
    user_mode(m);
    m.ctx.d[1] = 0x1234;
    m.ctx.cycles = 1000;
    int32_t code = m.dos->exec("A:\\KEEP.R", 0, 0);
    CHECK_EQ(code, 7);
    CHECK_EQ(m.ctx.d[1], 0x1234);              // the parent's registers come back
    CHECK_EQ(child_cycles, 1000);              // but time carries on through both switches
    CHECK_EQ(m.ctx.cycles, 1500);
    CHECK_EQ(m.dos->current(), 0x10000);
    CHECK_EQ(child_regs[0], 0x10100);          // a0: the child's block
    CHECK_EQ(child_regs[4], 0x10200 + 0x04);   // a4: its entry point
    CHECK_EQ(child_regs[1], 0x10200 + 64 + 0x20);  // a1: end of image + bss
    CHECK_EQ(m.mem.peek32(0x10100 + 8), 0x10200 + 0x40);  // shrunk to what it kept
    m.dos->call(0x4D, 0);                      // _WAIT
    CHECK_EQ(m.ctx.d[0], 0x10007);
}

static void test_exec_exit_frees_memory()
{
    FakeClock clock;
    Machine m({}, clock.fn());
    m.add_program(&exit_program);
    m.dos->init(0x10000, 0x40000);
    user_mode(m);
    uint32_t before = m.dos->malloc(0xFFFFFF, m.dos->current());
    CHECK_EQ(m.dos->exec("exit.r", 0, 0), 0);
    CHECK_EQ(m.dos->malloc(0xFFFFFF, m.dos->current()), before);
    CHECK_EQ(m.dos->exec("missing.r", 0, 0), err::kFileNotFound);
    bool threw = false;
    try {
        m.call(0x123456);
    } catch (const Fatal &) {
        threw = true;
    }
    CHECK(threw);
}

static void write_file(const fs::path &p, const std::string &content)
{
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << content;
}

static void test_filesystem()
{
    fs::path root = fs::temp_directory_path() / "willow-runtime-test";
    fs::remove_all(root);
    write_file(root / "system" / "cotsys", "");
    write_file(root / "data" / "user" / "cotsys.dat", "HELLO");
    write_file(root / "data" / "cotdat", "");

    FakeClock clock;
    Options options;
    options.data_dir = root;
    options.save_dir = root / "saves";
    Machine m(options, clock.fn());
    FileSystem &f = *m.fs;

    int32_t h = f.open("b:USER\\CotSys.dat", 0, 1);
    CHECK(h >= 5);
    CHECK_EQ(f.read(h, 0x9000, 16), 5);
    CHECK(m.mem.cstring(0x9000, 5) == "HELLO");
    CHECK_EQ(f.close(h), 0);
    CHECK_EQ(f.close(h), err::kBadHandle);

    CHECK_EQ(f.files(0xA000, "b:\\user", 0x10), 0);
    CHECK(m.mem.cstring(0xA000 + 30) == "user");
    CHECK_EQ(m.mem.peek8(0xA000 + 21), 0x10);
    CHECK_EQ(f.files(0xA000, "a:\\cotsys", 0x20), 0);
    CHECK_EQ(f.files(0xA000, "a:\\cotdat", 0x20), err::kFileNotFound);
    CHECK_EQ(f.drvctrl(0, 1), 0x02);
    CHECK_EQ(f.drvctrl(0, 3), 0x04);

    CHECK_EQ(f.drvxchg(2, 1), 0);  // what bootcot.r does when the disks are the other way round
    CHECK_EQ(f.files(0xA000, "a:\\cotdat", 0x20), 0);

    h = f.open("a:user\\cotsys.dat", 2, 1);  // writing copies the file into the overlay first
    CHECK(h >= 5);
    m.mem.poke8(0x9000, 'J');
    CHECK_EQ(f.write(h, 0x9000, 1), 1);
    f.close(h);
    {
        std::ifstream original(root / "data" / "user" / "cotsys.dat");
        std::string untouched((std::istreambuf_iterator<char>(original)), {});
        CHECK(untouched == "HELLO");
    }  // closed before the folder is removed: Windows won't delete open files
    h = f.open("a:user\\cotsys.dat", 0, 1);
    f.read(h, 0x9100, 5);
    f.close(h);
    CHECK(m.mem.cstring(0x9100, 5) == "JELLO");

    h = f.create("a:user\\new.dat", 0x20, 1);
    CHECK(h >= 5);
    f.close(h);
    CHECK(fs::exists(root / "saves" / "data" / "user" / "new.dat"));  // keyed by disk, not drive letter
    CHECK(!fs::exists(root / "data" / "user" / "new.dat"));
    CHECK_EQ(f.chdir("a:\\user"), 0);
    CHECK_EQ(f.files(0xA000, "a:cotsys.dat", 0x20), 0);
    fs::remove_all(root);
}

// Drop-in mods: their files replace the disks', the last enabled mod winning; writes still go
// to the overlay; programs load from the disk only.
static void test_mods()
{
    fs::path root = fs::temp_directory_path() / "willow-mods-test";
    fs::remove_all(root);
    write_file(root / "disks" / "system" / "cot.r", std::string(100, 'p'));
    write_file(root / "disks" / "data" / "obj" / "big.dat", "disk");
    write_file(root / "disks" / "data" / "obj" / "other.dat", "other");
    fs::path mods = root / "mods";
    write_file(mods / "good" / "mod.toml", "name = \"Good\"\nversion = \"1.0\"\nauthors = [\"Someone\"]\ndescription = \"A test.\"\n");
    write_file(mods / "good" / "files" / "data" / "obj" / "big.dat", "good");
    write_file(mods / "good" / "files" / "data" / "obj" / "new.dat", "new!");
    write_file(mods / "good" / "files" / "system" / "COT.R", "not a program");  // programs can't be replaced
    write_file(mods / "good" / "files" / "extra" / "x.dat", "x");                  // not a disk
    write_file(mods / "second" / "mod.toml", "name = \"Second\"\ncolour = \"blue\"\n");
    write_file(mods / "second" / "files" / "DATA" / "OBJ" / "BIG.DAT", "second");  // names in any case
    fs::create_directories(mods / "broken");                                       // no mod.toml

    std::vector<std::string> problems;
    std::vector<Mod> loaded = load_mods(mods, {"good", "missing", "broken", "second", "good"}, {"cot.r", "ealogo.r"}, problems);
    CHECK_EQ(loaded.size(), 2);
    if (loaded.size() != 2) return;
    CHECK(loaded[0].id == "good" && loaded[1].id == "second");
    CHECK(loaded[0].name == "Good" && loaded[0].version == "1.0" && loaded[0].authors == std::vector<std::string>{"Someone"});
    CHECK_EQ(problems.size(), 6);  // COT.R, files/extra, missing, broken, colour, good twice

    FakeClock clock;
    Options options;
    options.data_dir = root / "disks";
    options.save_dir = root / "saves";
    options.mods = loaded;
    Machine m(options, clock.fn());
    FileSystem &f = *m.fs;
    auto read_all = [&](const char *path) {
        int32_t h = f.open(path, 0, 1);
        if (h < 5) return std::string("(missing)");
        int32_t n = f.read(h, 0x9000, 64);
        f.close(h);
        return m.mem.cstring(0x9000, size_t(n));
    };
    CHECK(read_all("b:obj\\big.dat") == "second");   // the last enabled mod wins
    CHECK(read_all("b:obj\\new.dat") == "new!");     // a mod can add files
    CHECK(read_all("b:obj\\other.dat") == "other");  // the rest come from the disk
    CHECK_EQ(f.files(0xA000, "b:obj\\big.dat", 0x20), 0);
    CHECK_EQ(m.mem.peek32(0xA000 + 26), 6);          // listed at the mod's size
    CHECK_EQ(f.charge_load("a:\\cot.r"), 100);       // the program on the disk, not the mod's

    int32_t h = f.open("b:obj\\big.dat", 2, 1);      // a write starts from the mod's copy
    m.mem.poke8(0x9000, 'S');
    CHECK_EQ(f.write(h, 0x9000, 1), 1);
    f.close(h);
    CHECK(read_all("b:obj\\big.dat") == "Second");
    {
        std::ifstream mod_file(mods / "second" / "files" / "DATA" / "OBJ" / "BIG.DAT");
        std::string untouched((std::istreambuf_iterator<char>(mod_file)), {});
        CHECK(untouched == "second");
    }  // closed before the folder is removed
    fs::remove_all(root);
}

// File calls take the floppy drive's time, and interrupts are taken while Human68k waits.
static void test_disk_time()
{
    fs::path root = fs::temp_directory_path() / "willow-disk-test";
    fs::remove_all(root);
    write_file(root / "system" / "cotsys", "");
    write_file(root / "data" / "obj" / "big.dat", std::string(32768, 'x'));

    Options options;
    options.data_dir = root;
    Machine m(options);
    FileSystem &f = *m.fs;
    const uint64_t turn = Floppy::kTurnNs, speedup = Floppy::kSpeedup, directory = Floppy::transfer_ns(1024) / speedup;
    CHECK_EQ(Floppy::transfer_ns(1024), turn / 2 + 3 * turn / 16);  // half a turn to come round, then one sector
    int32_t h = f.open("b:obj\\big.dat", 0, 1);
    CHECK(h >= 5);
    CHECK_EQ(f.take_disk_time(), 2 * directory);  // the root, then obj
    CHECK_EQ(f.take_disk_time(), 0);
    f.close(h);
    h = f.open("b:\\obj\\BIG.DAT", 0, 1);
    CHECK_EQ(f.take_disk_time(), 0);  // both directories are buffered now

    // A _READ of two cylinders: half a turn, then three turns a cylinder, sped up. The VSYNC
    // handler runs meanwhile, once every 18 ms.
    crtc_cotton(m);
    m.load(isr_program, 0x20000);
    m.mem.poke32(0x46 * 4, 0x20000);
    enable_mfp_channel(m, Mfp::kVdisp);
    user_mode(m);
    m.ctx.a[7] -= 10;
    m.mem.poke16(m.ctx.a[7], uint16_t(h));
    m.mem.poke32(m.ctx.a[7] + 2, 0x40000);
    m.mem.poke32(m.ctx.a[7] + 6, 32768);
    m.dos->call(0x3F, 0x1234);
    CHECK_EQ(m.ctx.d[0], 32768);
    CHECK_EQ(m.mem.peek8(0x40000 + 32767), 'x');
    uint64_t wait = (turn / 2 + 6 * turn) / speedup, frames = wait / Machine::kFramePeriodNs;
    CHECK_EQ(m.now(), wait / Machine::kNsPerCycle * Machine::kNsPerCycle);  // the handlers ran within it
    CHECK_EQ(m.mem.peek16(0x5000), frames);

    // The player's loading setting: "instant" charges nothing, "real" the drive's own time.
    f.seek(h, 0, 0);
    f.set_speedup(0);
    f.read(h, 0x40000, 1024);
    CHECK_EQ(f.take_disk_time(), 0);
    f.set_speedup(1);
    f.read(h, 0x40000, 1024);
    CHECK_EQ(f.take_disk_time(), Floppy::transfer_ns(1024));
    f.close(h);
    fs::remove_all(root);
}

// _EXEC reads the program file off the disk, then a packed program spends its unpacker's time.
static uint32_t packed_base;
static const M68kProgram packed_program = {"packed.r", blank_image, sizeof blank_image, 0, 0, nullptr, 0,
                                           exit_funcs, 1, &packed_base, 50'000};

static void test_exec_takes_load_and_unpack_time()
{
    fs::path root = fs::temp_directory_path() / "willow-exec-test";
    fs::remove_all(root);
    write_file(root / "system" / "packed.r", std::string(16384, 'p'));

    Options options;
    options.data_dir = root;
    Machine m(options);
    m.add_program(&packed_program);
    m.dos->init(0x10000, 0x40000);
    user_mode(m);
    CHECK_EQ(m.dos->exec("A:\\packed.r", 0, 0), 0);
    // The root directory, then the file, each at the default speed.
    uint64_t disk = Floppy::transfer_ns(1024) / Floppy::kSpeedup + Floppy::transfer_ns(16384) / Floppy::kSpeedup;
    CHECK_EQ(m.ctx.cycles, disk / Machine::kNsPerCycle + 50'000);
    fs::remove_all(root);
}

// Boot with stand-ins: bootsnd returns, the logo exits, the shell exits.
static int boot_order = 0, bootsnd_seen = 0, logo_seen = 0, shell_seen = 0;
static void fake_bootsnd(M68kContext *c)
{
    bootsnd_seen = ++boot_order;
    c->ret = m68k_pop32(c);
}
static void fake_logo(M68kContext *c)
{
    logo_seen = ++boot_order;
    m68k_line_f(c, 0xFF00, 0);
}
static void fake_shell(M68kContext *c)
{
    shell_seen = ++boot_order;
    m68k_line_f(c, 0xFF00, 0);
}
static uint32_t b1, b2, b3;
static const M68kFuncEntry f1[] = {{0, fake_bootsnd}}, f2[] = {{0, fake_logo}}, f3[] = {{0, fake_shell}};
static const M68kProgram p1 = {"bootsnd.sys", blank_image, 64, 0, 0, nullptr, 0, f1, 1, &b1};
static const M68kProgram p2 = {"ealogo.r", blank_image, 64, 0, 0, nullptr, 0, f2, 1, &b2};
static const M68kProgram p3 = {"bootcot.r", blank_image, 64, 0, 0, nullptr, 0, f3, 1, &b3};

static void test_boot_chain()
{
    FakeClock clock;
    Machine m({}, clock.fn());
    for (const M68kProgram *p : {&p1, &p2, &p3}) m.add_program(p);
    boot(m);
    CHECK_EQ(bootsnd_seen, 1);
    CHECK_EQ(logo_seen, 2);
    CHECK_EQ(shell_seen, 3);
    CHECK_EQ(b1, 0x3000);  // where the boot loader puts bootsnd.sys
    CHECK(m.supervisor());
    CHECK_EQ(m.ctx.sr_hi & 7, 0);  // interrupts enabled once Human68k is up
}

int main()
{
    test_toml();
    test_settings();
    test_sjis_text();
    test_memory();
    test_text_vram_access_modes();
    test_status_register_switches_stacks();
    test_vsync_interrupt();
    test_masked_interrupt_waits();
    test_opm_timer_interrupt();
    test_opm_busy_after_data_write();
    test_time_follows_cycles();
    test_stop_idles_until_interrupt();
    test_raster_interrupt();
    test_raster_split_drawn_line_by_line();
    test_contrast_fades_gradually();
    test_sprites_per_line();
    test_special_priority();
    test_translucency();
    test_dma_array_chaining();
    test_adpcm_decoding();
    test_adpcm_dma_interrupt();
    test_note_restarts_within_one_tick();
    test_keyboard_and_joystick();
    test_memory_blocks();
    test_exec_keeppr_and_wait();
    test_exec_exit_frees_memory();
    test_filesystem();
    test_mods();
    test_disk_time();
    test_exec_takes_load_and_unpack_time();
    test_boot_chain();
    std::printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
