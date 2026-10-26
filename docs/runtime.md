# Runtime

The runtime turns the generated C into a program: it provides the machine the
recompiled code expects (memory, devices, interrupts) and native versions of
Human68k and IOCS. C++17, SDL3 for the window (and later sound and input),
built with CMake.

## Building

You need CMake 3.24 or newer, a C/C++ compiler (GCC or Clang; on Windows,
MinGW-w64 works), Ninja or another CMake generator, and an internet connection
the first time, to fetch SDL 3.4.18 (pinned by URL and SHA-256).

```sh
git submodule update --init       # third-party code: ymfm
python -m tools.willow extract    # the disks' files, for the game to read at run time
python -m tools.willow recomp     # the generated C
cmake -S . -B build/cmake -G Ninja
cmake --build build/cmake
```

That produces `build/cmake/runtime/willow` (`willow.exe` on Windows). It looks
for the extracted disks in `build/disks` by default; `--data <folder>` points it
elsewhere. Anything the game writes goes to `build/saves`.

Without generated code, the same commands build only the runtime library and its
tests, which need no game files: `ctest --test-dir build/cmake`.

## Layout

| File | Role |
|---|---|
| `include/willow/m68k.h` | The interface generated code compiles against. |
| `src/memory.*` | The 24-bit address space: RAM, graphic and text VRAM, I/O pages, ROM. |
| `src/devices.*` | MFP (interrupt controller), YM2151 register file and timers, PPI (joysticks). |
| `src/sound.*` | DMA channel 3, the MSM6258, and the mixer around ymfm's YM2151. |
| `src/input.*` | Keyboard codes and joystick state, from the window's thread to the game's. |
| `src/video.*` | The display: layers, composition, line-by-line drawing. |
| `src/machine.*` | CPU context, supervisor stack switching, exception frames, interrupt delivery, the clock, the loaded programs, the debugging options, and the C functions generated code calls. |
| `src/human68k.*` | Processes, memory blocks and DOS calls. |
| `src/filesystem.*` | Drives A: and B: on the extracted disks, with a write overlay, and the floppy drives' timing. |
| `src/iocs.*` | IOCS calls. |
| `src/boot.*` | The boot sequence. |
| `src/frontend/main.cpp` | The executable: window, input, and the game thread paced to the host's clock. |
| `src/frontend/dump.cpp` | `willow_dump`, the headless runner (see [Headless runs](#headless-runs)). |
| `src/common.*` | Logging, and Shift-JIS text from the game turned into UTF-8 for it. |
| `tests/test_runtime.cpp` | Headless tests on hand-written stand-in programs. |

## How it runs

- **One game thread.** The game's main loop is the recompiled code itself, so it
  runs on its own thread, started by `boot()`. The main thread owns the window.
- **Time is the 68000's.** Generated code adds each instruction's cycle count
  to `ctx->cycles` (`tools/willow/timing.py`: the 68000 manual's figures, with
  no wait states, which is how the 10 MHz X68000's memory runs). The machine's
  clock is that count, 100 ns per cycle, and VSYNC, the raster beam, the YM2151
  timers and the sound all follow it. A frame's work therefore takes as long as
  it did on the real machine. Cotton waits for VSYNC between game frames; in a
  busy scene its work can run past the next VSYNC, and the game slows down, as
  it did on the X68000. Light scenes such as the title run one game frame per
  video frame either way. The startup's graphics unpacking also takes its real
  few seconds.
- **Disk loading takes the drive's time.** Human68k is native code here, but its
  file calls add up how long the X68000's 2HD drives would have taken (`Floppy`
  in `src/filesystem.hpp`). Each transfer waits half a turn of the disk (83 ms)
  for its first sector, then reads about 32 KB/s, a 16 KB cylinder every three
  turns: two to read its tracks, one lost stepping to the next. The first search
  of a directory reads one sector; after that Human68k has it buffered
  (`config.sys` sets `BUFFERS = 20`). Willow's drive then runs three times
  faster than that (`Floppy::kSpeedup`), by choice. The DOS call waits that long
  in `Machine::busy`, with interrupts and the beam running, before it returns.
  `_EXEC` reads the whole program file, and an LZX-packed program then spends
  what its unpacker took on the 68000 (`tools/willow/lzx.py` counts it along the
  stub's code paths). Cotton reads both disks into memory at startup, so the EA
  logo stays up for about 30 seconds with the boot jingle looping (about 75 at
  the real drive's speed), and the title appears about 43 seconds after boot.
- **Interrupts by polling.** Generated code polls at every loop back-edge and
  function entry. Each time the cycle count passes `ctx->deadline`, set
  `kSliceCycles` (256 cycles, about one raster line) ahead, or the runtime
  raises `m68k_pending` (the interrupt mask was lowered, or the game is shutting
  down), `service()` advances the devices to the current time and delivers
  whatever is due, if the interrupt mask allows it: it pushes a real exception
  frame and calls the handler found in the vector table in emulated memory.
  Interrupts therefore land between instructions, as on hardware, and all
  emulated state stays on one thread. Taking an interrupt costs 44 cycles, as
  on the 68000, and `stop` lets time pass from service to service until one
  arrives.
- **Paced to the host.** `willow.exe` sets `Machine::wait`, and each service
  sleeps while emulated time is more than a millisecond ahead of the host's
  clock, so the game thread sleeps while the game busy-waits for VSYNC. If the
  host falls more than 100 ms behind (a stall), the machine carries on from
  there rather than racing to catch up. `willow_dump` leaves `wait` unset and
  runs as fast as it can, several times real time; the same keys then always
  give the same frames and audio.
- **Unwinding.** `_EXIT`, `_EXIT2` and `_KEEPPR` throw a C++ exception that
  unwinds through the recompiled code back to the `_EXEC` that started the
  program. Generated C is compiled with unwind tables for this. Closing the
  window throws `Shutdown` into the game thread the same way.

## Memory

| Address | Contents |
|---|---|
| $000000 | Exception vectors. Unclaimed vectors point into ROM, where the runtime handles them. |
| $000400 | IOCS call table, so programs can hook IOCS calls. |
| $002000 | The boot loader's stack top. |
| $003000 | `bootsnd.sys`, where the boot loader puts it; it stays resident. |
| $006800 | Supervisor stack top once Human68k is up. |
| $010000 | Human68k's memory pool: processes and memory blocks, up to the end of RAM (4 MB). |

Memory blocks carry Human68k's real headers (previous, owner, end, next), and a
process's block holds its 240-byte process block before its program at +$100,
because programs read both.

## Boot

`boot()` replays the real sequence:

1. **The boot loader's part:** read the root directory and `bootsnd.sys`, load
   it at $3000 and call it in supervisor mode with interrupts masked. It hooks
   `trap #0` and the YM2151 tick, and returns. Then read `HUMAN.SYS` with
   interrupts enabled, so the jingle plays meanwhile, which is what it is for.
2. **Human68k's part:** move the supervisor stack, set up the memory pool, and
   read `config.sys`.
3. **`config.sys`:** `_EXEC` `ealogo.r` (`PROGRAM =`), then `bootcot.r`
   (`SHELL =`). `bootcot.r` remaps the disks and runs `cot.r`, which reads in
   both disks and then installs `ctnsnd.r`.

Each read takes the disk's time (see above). `_EXEC` keeps the cycle count, the
machine's clock, running through the switch to the new process and back.

## Video

`src/video.*` composes frames from the video registers and memory, the way the
X68000's video chips do:

- **Graphics:** 16-color (four pages), 256-color (two pages) and 65,536-color
  modes, each page scrolled by its CRTC registers and ordered by VC R1.
- **Text:** four 1024x1024 bitplanes with scroll. Writes honor the CRTC's text
  access modes: R21 bit 8 writes all selected planes at once and bit 9 applies
  the R23 mask. The game clears its text layer that way.
- **Sprites and BG:** 128 16x16 sprites and two BG layers (8x8 or 16x16 cells)
  from PCG RAM, with each sprite placed behind, between or in front of the BG
  layers by its priority.
- **Composition:** text, graphics and the sprite layer ordered by VC R1, color 0
  transparent, and the system port's contrast applied as brightness.
- **Contrast fades by itself.** On the real machine a new contrast setting takes
  effect gradually: the brightness moves a third of a level every 1/60 s (the
  rate XEiJ uses), so a jump from 15 to 0 fades the picture out over 0.75 s.
  Cotton relies on this. It only ever writes 0 or 15 (helpers at `cot.r+$dfa`
  and `+$e0c`) and then counts frames while the hardware fades. Changing layers
  or palettes still takes effect at once, so a scene that switches its layers
  off cuts to black, as it does on the hardware.

**Drawn as the beam moves.** A frame is 568 raster lines (from CRTC R04).
`service()` advances the beam to the current time and handles each frame's events
in line order: a raster interrupt when the beam reaches R09, VSYNC at the end of
the display (R07), and the start of the next frame. Before each event it draws
the lines above it, so when a raster handler changes scroll or palette registers,
only the lines below change, as on the real screen. `cot.r` paces itself with a
chain of raster interrupts and splits its screen into a scrolling playfield and
fixed status bars this way.

The window shows each finished frame letterboxed to its shape on the original
monitor: Cotton's 384x256 picture filled a square area on a 4:3 screen.

## Sound

`src/sound.*` turns what the sound driver does into audio, all on the game
thread: every `service()` first generates samples up to the current time.

- **YM2151:** [ymfm](https://github.com/aaronsgiles/ymfm) (BSD-3-Clause), a git
  submodule in `external/ymfm` pinned to one commit. Register writes go to it as
  they happen; its timers and IRQ are unused, because the runtime's own YM2151
  timer model drives the game's interrupts. It runs at the X68000's 4 MHz, so the
  output is 62.5 kHz stereo, which the whole mix uses.
- **Write timing:** the chip first catches up to the current time, so each
  register write lands on the right sample. A data write then reports busy for
  64 clocks (16 µs), as the real chip does, so the sound driver's spin loops take
  their real time, and runs the chip for that one sample. The driver restarts
  every note by keying the channel off and on again within one tick; without the
  sample in between, the chip never sees the key-off and decaying sounds (the FM
  drums especially) don't sound again.
- **MSM6258:** our own 4-bit OKI ADPCM decoder, low nibble first. Its rate is
  4 or 8 MHz (the YM2151's CT1 pin) divided by 1024, 768 or 512 (PPI port C),
  linearly interpolated up to 62.5 kHz, high-passed like the hardware's output
  coupling, and sent left and right by PPI port C's enables.
- **DMA channel 3:** array chaining from memory to the ADPCM chip, one byte per
  two samples, timed by sample generation. When a transfer ends it raises the
  completion interrupt at level 3 through channel 3's vector register ($6A, as
  IOCS sets it), which is how the driver learns a voice has finished.

`willow.exe` sends the mix to an SDL audio stream with at most 1/8 s queued;
`willow_dump` writes it to `build/dump/audio.wav`. For listening to parts of the
mix, see [Debugging](#debugging).

## Input

`src/input.*` carries input from the window's thread to the game thread, where it
arrives as the hardware would deliver it:

- **Keyboard:** the X68000 keyboard sends key codes ($01-$7F, bit 7 set on
  release) over the MFP's serial line at 2400 baud. Each code lands in the MFP's
  receive buffer, at most one every 4 ms, and raises the receive interrupt
  (channel 12, vector $4C). Cotton's handler maps codes to its button word through
  a table, and also polls the data register once a frame for single keys.
- **Joysticks:** each joystick pulls PPI port A or B lines low: bits 0-3 for up,
  down, left and right, bits 5 and 6 for its two triggers.

In Cotton, trigger A (or SPACE/XF1) inserts a coin on the title screen and shoots
in play; trigger B (or XF2) starts and casts magic. The frontend maps PC keys to
the X68000 key in the same place, with Z and X standing in for XF1 and XF2, and
gamepads to joystick ports A and B. `willow_dump --key FRAME:CODE[:FRAMES]`
presses keys on a script, which is how coin, start and play were checked without
a window.

## Headless runs

`willow_dump` runs the game with no window and no sound for a fixed number of
frames, saving PNG snapshots and the log to `build/dump` (saves go to
`build/dump/saves`, not `build/saves`):

```sh
build/cmake/runtime/willow_dump --frames 3000 --every 100 --timeout 120
```

Loading takes the drive's time here too, though in emulated time, which runs
several times faster than real time: the title appears around frame 2400, so
`--key 2500:55 --key 2700:56` inserts a coin and starts a game, and stage 1
begins around frame 5650.

Left alone on the title screen, the game plays its three attract demos, which
cover parts of several stages, then the ranking, and starts over. That makes a
good regression run that needs no keys: `--frames 32000 --every 200` plays all
three in about 90 seconds, and any code the analysis missed stops it with "no
recompiled function at ...".

Each snapshot line in the log records where the game last polled (inside a loop,
usually) and the video registers.

## Debugging

These environment variables work with both `willow.exe` and `willow_dump`. The
frontends read them once at startup (`Debug::from_environment`), and the runtime
tests ignore them. Only the sound ones change anything, and only what the game
sounds like.

| Variable | Effect |
|---|---|
| `WILLOW_TRACE=1` | Logs every exception, DMA transfer ending and ADPCM voice starting. |
| `WILLOW_TRACE_VECTOR=<hex>` | Also logs every poll site (function entries, loop back-edges) reached inside that vector's handler, with the stack pointer. |
| `WILLOW_WATCH=<file>+<hex offset>` | Logs every write the CPU makes to one byte of a program, with the frame and beam line. `cot.r+df322` shows Cotton's VSYNC flag being set by its interrupt handler and cleared once per game frame by its main loop. |
| `WILLOW_WATCH=<hex address>` | The same for a fixed address, such as `e8e001` (the contrast) or `e82601` (which layers are shown). |
| `WILLOW_OPM_LOG=<file>` | Records every YM2151 register write with its time. |
| `WILLOW_MUTE=fm` or `adpcm` | Silences one chip. |
| `WILLOW_SOLO=0`-`7` | Keeps one FM channel audible (in Cotton's title music, channel 6 is the drums). |

## What exists

- **DOS calls:** `_EXIT`, `_PRINT`, `_CHGDRV`, `_DRVCTRL`, `_CURDRV`, `_SUPER`,
  `_CONCTRL` (ignored), `_INTVCS`, `_KEEPPR`, `_DRVXCHG`, `_INTVCG`, `_CHDIR`,
  `_CREATE`, `_OPEN`, `_CLOSE`, `_READ`, `_WRITE`, `_SEEK`, `_MALLOC`, `_MFREE`,
  `_SETBLOCK`, `_EXEC` (mode 0), `_EXIT2`, `_WAIT`, `_FILES`, `_NFILES`.
- **IOCS calls:** `_B_PRINT` (to the console for now), `_B_COLOR`, `_B_LOCATE`,
  `_OPMSET`, `_OPMINTST`, `_CRTCRAS`, the mouse calls (no mouse), `_SKEY_MOD`,
  `_B_INTVCS`, `_B_BPEEK`, `_B_LPEEK`, `_B_WPOKE`, `_BOOTINF`, `_ROMVER`,
  `_SYS_STAT`, `_OS_CURON`, `_OS_CUROF`.
- **Interrupts:** raster (MFP channel 14), VSYNC (channel 6) and the YM2151
  timers (channel 3), through MFP enable, mask, pending and in-service state.
- **Keyboard line:** the MFP's serial port always reports ready to send, so
  code that sends the keyboard LED commands (the boot jingle blinks them) runs
  on; the commands themselves are dropped.

## Known issues

- **Speed is only as exact as the cycle counts.** Divisions use a typical figure
  (their real time depends on the operands), the bus time that DMA and memory
  refresh take from the CPU is ignored, and IOCS and DOS calls take no time
  apart from the disk's. Stage 1 keeps the 68000 80-100% busy, so small errors
  there decide how often the game slows down.
- **Disk time is an estimate, then sped up.** It follows the drive's geometry,
  not where each file actually lies on the disk, and leaves out the motor
  spinning up after the drive has idled, so the real figure could differ by some
  tens of percent. Every `_READ` waits its half turn and nothing is buffered, so
  a program reading a file in small pieces would be charged more than Human68k's
  buffering costs; Cotton reads each file in one go. On top of that,
  `Floppy::kSpeedup` makes loading three times faster than the estimate.

## Not yet

- **Video details** not used so far: the 1024x1024 graphic mode, VC R2's
  translucency and special-priority effects, the CRTC raster copy and graphic
  fast clear, and the hardware's 32-sprites-per-line limit.
- **Unimplemented calls and I/O** are logged once each, so the console shows
  what the game needs next.
