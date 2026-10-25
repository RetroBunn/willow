"""Generated C, compiled and run: each case is a hand-assembled snippet with the
result worked out from the 68000 manual. Needs a C compiler; skipped without one."""

import json
import shutil
import subprocess
import tempfile
import unittest
from dataclasses import dataclass, field
from pathlib import Path

from tools.willow import codegen
from tools.willow.analysis import analyze
from tools.willow.config import REPO_ROOT
from tools.willow.program import Program

CC = shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")
BASE, STACK, SENTINEL = 0x1000, 0x80000, 0xDEAD00
REGS = [f"d{n}" for n in range(8)] + [f"a{n}" for n in range(7)]


@dataclass
class Case:
    name: str
    code: str
    init: dict = field(default_factory=dict)  # registers, plus x/n/z/v/c
    expect: dict = field(default_factory=dict)
    relocations: tuple = ()
    roots: tuple = ()
    entry: int = 0
    ranges: tuple = ()


CASES = [
    Case("add_b_overflow", "d001 4e75", {"d0": 0x1234567F, "d1": 1},
         {"d0": 0x12345680, "n": 1, "z": 0, "v": 1, "c": 0, "x": 0}),
    Case("add_l_carry", "d081 4e75", {"d0": 0xFFFFFFFF, "d1": 1},
         {"d0": 0, "n": 0, "z": 1, "v": 0, "c": 1, "x": 1}),
    Case("sub_w_borrow", "9041 4e75", {"d0": 0xAAAA0000, "d1": 1},
         {"d0": 0xAAAAFFFF, "n": 1, "z": 0, "v": 0, "c": 1, "x": 1}),
    Case("cmp_l_overflow_keeps_x", "b081 4e75", {"d0": 0x80000000, "d1": 1, "x": 1},
         {"d0": 0x80000000, "n": 0, "z": 0, "v": 1, "c": 0, "x": 1}),
    Case("addx_zero_keeps_z", "d181 4e75", {"d0": 0, "d1": 0xFFFFFFFF, "x": 1, "z": 1},
         {"d0": 0, "z": 1, "c": 1, "x": 1, "v": 0, "n": 0}),
    Case("addx_nonzero_clears_z", "d181 4e75", {"d0": 1, "d1": 1, "z": 1},
         {"d0": 2, "z": 0, "c": 0, "x": 0}),
    Case("neg_w", "4440 4e75", {"d0": 1}, {"d0": 0xFFFF, "n": 1, "z": 0, "v": 0, "c": 1, "x": 1}),
    Case("neg_w_overflow", "4440 4e75", {"d0": 0x8000}, {"d0": 0x8000, "n": 1, "v": 1, "c": 1}),
    Case("lsl_w_8", "e148 4e75", {"d0": 0x181}, {"d0": 0x8100, "c": 1, "x": 1, "n": 1, "z": 0, "v": 0}),
    Case("asr_l_1", "e280 4e75", {"d0": 0x80000001}, {"d0": 0xC0000000, "c": 1, "x": 1, "n": 1}),
    Case("roxl_w_through_x", "e350 4e75", {"d0": 0x8000, "x": 1},
         {"d0": 0x0001, "x": 1, "c": 1, "n": 0, "z": 0}),
    Case("rol_l_by_register_keeps_x", "e3b8 4e75", {"d0": 0x12345678, "d1": 4, "x": 1},
         {"d0": 0x23456781, "c": 1, "x": 1, "n": 0, "cycles": 8 + 2 * 4 + 16}),
    Case("lsr_count_zero", "e2a8 4e75", {"d0": 5, "d1": 0, "x": 1, "c": 1},
         {"d0": 5, "c": 0, "x": 1, "n": 0, "z": 0, "v": 0}),
    Case("mulu", "c0c1 4e75", {"d0": 0xFFFF, "d1": 0xFFFF},
         {"d0": 0xFFFE0001, "n": 1, "z": 0, "v": 0, "c": 0, "cycles": 38 + 2 * 16 + 16}),
    Case("muls", "c1c1 4e75", {"d0": 0xFFFF, "d1": 2}, {"d0": 0xFFFFFFFE, "n": 1, "cycles": 38 + 2 * 2 + 16}),
    Case("divu", "80c1 4e75", {"d0": 100007, "d1": 10}, {"d0": 0x00072710, "n": 0, "z": 0, "v": 0, "c": 0}),
    Case("divu_overflow", "80c1 4e75", {"d0": 0x00100000, "d1": 1}, {"d0": 0x00100000, "v": 1, "c": 0}),
    Case("divs_negative", "81c1 4e75", {"d0": 0xFFFFFFF9, "d1": 2}, {"d0": 0xFFFFFFFD, "n": 1, "v": 0}),
    Case("divu_by_zero", "80c1 4e75", {"d0": 5, "d1": 0}, {"d0": 5, "vector": 5}),
    Case("movem_round_trip", "48e7 c080 7000 7200 91c8 4cdf 0103 4e75", {"d0": 1, "d1": 2, "a0": 3},
         {"d0": 1, "d1": 2, "a0": 3}),
    Case("movem_w_sign_extends", "3f3c 8000 4c9f 0004 4e75", {}, {"d2": 0xFFFF8000}),
    Case("ext_w_then_l", "4880 48c0 4e75", {"d0": 0xF0}, {"d0": 0xFFFFFFF0, "n": 1}),
    Case("swap", "4840 4e75", {"d0": 0x12345678}, {"d0": 0x56781234, "n": 0, "z": 0}),
    Case("bset_then_btst", "08c0 001f 0800 001f 4e75", {}, {"d0": 0x80000000, "z": 0}),
    # Four addq.l (8 each), dbra taken three times (10) and expiring once (14), rts (16).
    Case("dbra_loop", "5280 51c9 fffc 4e75", {"d1": 3}, {"d0": 4, "d1": 0xFFFF, "cycles": 4 * 8 + 3 * 10 + 14 + 16}),
    Case("beq_taken", "6702 7001 4e75", {"z": 1}, {"d0": 0, "cycles": 10 + 16}),
    Case("beq_not_taken", "6702 7001 4e75", {}, {"d0": 1, "cycles": 8 + 4 + 16}),
    Case("bsr_and_return", "6104 5280 4e75 5481 4e75", {}, {"d0": 1, "d1": 2}),
    # pea pushes a return address, then a table jump "calls" a handler that returns to it.
    Case("pea_then_table_jump", "4879 0000000e 4efb 0002 6000 0008 5281 4e75 4e71 5482 4e75", {},
         {"d1": 1, "d2": 2}, relocations=(0x02,)),
    Case("seq", "57c0 4e75", {"d0": 0x12345678, "z": 1}, {"d0": 0x123456FF}),
    Case("byte_pop_keeps_a7_even", "3f3c ab00 101f 4e75", {}, {"d0": 0xAB}),
    Case("movea_adda_sign_extend", "3040 d2c1 4e75", {"d0": 0x8000, "d1": 0xFFFF, "a1": 0x10000},
         {"a0": 0xFFFF8000, "a1": 0x0000FFFF}),
    Case("cmpa_w", "b0c0 4e75", {"a0": 0xFFFF8000, "d0": 0x8000}, {"z": 1, "n": 0, "c": 0, "v": 0}),
    Case("movep_l_round_trip", "01c8 0000 0348 0000 4e75", {"a0": 0x20000, "d0": 0x11223344},
         {"d1": 0x11223344, "mem": [0x11, 0, 0x22, 0, 0x33, 0, 0x44, 0]}),
    Case("trap_resumes", "7021 4e4f 7201 4e75", {}, {"trap": 15, "trap_d0": 0x21, "d1": 1}),
    Case("dos_call_resumes", "ff09 7201 4e75", {}, {"line_f": 0xFF09, "d1": 1}),
    Case("abcd", "c101 4e75", {"d0": 0x45, "d1": 0x38}, {"d0": 0x83, "c": 0, "x": 0}),
    Case("abcd_carry_keeps_z", "c101 4e75", {"d0": 0x99, "d1": 0x01, "z": 1}, {"d0": 0x00, "c": 1, "x": 1, "z": 1}),
    Case("sbcd", "8101 4e75", {"d0": 0x45, "d1": 0x38}, {"d0": 0x07, "c": 0}),
    Case("and_then_eor", "c07c 0ff0 b181 4e75", {"d0": 0xF0F0, "d1": 0xFFFF0000},
         {"d0": 0x00F0, "d1": 0xFFFF00F0, "n": 1, "z": 0, "v": 0, "c": 0}),
    Case("indirect_call", "41fa 0008 4e90 4e75 4e71 7405 4e75", {}, {"d2": 5}, roots=(0x0A,)),
    # The function starts at 4, but its loop body sits before it at 0: the C must begin at 4.
    Case("loop_body_before_entry", "5281 6002 7003 51c8 fff8 4e75", {}, {"d1": 3, "d0": 0xFFFF}, entry=0x04),
    # An unrolled loop entered partway in, as a computed jsr does: two of the four addq run.
    Case("entry_range", "5280 5280 5280 5280 4e75", {}, {"d0": 2}, entry=0x04, ranges=((0x00, 0x0A),)),
]

