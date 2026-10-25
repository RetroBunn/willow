import unittest

from tools.willow.m68k import decode
from tools.willow.timing import cycles

# (encoding, cycles) from the M68000 User's Manual, section 8; data-dependent parts excluded.
CASES = [
    ("4e71", 4),                  # nop
    ("7001", 4),                  # moveq #1,d0
    ("2200", 4),                  # move.l d0,d1
    ("3e3c 1234", 8),             # move.w #$1234,d7
    ("13fc 0080 00e9a005", 20),   # move.b #$80,$e9a005.l
    ("2030 1804", 18),            # move.l 4(a0,d1.l),d0
    ("2f00", 12),                 # move.l d0,-(a7)
    ("20d9", 20),                 # move.l (a1)+,(a0)+
    ("3080", 8),                  # move.w d0,(a0)
    ("2079 00001000", 20),        # movea.l $1000.l,a0
    ("41fa fffe", 8),             # lea 0(pc),a0
    ("4879 00001000", 20),        # pea $1000.l
    ("48e7 fffe", 8 + 8 * 15),    # movem.l d0-d7/a0-a6,-(a7)
    ("4cdf 7fff", 12 + 8 * 15),   # movem.l (a7)+,d0-d7/a0-a6
    ("4cd0 00ff", 12 + 8 * 8),    # movem.l (a0),d0-d7
    ("6000 0010", 10),            # bra.w
    ("4eb9 00001000", 20),        # jsr $1000.l
    ("4ed0", 8),                  # jmp (a0)
    ("4e75", 16),                 # rts
    ("4e73", 20),                 # rte
    ("e148", 6 + 2 * 8),          # lsl.w #8,d0
    ("e388", 8 + 2),              # lsl.l #1,d0
    ("e2a9", 8),                  # lsr.l d1,d1: plus 2 per bit at run time
    ("e0d0", 12),                 # asr.w (a0)
    ("d1c8", 8),                  # adda.l a0,a0
    ("d0fc 0080", 12),            # adda.w #$80,a0
    ("d081", 8),                  # add.l d1,d0
    ("d090", 14),                 # add.l (a0),d0
    ("d150", 12),                 # add.w d0,(a0)
    ("0680 00010000", 16),        # addi.l #$10000,d0
    ("0640 0001", 8),             # addi.w #1,d0
    ("5340", 4),                  # subq.w #1,d0
    ("5388", 8),                  # subq.l #1,a0
    ("5279 00001000", 20),        # addq.w #1,$1000.l
    ("b081", 6),                  # cmp.l d1,d0
    ("0c40 0005", 8),             # cmpi.w #5,d0
    ("0c80 00000005", 14),        # cmpi.l #5,d0
    ("4a39 000df322", 16),        # tst.b $df322.l
    ("4a2e 0002", 12),            # tst.b 2(a6)
    ("4240", 4),                  # clr.w d0
    ("4280", 6),                  # clr.l d0
    ("4239 000df322", 20),        # clr.b $df322.l
    ("50f9 000df322", 20),        # st.b $df322.l
    ("c340", 6),                  # exg d1,d0
    ("4840", 4),                  # swap d0
    ("0839 0003 00e88001", 20),   # btst.b #3,$e88001.l
    ("0100", 6),                  # btst.l d0,d0
    ("08c0 0003", 12),            # bset.l #3,d0
    ("0880 0003", 14),            # bclr.l #3,d0
    ("0188 0002", 16),            # movep.w d0,2(a0)
    ("46fc 2700", 16),            # move.w #$2700,sr
    ("c0c1", 38),                 # mulu.w d1,d0: plus 2 per set bit at run time
    ("80c1", 128),                # divu.w d1,d0: a typical figure
    ("4e56 fff0", 16),            # link a6,#-16
    ("4e5e", 12),                 # unlk a6
    ("4e40", 34),                 # trap #0
    ("ff09", 34),                 # DOS _PRINT: the line-F trap
    ("6700 0010", 0),             # beq.w: the generated code counts taken or not
    ("51c8 fffe", 0),             # dbra: likewise
]


class TimingTest(unittest.TestCase):
    def test_cases(self):
        for hexcode, expected in CASES:
            insn = decode(bytes.fromhex(hexcode), 0)
            with self.subTest(insn=str(insn)):
                self.assertEqual(cycles(insn), expected)


if __name__ == "__main__":
    unittest.main()
