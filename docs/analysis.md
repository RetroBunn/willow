# Recompiler front end

The front end turns each program into a map of its code: every instruction,
where functions start, and where jumps go. It is the first half of the
recompiler; the second half will turn that map into C.

```sh
python -m tools.willow analyze
```

This writes a labeled listing (`<name>.lst`) and a summary for the code
generator (`<name>.json`) per program to `build/analysis/`. Both are derived
from the game and stay in the gitignored `build/` folder.

## Pieces

| File | Role |
|---|---|
| `programs.toml` | Which programs to recompile, plus what analysis can't find alone: extra roots and decryption stages. Offsets only. |
| `tools/willow/program.py` | Loads each program as it sits in memory (`image`), and the view the analyzer decodes (`code`). |
| `tools/willow/m68k.py` | MC68000 instruction decoder. Rejects 68010+ encodings, which helps tell code from data. Decodes Human68k DOS calls (`$FFxx`) and FLOAT calls (`$FExx`) as instructions. |
| `tools/willow/analysis.py` | Recursive descent from the entry point and roots, jump-table resolution, function pointer discovery. |

## How code is found

1. **Trusted descent.** Start at the entry point and the roots in
   `programs.toml`. Follow every branch, call and static jump. An absolute
   `jmp`/`jsr` counts as internal only if its operand is relocated; otherwise it
   points at ROM or the system area.
2. **Jump tables.** These shapes cover every table found so far:
   - `jmp d8(pc,dn)` into slots of code. When the index was scaled in the same
     run (`add.w dn,dn`, `lsl.w #k,dn`), the slot size follows from it and an
     earlier `cmp.w #n,dn` bounds the count; slots hold a `bra` or an `rts` for
     "nothing to do". When the index arrives pre-scaled from memory, the table is
     the run of `bra` instructions at the base, after any `nop`/`rts` padding.
     Some tables are based on the `jmp` itself (`jmp *(pc,dn)`). These are the
     state machines in `cot.r`.
   - `move.w T(pc,dn),dn` then `jmp/jsr B(pc,dn)`: word offsets from B. The
     entry count comes from an earlier `and.w #mask,dn`, and the table can't run
     into the code it jumps to. These are the sound driver's command tables.
   - **Entry ranges**, declared in `programs.toml`: unrolled loops the game enters
     at a computed instruction by counting back from their `rts`
     (`jsr 0(a2,d0.w)`). Every instruction in a range is an entry point. `cot.r`
     copies its sprite list this way.
3. **Function pointers.** Every relocated longword that points into the code
   section is tried as an entry point. A candidate is kept only if everything
   reachable from it decodes cleanly and lines up with code already found.

Trusted code may leave the header's code section: `cot.r` keeps a small file
I/O module at the start of its data section.

## The self-decrypting startup

`cot.r`'s startup decrypts two stages of itself in place before running them:
first [0x3b8, 0x3fe), then [0x3fe, 0x6e8). Each stage XORs its bytes with a
nibble-swapped key built from byte sums of the decryptor code, so editing the
decryptor changes the key. `programs.toml` lists both stages, and `program.py`
applies them to produce the `code` view.

There are two views on purpose:

- **The recompiler translates `code`**, the decrypted bytes.
- **The runtime loads `image`**, the original bytes, into emulated memory. The
  recompiled decryptor still runs at startup and still rewrites those bytes,
  just as on real hardware. Loading pre-decrypted bytes would decrypt them a
  second time.

The decrypted stage also contains a disk-check routine (0x40e–0x6e8) that reads
raw sectors. Nothing in this build calls it: the stage's first instruction
branches past it. So the runtime does not need raw sector access for it.

## Results

| | `cot.r` | `ctnsnd.r` | `bootsnd.sys` |
|---|---|---|---|
| Instructions | 53,950 | 1,848 | 332 |
| Functions | 1,522 (1 entry, 410 called, 613 from pointers, 496 in entry ranges, 2 range starts) | 19 (1 entry, 4 roots, 14 called) | 9 (1 entry, 2 roots, 6 called) |
| Code section covered | 92.4% | code sits at the front, data after | code sits at the front, data after |
| Jump tables | 331 | 2 | 1 |
| Unresolved jumps | 3 | 0 | 0 |
| Decode errors | 0 | 0 | 0 |

`ealogo.r` (1,225 instructions) and `bootcot.r` (155) analyze cleanly too.

Running the game showed what static analysis alone missed: the two unrolled sprite
loops (now entry ranges) and three table shapes (now handled above). When the
runtime meets a jump to an address with no recompiled function, it stops and
names the address; that is the cue to look for the next such shape.

The three unresolved jumps go through function pointers stored in RAM, at
`$df2f0` and a raster-interrupt handler chain at `$df36e`. Their targets get
there from relocated pointers, which step 3 already covers. The largest
uncovered stretches of `cot.r` are data: tables, and the disk check's 1 KB
sector buffer at 0x6fa.

The decoder was checked against capstone at all 53,994 instruction boundaries
the analysis found in `cot.r` and `ctnsnd.r`. Mnemonics and lengths agree everywhere. The only
differences are cosmetic: capstone omits zero displacements and prints `moveq`
immediates unsigned. capstone is not a dependency; this was a one-off check.

## Next

- The three RAM-pointer jumps and the indirect calls go through `m68k_jump`, a
  runtime lookup from address to recompiled function ([`codegen.md`](codegen.md)).
- Confirm the pointer-found functions are real. A few could be data that
  happens to decode cleanly. They are harmless as long as nothing calls them,
  since each one compiles to its own C function.
