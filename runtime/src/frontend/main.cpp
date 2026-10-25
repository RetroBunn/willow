// The Willow executable: an SDL3 window around the machine. The game runs on its
// own thread (its main loop is the recompiled code itself); this thread handles
// the window and raises m68k_pending so the game thread delivers interrupts on time.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "boot.hpp"
#include "common.hpp"
#include "machine.hpp"

extern "C" {
extern const M68kProgram *const willow_programs[];
extern const size_t willow_program_count;
}

#ifndef WILLOW_DEFAULT_DATA_DIR
#define WILLOW_DEFAULT_DATA_DIR ""
#endif

namespace fs = std::filesystem;

// PC keys to X68000 key codes, by position. Cotton's buttons are SPACE or XF1 (coin,
// shot) and XF2 (start, magic); PC keyboards have no XF keys, so Z and X stand in.
static uint8_t x68k_key(SDL_Scancode s)
{
    switch (s) {
    case SDL_SCANCODE_Z: return 0x55;  // XF1
    case SDL_SCANCODE_X: return 0x56;  // XF2
    case SDL_SCANCODE_ESCAPE: return 0x01;
    case SDL_SCANCODE_MINUS: return 0x0C;
    case SDL_SCANCODE_EQUALS: return 0x0D;
    case SDL_SCANCODE_BACKSLASH: return 0x0E;
    case SDL_SCANCODE_BACKSPACE: return 0x0F;
    case SDL_SCANCODE_TAB: return 0x10;
    case SDL_SCANCODE_GRAVE: return 0x1B;
    case SDL_SCANCODE_LEFTBRACKET: return 0x1C;
    case SDL_SCANCODE_RETURN: return 0x1D;
    case SDL_SCANCODE_SEMICOLON: return 0x27;
    case SDL_SCANCODE_APOSTROPHE: return 0x28;
    case SDL_SCANCODE_RIGHTBRACKET: return 0x29;
    case SDL_SCANCODE_COMMA: return 0x31;
    case SDL_SCANCODE_PERIOD: return 0x32;
    case SDL_SCANCODE_SLASH: return 0x33;
    case SDL_SCANCODE_SPACE: return 0x35;
    case SDL_SCANCODE_HOME: return 0x36;
    case SDL_SCANCODE_DELETE: return 0x37;
    case SDL_SCANCODE_PAGEUP: return 0x38;    // ROLL UP
    case SDL_SCANCODE_PAGEDOWN: return 0x39;  // ROLL DOWN
    case SDL_SCANCODE_END: return 0x3A;       // UNDO
    case SDL_SCANCODE_LEFT: return 0x3B;
    case SDL_SCANCODE_UP: return 0x3C;
    case SDL_SCANCODE_RIGHT: return 0x3D;
    case SDL_SCANCODE_DOWN: return 0x3E;
    case SDL_SCANCODE_NUMLOCKCLEAR: return 0x3F;  // CLR
    case SDL_SCANCODE_KP_DIVIDE: return 0x40;
    case SDL_SCANCODE_KP_MULTIPLY: return 0x41;
    case SDL_SCANCODE_KP_MINUS: return 0x42;
    case SDL_SCANCODE_KP_7: return 0x43;
    case SDL_SCANCODE_KP_8: return 0x44;
    case SDL_SCANCODE_KP_9: return 0x45;
    case SDL_SCANCODE_KP_PLUS: return 0x46;
    case SDL_SCANCODE_KP_4: return 0x47;
    case SDL_SCANCODE_KP_5: return 0x48;
    case SDL_SCANCODE_KP_6: return 0x49;
    case SDL_SCANCODE_KP_EQUALS: return 0x4A;
    case SDL_SCANCODE_KP_1: return 0x4B;
    case SDL_SCANCODE_KP_2: return 0x4C;
    case SDL_SCANCODE_KP_3: return 0x4D;
    case SDL_SCANCODE_KP_ENTER: return 0x4E;
    case SDL_SCANCODE_KP_0: return 0x4F;
    case SDL_SCANCODE_KP_PERIOD: return 0x51;
    case SDL_SCANCODE_F11: return 0x54;  // HELP
    case SDL_SCANCODE_CAPSLOCK: return 0x5D;
    case SDL_SCANCODE_INSERT: return 0x5E;
    case SDL_SCANCODE_PAUSE: return 0x61;        // BREAK
    case SDL_SCANCODE_PRINTSCREEN: return 0x62;  // COPY
    case SDL_SCANCODE_LSHIFT:
    case SDL_SCANCODE_RSHIFT: return 0x70;
    case SDL_SCANCODE_LCTRL:
    case SDL_SCANCODE_RCTRL: return 0x71;
    case SDL_SCANCODE_LALT: return 0x72;  // OPT.1
    case SDL_SCANCODE_RALT: return 0x73;  // OPT.2
    default: break;
    }
    if (s >= SDL_SCANCODE_1 && s <= SDL_SCANCODE_0) return uint8_t(0x02 + (s - SDL_SCANCODE_1));
    if (s >= SDL_SCANCODE_F1 && s <= SDL_SCANCODE_F10) return uint8_t(0x63 + (s - SDL_SCANCODE_F1));
    static const uint8_t letters[26] = {0x1E, 0x2E, 0x2C, 0x20, 0x13, 0x21, 0x22, 0x23, 0x18, 0x24, 0x25, 0x26, 0x30,
                                        0x2F, 0x19, 0x1A, 0x11, 0x14, 0x1F, 0x15, 0x17, 0x2D, 0x12, 0x2B, 0x16, 0x2A};
    if (s >= SDL_SCANCODE_A && s <= SDL_SCANCODE_Z) return letters[s - SDL_SCANCODE_A];
    return 0;
}

