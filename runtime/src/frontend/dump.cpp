// willow_dump: runs the game headlessly for a fixed number of frames, with no
// window and no sound, saving snapshots and the log. For development: checking
// the renderer and seeing how far the game gets. It runs as fast as the host
// allows; the game's own timing follows its cycle count, so runs are repeatable.
//
//   willow_dump [--data DIR] [--out DIR] [--frames N] [--every K] [--timeout SECONDS]
//               [--key FRAME:CODE[:FRAMES]]...
//
// --key presses an X68000 key code (hex) at a frame and holds it for FRAMES frames
// (6 by default): `--key 2500:55` presses XF1, which inserts a coin on the title screen
// once the startup's disk loading is done.
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

#include "boot.hpp"
#include "common.hpp"
#include "machine.hpp"
#include "png.hpp"

extern "C" {
extern const M68kProgram *const willow_programs[];
extern const size_t willow_program_count;
}

#ifndef WILLOW_DEFAULT_DATA_DIR
#define WILLOW_DEFAULT_DATA_DIR ""
#endif

namespace fs = std::filesystem;
using namespace std::chrono;

static void write_wav(const fs::path &path, uint32_t rate, const std::vector<int16_t> &samples)
{
    std::FILE *f = std::fopen(path.string().c_str(), "wb");
    if (!f) return;
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };  // WAV is little-endian, as are our hosts
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    uint32_t bytes = uint32_t(samples.size() * 2);
    std::fwrite("RIFF", 1, 4, f), u32(36 + bytes), std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16), u16(1), u16(2), u32(rate), u32(rate * 4), u16(4), u16(16);
    std::fwrite("data", 1, 4, f), u32(bytes);
    std::fwrite(samples.data(), 2, samples.size(), f);
    std::fclose(f);
}

int main(int argc, char *argv[])
{
    fs::path data = WILLOW_DEFAULT_DATA_DIR, out = fs::path(WILLOW_DEFAULT_DATA_DIR).parent_path() / "dump";
    uint64_t frames = 300, every = 30;
    double timeout = 30;
    struct Press {
        uint64_t frame, hold;
        uint8_t code;
    };
    std::vector<Press> presses;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string opt = argv[i], val = argv[i + 1];
        if (opt == "--data") data = val;
        else if (opt == "--out") out = val;
        else if (opt == "--frames") frames = std::stoull(val);
        else if (opt == "--every") every = std::max<uint64_t>(1, std::stoull(val));
        else if (opt == "--timeout") timeout = std::stod(val);
        else if (opt == "--key") {
            size_t a = val.find(':'), b = val.find(':', a + 1);
            uint64_t hold = b == std::string::npos ? 6 : std::stoull(val.substr(b + 1));
            presses.push_back({std::stoull(val.substr(0, a)), hold, uint8_t(std::stoul(val.substr(a + 1, b - a - 1), nullptr, 16))});
        }
    }
    if (!fs::is_directory(data / "system")) {
        willow::log("no extracted disks in %s; run `python -m tools.willow extract`", data.string().c_str());
        return 1;
    }
    fs::create_directories(out);

    willow::Options options;
    options.data_dir = data;
    options.save_dir = out / "saves";  // never touch the real saves
    options.debug = willow::Debug::from_environment();
    willow::Machine machine(options);
    for (size_t i = 0; i < willow_program_count; i++) machine.add_program(willow_programs[i]);
    std::vector<int16_t> audio;
    machine.sound.sink = [&](const int16_t *samples, size_t n) { audio.insert(audio.end(), samples, samples + n * 2); };

    machine.on_frame = [&] {
        uint64_t n = machine.frames();
        for (const Press &p : presses) {
            if (n == p.frame) machine.input.key(p.code, true);
            if (n == p.frame + p.hold) machine.input.key(p.code, false);
        }
        if (n % every == 0) {
            std::vector<uint8_t> rgb;
            int w = 0, h = 0;
            float aspect = 1;
            if (machine.video.latest(rgb, w, h, aspect)) {
                char name[32];
                std::snprintf(name, sizeof name, "frame%05llu.png", (unsigned long long)n);
                willow::write_png(out / name, w, h, rgb);
            }
            willow::log("frame %llu: last poll at %06x, %dx%d, %s", (unsigned long long)n, machine.last_poll.load(), w, h,
                        machine.video.describe().c_str());
        }
        if (n >= frames) machine.stop_requested = true;
    };

    std::atomic<bool> done{false};
    std::string error;
    std::thread game([&] {
        try {
            willow::boot(machine);
        } catch (const willow::Shutdown &) {
        } catch (const std::exception &e) {
            error = e.what();
        }
        done = true;
    });
    auto start = steady_clock::now();
    while (!done) {
        std::this_thread::sleep_for(microseconds(500));
        if (duration<double>(steady_clock::now() - start).count() > timeout) {
            machine.stop_requested = true;
            m68k_pending = 1;
            if (duration<double>(steady_clock::now() - start).count() > timeout + 5) {
                // The game thread never polled again (stuck outside any loop); don't wait forever.
                willow::log("timed out after %.0f s with the game thread unresponsive at frame %llu", timeout,
                            (unsigned long long)machine.frames());
                std::fflush(stderr);
                std::_Exit(3);
            }
        }
    }
    game.join();
    write_wav(out / "audio.wav", machine.sound.rate(), audio);
    willow::log("ADPCM voices started: %u", machine.adpcm.voices_started);
    willow::log("finished at frame %llu%s%s", (unsigned long long)machine.frames(), error.empty() ? "" : ": ", error.c_str());
    return error.empty() ? 0 : 2;
}