HARNESS = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "willow/m68k.h"
%(includes)s

static uint8_t *mem;
uint8_t m68k_read8(uint32_t a) { return mem[a & 0xFFFFFF]; }
uint16_t m68k_read16(uint32_t a) { return (uint16_t)(m68k_read8(a) << 8 | m68k_read8(a + 1)); }
uint32_t m68k_read32(uint32_t a) { return (uint32_t)m68k_read16(a) << 16 | m68k_read16(a + 2); }
void m68k_write8(uint32_t a, uint8_t v) { mem[a & 0xFFFFFF] = v; }
void m68k_write16(uint32_t a, uint16_t v) { m68k_write8(a, (uint8_t)(v >> 8)); m68k_write8(a + 1, (uint8_t)v); }
void m68k_write32(uint32_t a, uint32_t v) { m68k_write16(a, (uint16_t)(v >> 16)); m68k_write16(a + 2, (uint16_t)v); }

typedef struct {
    const char *name; uint32_t *base; const M68kFuncEntry *funcs; const size_t *count;
    M68kFunc entry; const uint8_t *image; size_t size; uint32_t regs[15]; uint8_t ccr;
} Case;
static const Case *current;
volatile uint32_t m68k_pending;
void m68k_service(M68kContext *c, uint32_t pc) { (void)c; (void)pc; }
static int last_trap, last_vector;
static unsigned last_line_f;
static uint32_t trap_d0;