// Gamepads drive joystick port A (the first pad) and B (the second): stick or d-pad to
// move, south/west for button A (coin, shot), east/north for B (start, magic), and
// back/start as coin/start.
static uint8_t pad_state(SDL_Gamepad *pad)
{
    using willow::Input;
    uint8_t bits = 0;
    int x = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX), y = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
    if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_UP) || y < -16000) bits |= Input::kUp;
    if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN) || y > 16000) bits |= Input::kDown;
    if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT) || x < -16000) bits |= Input::kLeft;
    if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || x > 16000) bits |= Input::kRight;
    if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH) || SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_WEST) ||
        SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK))
        bits |= Input::kA;
    if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_EAST) || SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_NORTH) ||
        SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_START))
        bits |= Input::kB;
    return bits;
}

int main(int argc, char *argv[])
{
    fs::path data = WILLOW_DEFAULT_DATA_DIR;
    for (int i = 1; i < argc; i++)
        if (std::string(argv[i]) == "--data" && i + 1 < argc) data = argv[++i];
    if (data.empty() || !fs::is_directory(data / "system") || !fs::is_directory(data / "data")) {
        const char *message = "Extracted game files not found.\nRun `python -m tools.willow extract` first, "
                              "or start Willow with --data <folder holding system/ and data/>.";
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Willow", message, nullptr);
        return 1;
    }

    willow::Options options;
    options.data_dir = data;
    options.save_dir = data.parent_path() / "saves";
    willow::Machine machine(options);
    for (size_t i = 0; i < willow_program_count; i++) machine.add_program(willow_programs[i]);

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        willow::log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    // Sound comes from the game thread in small slices at the YM2151's own rate; SDL
    // resamples it for the device. Keep the queue short so the sound stays in sync.
    SDL_AudioStream *audio = nullptr;
    SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, int(machine.sound.rate())};
    if (SDL_InitSubSystem(SDL_INIT_AUDIO))
        audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (audio) {
        int max_queued = int(machine.sound.rate()) * 4 / 8;  // 1/8 s of stereo 16-bit
        machine.sound.sink = [audio, max_queued](const int16_t *samples, size_t frames) {
            if (SDL_GetAudioStreamQueued(audio) < max_queued) SDL_PutAudioStreamData(audio, samples, int(frames * 4));
        };
        SDL_ResumeAudioStreamDevice(audio);
    } else {
        willow::log("no sound: %s", SDL_GetError());
    }
    SDL_Window *window = SDL_CreateWindow("Willow", 960, 720, SDL_WINDOW_RESIZABLE);
    SDL_Renderer *renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (!renderer) {
        willow::log("could not open a window: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_SetRenderVSync(renderer, 1);

    std::atomic<bool> game_done{false};
    std::string game_error;
    std::thread game([&] {
        try {
            willow::boot(machine);
        } catch (const willow::Shutdown &) {
        } catch (const std::exception &e) {
            game_error = e.what();
            willow::log("stopped: %s", e.what());
        }
        game_done = true;
    });
    std::thread ticker([&] {
        while (!game_done) {
            m68k_pending = 1;
            SDL_DelayNS(500'000);
        }
    });

    bool running = true;
    uint64_t last_title = 0;
    SDL_Texture *texture = nullptr;
    int tex_w = 0, tex_h = 0;
    std::vector<uint8_t> frame;
    std::vector<SDL_Gamepad *> pads;
    bool held[0x80] = {};
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_QUIT:
                running = false;
                break;
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
                if (uint8_t code = x68k_key(e.key.scancode); code && !e.key.repeat) {
                    bool down = e.type == SDL_EVENT_KEY_DOWN;
                    if (held[code] != down) machine.input.key(code, down);  // the X68000 keyboard does its own repeat
                    held[code] = down;
                }
                break;
            case SDL_EVENT_WINDOW_FOCUS_LOST:  // let go of everything, or keys stay down
                for (int code = 1; code < 0x80; code++)
                    if (held[code]) machine.input.key(uint8_t(code), false), held[code] = false;
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                if (SDL_Gamepad *pad = SDL_OpenGamepad(e.gdevice.which)) pads.push_back(pad);
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                for (size_t i = 0; i < pads.size(); i++)
                    if (SDL_GetGamepadID(pads[i]) == e.gdevice.which) SDL_CloseGamepad(pads[i]), pads.erase(pads.begin() + long(i));
                break;
            default:
                break;
            }
        }
        for (int port = 0; port < 2; port++)
            machine.input.joystick(port, port < int(pads.size()) ? pad_state(pads[size_t(port)]) : 0);
        uint64_t ticks = SDL_GetTicks();
        if (ticks - last_title >= 500) {
            last_title = ticks;
            std::string title = game_done ? (game_error.empty() ? "Willow - stopped" : "Willow - stopped: " + game_error)
                                          : std::string("Willow");
            SDL_SetWindowTitle(window, title.c_str());
        }
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        int w = 0, h = 0;
        float aspect = 1;
        if (machine.video.latest(frame, w, h, aspect)) {
            if (!texture || w != tex_w || h != tex_h) {
                if (texture) SDL_DestroyTexture(texture);
                texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, w, h);
                SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
                tex_w = w, tex_h = h;
            }
            SDL_UpdateTexture(texture, nullptr, frame.data(), w * 3);
            // Letterbox to the picture's shape on the original monitor.
            int out_w = 0, out_h = 0;
            SDL_GetRenderOutputSize(renderer, &out_w, &out_h);
            float scale = std::min(out_w / aspect, float(out_h));
            SDL_FRect dst = {(out_w - scale * aspect) / 2, (out_h - scale) / 2, scale * aspect, scale};
            SDL_RenderTexture(renderer, texture, nullptr, &dst);
        }
        SDL_RenderPresent(renderer);
    }

    machine.stop_requested = true;
    m68k_pending = 1;
    game.join();
    ticker.join();
    for (SDL_Gamepad *pad : pads) SDL_CloseGamepad(pad);
    if (audio) SDL_DestroyAudioStream(audio);
    if (texture) SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
