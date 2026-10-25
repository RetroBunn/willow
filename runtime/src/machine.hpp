// The emulated machine as recompiled code sees it: one CPU context, the address
// space, the devices, and the programs loaded into memory.
//
// Game code runs on a single thread. Interrupts are delivered when that code
// polls m68k_pending (at loop back-edges and function entries): service()
// advances the devices to the current time and runs any handler that is due,
// through the vector table in emulated memory, exactly as the hardware would.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "devices.hpp"
#include "input.hpp"
#include "memory.hpp"
#include "sound.hpp"
#include "video.hpp"
#include "willow/m68k.h"

namespace willow {

class Human68k;
class Iocs;
class FileSystem;

struct Options {
    uint32_t ram_size = 4u << 20;
    std::filesystem::path data_dir;  // extracted disks: data_dir/system and data_dir/data
    std::filesystem::path save_dir;  // where writes go (scores, replays)
};

class Machine {
public:
    // Vectors nobody has claimed point into ROM; the runtime handles those itself.
    static constexpr uint32_t kRomVectors = 0xFE0000;  // exception vector n -> kRomVectors + 4n
    static constexpr uint32_t kRomIocs = 0xFF0000;     // IOCS call n -> kRomIocs + 4n
    static constexpr uint32_t kIocsTable = 0x400;
    static constexpr uint64_t kFramePeriodNs = 18'029'000;  // 55.46 Hz, the 31 kHz 512-line mode

    explicit Machine(Options options, std::function<uint64_t()> clock = {});
    ~Machine();
    static Machine &get();

    M68kContext ctx{};
    Memory mem;
    Mfp mfp;
    Opm opm;
    Ppi ppi;
    Video video{mem};
    Dmac dmac{mem};
    Adpcm adpcm{opm, ppi, dmac};
    Sound sound{adpcm};
    Input input;
    std::unique_ptr<FileSystem> fs;
    std::unique_ptr<Human68k> dos;
    std::unique_ptr<Iocs> iocs;

    // Programs: those linked in, and those currently loaded into memory.
    void add_program(const M68kProgram *program);
    const M68kProgram *program(const std::string &file) const;
    void load(const M68kProgram &program, uint32_t base);
    void unload(uint32_t start, uint32_t end);  // forget programs loaded in [start, end)
    void call(uint32_t address);                // run the recompiled function at `address`

    // CPU.
    void set_sr(uint16_t sr);
    bool supervisor() const { return ctx.sr_hi & 0x20; }
    void exception(int vector, uint32_t ret, int level = -1);
    void trap(int number, uint32_t ret);
    void line_f(uint16_t opcode, uint32_t ret);
    void service(uint32_t pc);
    void stop(uint16_t sr, uint32_t pc);

    uint64_t now() const { return clock_(); }
    std::atomic<bool> stop_requested{false};
    std::function<void()> on_frame;  // at each vertical blank, on the game thread
    uint64_t frames() const { return frames_.load(); }
    std::atomic<uint32_t> last_poll{0};  // where the game last polled: inside a loop, usually

private:
    bool deliver(uint32_t pc);
    void beam(uint64_t t, uint32_t pc);  // raster interrupts, line drawing and VSYNC up to time t
    int beam_line(uint64_t t) const;
    uint8_t gpip(uint64_t now) const;

    struct Loaded {
        const M68kProgram *program;
        uint32_t base;
    };
    std::vector<const M68kProgram *> programs_;
    std::vector<Loaded> loaded_;
    std::function<uint64_t()> clock_;
    int tracing_ = 0;
    // The beam, within the current frame: raster lines from the start of vertical sync.
    uint64_t frame_start_ = 0;
    int rendered_ = 0;     // lines drawn so far
    int raster_from_ = 0;  // the raster interrupt can still fire at this line or later
    bool vblank_done_ = false;
    uint64_t next_key_ = 0;  // the keyboard line carries a code every ~4 ms (2400 baud)
    std::atomic<uint64_t> frames_{0};
};

}  // namespace willow
