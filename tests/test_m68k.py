import unittest

from tools.willow.m68k import DecodeError, Flow, Mode, decode

# (encoding, text, flow, target), all decoded at address 0.
VALID = [
    ("4e75", "rts", Flow.RETURN, None),
    ("4e71", "nop", Flow.NEXT, None),
    ("7001", "moveq #$1,d0", Flow.NEXT, None),
    ("70ff", "moveq #-$1,d0", Flow.NEXT, None),
    ("2200", "move.l d0,d1", Flow.NEXT, None),
    ("3e3c1234", "move.w #$1234,d7", Flow.NEXT, None),
    ("13fc008000e9a005", "move.b #$80,$e9a005.l", Flow.NEXT, None),
    ("2030 1804", "move.l $4(a0,d1.l),d0", Flow.NEXT, None),
    ("41fa fffe", "lea $0(pc),a0", Flow.NEXT, None),
    ("48e7 fffe", "movem.l d0-d7/a0-a6,-(a7)", Flow.NEXT, None),
    ("4cdf 7fff", "movem.l (a7)+,d0-d7/a0-a6", Flow.NEXT, None),
    ("51c8 fffe", "dbra d0,$0", Flow.BRANCH, 0x0),
    ("6700 0010", "beq $12", Flow.BRANCH, 0x12),
    ("60fe", "bra $0", Flow.JUMP, 0x0),
    ("4eb9 00001000", "jsr $1000.l", Flow.CALL, 0x1000),
    ("4ed0", "jmp (a0)", Flow.JUMP, None),
    ("e148", "lsl.w #$8,d0", Flow.NEXT, None),
    ("e2a9", "lsr.l d1,d1", Flow.NEXT, None),
    ("d1c8", "adda.l a0,a0", Flow.NEXT, None),
    ("c340", "exg d1,d0", Flow.NEXT, None),
    ("0839 0003 00e88001", "btst.b #$3,$e88001.l", Flow.NEXT, None),
    ("0100", "btst.l d0,d0", Flow.NEXT, None),
    ("0188 0002", "movep.w d0,$2(a0)", Flow.NEXT, None),
    ("46fc 2700", "move.w #$2700,sr", Flow.NEXT, None),
    ("027c f8ff", "andi.w #$f8ff,sr", Flow.NEXT, None),
    ("0000 00ff", "ori.b #$ff,d0", Flow.NEXT, None),
    ("0000 ffff", "ori.b #$ff,d0", Flow.NEXT, None),
    ("4e72 2700", "stop #$2700", Flow.NEXT, None),
    ("4e4f", "trap #$f", Flow.NEXT, None),
    ("ff09", "dos #$9", Flow.NEXT, None),
    ("ff00", "dos #$0", Flow.STOP, None),
    ("4afc", "illegal", Flow.STOP, None),
]

INVALID = [
    ("6101", "odd branch target"),
    ("2030 1a04", "68020 index extension word"),
    ("4e7a", "line-4"),
    ("a000", "line-A"),
    ("1040", "movea.b"),
    ("1048", "addressing mode"),  # a byte-sized address register source
    ("0000 4268", "stray high byte"),
    ("0000 ff7f", "stray high byte"),
    ("4eb9 0000", "past the end"),
    ("e0c0", "addressing mode"),  # memory shift with a data register operand
    ("e8c0", "bit field"),
    ("4100", "chk.l"),
]


def mem(encoding: str) -> bytes:
    return bytes.fromhex(encoding.replace(" ", ""))


class DecodeTests(unittest.TestCase):
    def test_valid_instructions(self):
        for encoding, text, flow, target in VALID:
            with self.subTest(encoding):
                insn = decode(mem(encoding), 0)
                self.assertEqual(" ".join(str(insn).split()), text)
                self.assertEqual(insn.length, len(mem(encoding)))
                self.assertEqual(insn.flow, flow)
                self.assertEqual(insn.target, target)

    def test_invalid_encodings(self):
        for encoding, reason in INVALID:
            with self.subTest(encoding):
                with self.assertRaises(DecodeError) as cm:
                    decode(mem(encoding), 0)
                self.assertIn(reason, cm.exception.reason)

    def test_pc_relative_operands_resolve_against_the_extension_word(self):
        insn = decode(bytes(0x100) + mem("4efb 0002"), 0x100)  # jmp d8(pc,d0.w) at 0x100
        op = insn.operands[0]
        self.assertEqual((op.mode, op.value, op.index, op.ext), (Mode.PCINDEX, 0x104, 0, 0x102))

    def test_decodes_at_an_address_relative_to_a_base(self):
        insn = decode(mem("6002 4e71 4e75"), 0x1000, base=0x1000)
        self.assertEqual(insn.target, 0x1004)

    def test_operand_extension_addresses(self):
        insn = decode(mem("13fc008000e9a005"), 0)
        self.assertEqual([o.ext for o in insn.operands], [2, 4])


if __name__ == "__main__":
    unittest.main()
