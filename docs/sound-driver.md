# Sound driver interface

How Cotton makes sound, and what the Willow runtime has to provide for it.
Offsets are file offsets into `ctnsnd.r` (a `.R` program, so it runs at whatever
address it is loaded) or into the unpacked `cot.r` image (load address 0).

## Who loads what

| Stage | Program | Sound |
|---|---|---|
| Boot sector IPL | loads `HUMAN.SYS` and `bootsnd.sys` | `bootsnd.sys` plays a jingle on the YM2151 while the OS boots. It hooks `trap #0` and marks itself with the signature `CBot`. |
| `config.sys` | `PROGRAM = ealogo.r`, then `SHELL = bootcot.r` | none |
| `bootcot.r` | checks for about 1.8 MB free, maps the system disk to A: and the data disk to B: (by their marker files), then `_EXEC`s `cot.r` | none |
| `cot.r` 0x18c | clears PPI port C, `_EXEC`s `ctnsnd.r`, then `_WAIT`s and quits unless the driver stayed resident | none |
| `ctnsnd.r` | installs itself, stops `bootsnd.sys`, stays resident (`_KEEPPR`, the whole 0x2a13a-byte file) | everything from here on |

`cot.r` never touches the YM2151, the ADPCM chip or the DMA controller. All
sound goes through `ctnsnd.r`.

## Driver install (`ctnsnd.r` 0x000)

1. Reads the `trap #0` vector and checks the long just before the handler.
   - `CSnd`: the driver is already resident. It calls `trap #0` with d0 = $FF (uninstall) and exits. Running it twice toggles it off.
   - `CBot`: the boot jingle is resident. It calls `trap #0` once to stop it.
2. Stops the YM2151 timers (register $14 = $30) and clears the YM2151 interrupt hook.
3. Installs its vectors with `_INTVCS` and saves the old ones for uninstall:
   - `trap #0` (vector $20) to 0x0de, the command queue
   - `trap #1` (vector $21) to 0x1c4, the voice request
   - vector $6A (DMA channel 3 done) to 0x210
4. Starts YM2151 timer B: register $12 = $BF, register $14 = $3A. With the
   X68000's 4 MHz YM2151 clock, that is 1024 × (256 − 191) / 4 MHz ≈ 16.6 ms,
   about 60.1 Hz.
5. Hooks the YM2151 interrupt with IOCS `_OPMINTST` to 0x27c, the tick.
6. Calls `trap #0` with d0 = 0 (stop all), then `_KEEPPR`.

The work area starts at 0x1680, with 18 track slots of $82 bytes from 0x16d6.

## The game's interface

### `trap #0`: command queue (0x0de)

d0.b = command, d1.w = argument. Except for $FF, nothing happens right away.
With interrupts masked (`sr = $2700`), the handler appends the command to a
7-entry queue. It drops the command if the queue is full, or if the same
command byte is already waiting, even with a different argument.

The next tick drains the queue:

| d0 | Effect |
|---|---|
| $00 | Stop everything: clear all 18 tracks, reset the fade, key off all YM2151 channels |
| $01–$F0 | Play sound entry (d0 & $7F) from the table at 0x23fa (songs and effects) |
| $F9 | Set a flag that changes which saved keyboard byte (IOCS work $0810) uninstall restores |
| $FA | Tempo = d1 |
| $FB | Master attenuation = d1 |
| $FC | Fade speed = d1 (step and delay bytes) |
| $FD | Fade in |
| $FE | Fade out |
| $F1–$F8 | No effect |
| $FF | Uninstall immediately (not queued): silence, restore vectors, free memory |

`cot.r` has 176 `trap #0` calls. 173 load a constant command into d0: 82
distinct values, covering sounds $01–$7D plus $00 (stop), $FD (fade in), $FE
(fade out) and a single $FF (uninstall, on exit). The other three read the
command from memory. The commands that take an argument ($FA–$FC) never appear
as constants.

### `trap #1`: voice request (0x1c4)

d0.w = (priority << 8) | sample, with sample 1–31. d0 = 0 cancels the voice. The
request replaces the pending one only if its priority is at least the current
priority. While it updates, the handler masks the YM2151 interrupt in the MFP
(IMRB bit 3, at $E88015).

The game never calls `trap #1`. Voices are triggered from inside sound
sequences: a sequence command reads a 2-byte operand and the driver calls its
own `trap #1` with it (0x126a).

## The tick: YM2151 timer B interrupt (0x27c)

About 60 times a second, at interrupt mask level 5, the tick:

1. Drains the command queue.
2. Advances the fade.
3. Runs the sequencer for the 18 track slots and writes the YM2151 directly
   (address $E90001, data $E90003). Before each access it spins until the
   chip's busy bit clears.