void m68k_trap(M68kContext *c, int n, uint32_t ret) { last_trap = n; trap_d0 = c->d[0]; c->ret = ret; }
void m68k_line_f(M68kContext *c, uint16_t op, uint32_t ret) { last_line_f = op; c->ret = ret; }
void m68k_exception(M68kContext *c, int v, uint32_t ret) { last_vector = v; c->ret = ret; }
void m68k_set_sr(M68kContext *c, uint16_t sr) { c->sr_hi = (uint8_t)(sr >> 8); m68k_set_ccr(c, (uint8_t)sr); }
void m68k_stop(M68kContext *c, uint16_t sr) { m68k_set_sr(c, sr); }
void m68k_reset(M68kContext *c) { (void)c; }
void m68k_jump(M68kContext *c, uint32_t target)
{
    for (size_t i = 0; i < *current->count; i++)
        if (*current->base + current->funcs[i].offset == target) { current->funcs[i].func(c); return; }
    fprintf(stderr, "%%s: no function at %%x\n", current->name, (unsigned)target);
    exit(3);
}

%(images)s
static const Case cases[] = {
%(cases)s
};

int main(void)
{
    mem = calloc(1, 1 << 24);
    for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        const Case *t = current = &cases[k];
        M68kContext ctx;
        memset(mem, 0, 1 << 24);
        memset(&ctx, 0, sizeof ctx);
        last_trap = last_vector = -1;
        last_line_f = 0;
        trap_d0 = 0;
        *t->base = %(base)d;
        memcpy(mem + %(base)d, t->image, t->size);
        for (int r = 0; r < 8; r++) ctx.d[r] = t->regs[r];
        for (int r = 0; r < 7; r++) ctx.a[r] = t->regs[8 + r];
        ctx.a[7] = %(stack)d;
        m68k_push32(&ctx, %(sentinel)d);
        m68k_set_ccr(&ctx, t->ccr);
        t->entry(&ctx);
        printf("{\"name\": \"%%s\", \"d\": [", t->name);
        for (int r = 0; r < 8; r++) printf("%%s%%u", r ? ", " : "", (unsigned)ctx.d[r]);
        printf("], \"a\": [");
        for (int r = 0; r < 8; r++) printf("%%s%%u", r ? ", " : "", (unsigned)ctx.a[r]);
        printf("], \"x\": %%d, \"n\": %%d, \"z\": %%d, \"v\": %%d, \"c\": %%d", ctx.x, ctx.n, ctx.z, ctx.v, ctx.c);
        printf(", \"ret\": %%u, \"trap\": %%d, \"trap_d0\": %%u, \"vector\": %%d, \"line_f\": %%u, \"cycles\": %%llu",
               (unsigned)ctx.ret, last_trap, (unsigned)trap_d0, last_vector, last_line_f, (unsigned long long)ctx.cycles);
        printf(", \"mem\": [");
        for (int i = 0; i < 8; i++) printf("%%s%%d", i ? ", " : "", mem[0x20000 + i]);
        printf("]}\n");
    }
    return 0;
}
"""


@unittest.skipUnless(CC, "no C compiler found")
class GeneratedCodeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp())
        sources, includes, images, rows = [], [], [], []
        for i, case in enumerate(CASES):
            name = f"c{i}"
            image = bytes.fromhex(case.code.replace(" ", ""))
            p = Program(name, image, image, len(image), 0, case.entry, frozenset(case.relocations), tuple(case.roots),
                        "", case.ranges)
            gen = codegen.generate(analyze(p))
            sources += codegen.write(gen, cls.tmp / name)
            includes.append(f'#include "{name}/{name}.h"')
            images.append(f"static const uint8_t img{i}[] = {{{', '.join(str(b) for b in image)}}};")
            regs = ", ".join(str(case.init.get(r, 0)) for r in REGS)
            ccr = sum(case.init.get(f, 0) << bit for f, bit in (("x", 4), ("n", 3), ("z", 2), ("v", 1), ("c", 0)))
            rows.append(f'    {{"{case.name}", &{name}_base, {name}_functions, &{name}_function_count, '
                        f"{name}_{case.entry:06x}, img{i}, sizeof img{i}, {{{regs}}}, {ccr}}},")
        main = cls.tmp / "main.c"
        main.write_text(HARNESS % {
            "includes": "\n".join(includes), "images": "\n".join(images), "cases": "\n".join(rows),
            "base": BASE, "stack": STACK, "sentinel": SENTINEL,
        }, encoding="utf-8")
        exe = cls.tmp / "cases.exe"
        include_dirs = ["-I", str(REPO_ROOT / "runtime" / "include")]
        include_dirs += [arg for i in range(len(CASES)) for arg in ("-I", str(cls.tmp / f"c{i}"))]
        build = subprocess.run(
            [CC, "-std=c11", "-O1", "-Wall", "-Werror", "-Wno-unused-label", *include_dirs, "-I", str(cls.tmp),
             str(main), *map(str, sources), "-o", str(exe)],
            capture_output=True, text=True,
        )
        if build.returncode:
            raise AssertionError("generated C failed to compile:\n" + build.stderr[:4000])
        run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
        if run.returncode:
            raise AssertionError(f"harness failed ({run.returncode}): {run.stderr}")
        cls.results = {r["name"]: r for r in map(json.loads, run.stdout.splitlines())}

    def test_cases(self):
        for case in CASES:
            with self.subTest(case.name):
                got = self.results[case.name]
                self.assertEqual(got["ret"], SENTINEL, "did not return to the caller")
                self.assertEqual(got["a"][7], STACK, "stack not balanced")
                for key, want in case.expect.items():
                    if key[0] in "da" and key[1:].isdigit():
                        value = got[key[0]][int(key[1:])]
                    else:
                        value = got[key]
                    self.assertEqual(value, want, f"{key}: got {value:#x}" if isinstance(value, int) else key)


if __name__ == "__main__":
    unittest.main()
