# Code generator

The second half of the recompiler: it turns each analyzed program into C.

```sh
python -m tools.willow recomp
```

This writes `build/recomp/<program>/`: a header with prototypes, a table that
maps each function's offset to its C function, and the functions themselves,
split into files of 64 so they compile in parallel. Like everything in
`build/`, the output is derived from the game and is never committed.

## The interface

Generated code compiles against `runtime/include/willow/m68k.h`. That header
defines the CPU context (`M68kContext`: registers, condition codes, the system
byte of SR, and `ret`) and inline helpers for arithmetic with condition codes.
It declares the functions the runtime provides:

| Function | Purpose |
|---|---|
| `m68k_read8/16/32`, `m68k_write8/16/32` | Emulated memory: 24-bit, big-endian. RAM, video memory and I/O registers all live behind these. |
| `m68k_trap` | `trap #n`. IOCS calls (`trap #15`), or vectors the game installed (the sound driver's `trap #0` and `trap #1`). |
| `m68k_line_f` | Human68k DOS calls (`$FFxx`) and FLOAT calls (`$FExx`). |
| `m68k_exception` | Illegal instruction, division by zero, `chk`, `trapv`. |
| `m68k_jump` | Run the function at a computed address: `jsr (a0)` and the like. |
| `m68k_set_sr`, `m68k_stop`, `m68k_reset` | Supervisor state, the interrupt mask, stack switching. |

## How 68000 code maps to C

- **One C function per 68000 function.** A function's body is everything it
  reaches without calling. Branches inside it become `goto`. A transfer to
  another function's start becomes a tail call. Code reachable from several
  functions is emitted into each.
- **Calls are real.** `bsr`/`jsr` push the return address on the emulated stack
  and call the C function; `rts` pops it into `ctx->ret`. After each call, the
  caller checks `ctx->ret`. If the callee came back somewhere unexpected, a
  per-function resume dispatcher either jumps to the matching label or returns,
  so the caller's caller can check in turn. Returns go through the same
  dispatcher. This keeps working when code edits its stack. `cot.r` relies on
  that: it pushes a return address with `pea`, then jumps through a state table.
- **Addresses inside a program are `BASE + offset`.** `<program>_base` is where
  the runtime loaded it. Relocated operands and PC-relative addresses use it;
  hardware addresses and low memory stay literal. The runtime applies the same
  relocations to the program's data when it loads the image.
- **Condition codes are computed exactly, every time,** with the helpers in
  `m68k.h`. Skipping flags that are never read is a later optimization.
- **Interrupt polls** (`M68K_POLL`) sit at every function entry and every backward
  `goto`, so interrupts land between instructions, inside any loop
  (`docs/runtime.md`).
- **Entry ranges** (`programs.toml`) become one C function that starts at
  whichever instruction it is asked to, plus a named entry point per instruction,
  so unrolled loops entered at a computed instruction cost linear code.
- **Code laid out before a function's entry** (a loop head that precedes it) is
  skipped on entry with a `goto` to the entry label.
- **Memory side effects stay in order.** Every operand read goes into its own
  temporary in program order. The source is always fetched before the
  destination address is computed, `movep` reads one byte per statement, and
  read-modify-write instructions compute their address once.

## Results

| Program | Functions | Lines of C |
|---|---|---|
| `cot.r` | 1,522 | 352,723 |
| `ealogo.r` | 3 | 12,463 |
| `ctnsnd.r` | 19 | 17,647 |
| `bootsnd.sys` | 9 | 2,017 |
| `bootcot.r` | 1 | 772 |

Each program's table file also carries its original image and relocations, which
the runtime loads into emulated memory. All of it compiles with no warnings.

Tail calls between functions are rare in `cot.r` and form no cycles, so they
cannot grow the C stack without bound, and no trampoline is needed for now.

## Tests

`tests/test_codegen.py` assembles 41 short programs by hand, runs them through
the real analyzer and code generator, compiles the result with `-Wall -Werror`
together with a small test runtime, and checks registers, flags, memory and the
stack against results worked out from the 68000 manual. The cases cover carry,
overflow and X; `addx` keeping Z; shift and rotate edge cases; multiply and
divide, including overflow and division by zero; BCD; `movem` order and sign
extension; byte pushes keeping a7 even; `movep`; `dbra`; calls, traps, DOS
calls, indirect calls; the `pea`-then-table-jump call; a loop body laid out
before its function's entry; and an entry range entered partway in. The test is
skipped when no C compiler is installed.

## Next

The runtime that links these functions into a program is in `docs/runtime.md`.
For the code generator itself:

- **Unknown jump targets:** `m68k_jump` needs a function to start exactly at the
  target; the runtime stops and names any address it meets that has none. Each
  one so far was a code shape analysis now recognizes (`docs/analysis.md`).
- **Speed:** skip condition codes nobody reads, and fast paths for RAM accesses.
  Not needed yet: the game runs at full speed.