4. Starts a pending voice, if any (0x47e):
   - Sets YM2151 register $1B to $80. The CT1 output pin then selects the 4 MHz ADPCM clock.
   - Looks the sample up in the 8-byte-entry table at 0x265a: offset, length, flags. The flags select 7.8 kHz or 3.9 kHz (4 MHz / 512 or / 1024), left/right output, whether to use the global pan setting, and "skip if this is the same sample as the last one started".
   - Programs DMA channel 3: abort (CCR = $10), then OCR = $3A (memory to device, byte transfers, array chaining, external request), clears CSR, sets BAR to a two-entry chain (the sample, then a 26-byte silence tail at 0x1666) and BTC = 2, and starts it with completion interrupts enabled (CCR = $88).
   - Writes the pan and rate bits to PPI port C ($E9A005).
   - Starts playback: MSM6258 command $E92001 = 2.

### DMA channel 3 done (0x210)

Clears the IOCS ADPCM-busy word ($C32), clears the channel status, writes $88
(silence) to the MSM6258 data port ($E92003) and clears a driver state long at
0x16b6.

## What the runtime must provide

**Recompile, don't emulate.** `ctnsnd.r` is a second 68000 program, compiled
like `cot.r`. Its handlers are reached only through vectors, so `programs.toml`
lists them as roots beside the entry point 0x000: 0x0de, 0x1c4, 0x210 and
0x27c.

**Human68k and IOCS calls the sound path uses:** `_EXEC`, `_WAIT`, `_KEEPPR`,
`_MFREE`, `_INTVCG`, `_INTVCS` (vectors $20, $21, $6A), IOCS `_OPMSET`,
`_OPMINTST`, `_B_LPEEK` and `_B_BPEEK`, and IOCS $03. The driver calls IOCS
$03 at uninstall with a keyboard byte saved from IOCS work $0810; its exact
function is still unconfirmed.

**Hardware, modeled at the register level** (implemented in `runtime/src/devices.*`
and `runtime/src/sound.*`; see [`runtime.md`](runtime.md#sound)):

| Device | Registers | Model |
|---|---|---|
| YM2151 | $E90001 address, $E90003 data and status | Third-party core. Reports busy for 64 clocks (16 µs) after each data write, so the driver's spin loops take their real time. Timer B overflow raises the tick. Register $1B bit 7 (CT1) selects the ADPCM clock. |
| MSM6258 | $E92001 command, $E92003 data | Third-party core or our own decoder. 4-bit OKI ADPCM. |
| DMA channel 3 | CSR, OCR, CCR, BTC, BAR at $E840C0 + offset | Our own model: array chaining from memory to the ADPCM data port, completion raises vector $6A. The driver relies on IOCS having set the device address and control registers at boot, so the model hardwires them. |
| PPI port C | $E9A005 | Bits 0–1 turn the ADPCM left/right outputs off, bits 2–3 select the clock divider. `cot.r` writes it too. |
| MFP IMRB bit 3 | $E88015 | Masks the YM2151 interrupt. |

**Threading.** The driver already separates the game from the tick: `trap #0`
and `trap #1` only touch the queue and the pending voice, with interrupts
masked, and the tick consumes them. The runtime keeps all 68000 code on one
thread and delivers the tick like any other interrupt: generated code polls at
every loop back-edge and function entry, and the runtime runs the handler there
when timer B is due and the interrupt mask allows it (`docs/runtime.md`). The
original queue semantics, including dropping duplicates, hold with no changes to
the driver and no locking. The audio thread only renders chip output from the
register writes it is handed.

## The boot jingle

Willow plays it. `bootsnd.sys` is recompiled like the other programs
(`programs.toml`), and the runtime calls it at startup the way the boot loader
does: loaded at $3000, called as a subroutine, before anything else runs.

- Install (+0x000): stops the YM2151 timers, hooks `trap #0` with IOCS
  `_B_INTVCS` (handler at +0x06a, signature `CBot` just before it), starts timer
  B at $BF like `ctnsnd.r`, and hooks the tick (+0x0b8) with `_OPMINTST`.
- `trap #0` (+0x06a): requests a fade, then busy-waits on a flag until the tick
  has finished it. It stops the timers, unhooks itself and returns with `rte`.
  The busy-wait only ends because the tick keeps running. Interrupt polling
  covers it: the wait is a loop, so it polls, and the runtime delivers the tick
  inside it. The fade adds one step a tick up to $40, about a second.

The jingle loops until then, because `cot.r` reads both disks in before it
installs `ctnsnd.r`: about 30 seconds with Willow's disk timing, about 75 at the
real drive's speed (`docs/runtime.md`). So it plays through the EA logo, as on
the real machine.

## Open questions

- After `_EXEC` returns, `cot.r` stores a1 + $2B400 (0x1ac). We need to know what
  Human68k leaves in a1, because it feeds the game's memory layout.
- The sequencer's tempo: timer B is fixed at about 60 Hz. The tempo value ($FA,
  default $96) appears to drive a software accumulator (0x556). Confirm this
  before relying on it.
