import unittest

from tools.willow.analysis import analyze
from tools.willow.program import Program, decrypt


def program(code: str, relocations=(), roots=(), text_size=None) -> Program:
    image = bytes.fromhex(code.replace(" ", ""))
    return Program("test", image, image, text_size or len(image), 0, 0, frozenset(relocations), tuple(roots))


class AnalysisTests(unittest.TestCase):
    def test_calls_branches_and_function_pointers(self):
        p = program(
            "6106"  # 00 bsr.s $08
            "ff00"  # 02 dos _EXIT
            "00000010"  # 04 relocated pointer to $10
            "7001"  # 08 moveq #1,d0
            "6702"  # 0a beq.s $0e
            "4e71"  # 0c nop
            "4e75"  # 0e rts
            "4e71"  # 10 nop
            "4e75"  # 12 rts
            "ffffffff",  # 14 data
            relocations={0x04},
        )
        a = analyze(p)
        self.assertEqual(a.functions, {0x00: "entry", 0x08: "call", 0x10: "pointer"})
        self.assertEqual(sorted(a.insns), [0x00, 0x02, 0x08, 0x0A, 0x0C, 0x0E, 0x10, 0x12])
        self.assertEqual(a.labels, {0x0E})
        self.assertFalse(a.errors or a.rejected or a.unresolved)

    def test_branch_table_after_pc_indexed_jump(self):
        a = analyze(program(
            "4efb 0002"  # 00 jmp $04(pc,d0.w)
            "6000 0006"  # 04 bra.w $0c
            "6000 0004"  # 08 bra.w $0e
            "4e75"  # 0c rts
            "4e75"  # 0e rts
        ))
        self.assertEqual(a.tables, {0x00: [0x04, 0x08]})
        self.assertIn(0x0C, a.insns)
        self.assertIn(0x0E, a.insns)
        self.assertFalse(a.unresolved)

    def test_word_offset_table_bounded_by_mask(self):
        a = analyze(program(
            "c07c 0001"  # 00 and.w #1,d0
            "d040"  # 04 add.w d0,d0
            "303b 0006"  # 06 move.w $0e(pc,d0.w),d0
            "4efb 0002"  # 0a jmp $0e(pc,d0.w)
            "0004 0006"  # 0e offsets to $12 and $14
            "4e75"  # 12 rts
            "4e75"  # 14 rts
        ))
        self.assertEqual(a.tables, {0x0A: [0x12, 0x14]})
        self.assertFalse(a.errors)

    def test_word_offset_table_stops_where_its_code_begins(self):
        a = analyze(program(
            "c07c 0003"  # 00 and.w #3,d0: four entries allowed
            "d040"  # 04 add.w d0,d0
            "303b 0006"  # 06 move.w $0e(pc,d0.w),d0
            "4efb 0002"  # 0a jmp $0e(pc,d0.w)
            "0004 0004"  # 0e two offsets, both to $12
            "4e75"  # 12 rts: would be entry 2 if the mask were trusted
            "4e75"  # 14
        ))
        self.assertEqual(a.tables, {0x0A: [0x12, 0x12]})

    def test_unresolved_jump_and_indirect_call(self):
        a = analyze(program("4e90 4ed0"))  # jsr (a0); jmp (a0)
        self.assertEqual((a.indirect_calls, a.unresolved), ({0x0}, {0x2}))

    def test_rejects_pointers_that_are_not_code(self):
        a = analyze(program(
            "4e75"  # 00 rts
            "00000008 0000000c"  # 02 relocated pointers
            "a000"  # 0a line-A: not code
            "6000 fff2",  # 0c bra.w $00 ...
            relocations={0x02, 0x06},
        ))
        self.assertIn(0x08, a.rejected)  # mid-pointer bytes, decoding into the line-A word
        self.assertNotIn(0x08, a.functions)

    def test_trusted_roots_record_errors_instead_of_dropping_code(self):
        a = analyze(program("4e71 6002 a000 4e75", roots=[0x04]))
        self.assertIn(0x04, a.errors)
        self.assertIn(0x02, a.insns)


class DecryptTests(unittest.TestCase):
    def test_stages_chain_their_keys(self):
        image = bytes([1, 2, 3, 0x10, 0x20, 4, 0x30])
        stages = [{"key_sum": [0, 3], "range": [3, 5]}, {"key_sum": [5, 6], "range": [6, 7]}]
        # Stage 1: key 6, mask $60. Stage 2: key 6 ^ 4 = 2, mask $20.
        self.assertEqual(decrypt(image, stages), bytes([1, 2, 3, 0x70, 0x40, 4, 0x10]))

    def test_no_stages_is_a_copy(self):
        self.assertEqual(decrypt(b"\x12\x34", ()), b"\x12\x34")


if __name__ == "__main__":
    unittest.main()
