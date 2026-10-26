import unittest

from tests.fixtures import ABC_PROGRAM, build_lzx
from tools.willow import lzx


class UnpackTests(unittest.TestCase):
    def test_literals_and_long_match(self):
        u = lzx.unpack(ABC_PROGRAM)
        self.assertEqual(u.image, b"abcabcabc")
        self.assertEqual((u.text_size, u.data_size, u.bss_size, u.relocations), (6, 3, 0, ()))

    def test_short_match(self):
        # Flags 1100_0101: literal x, y; short match -2 x3; end marker.
        u = lzx.unpack(build_lzx(bytes.fromhex("c5 7879 fe 0000 00"), image_size=5, text_size=5))
        self.assertEqual(u.image, b"xyxyx")

    def test_long_match_with_length_byte_and_relocations(self):
        # Flags 1010_1000: literal 0; long match -1 x256; end marker.
        # Then relocation deltas 200 (two bytes: 128 + 72) and 4, and the terminator.
        stream = bytes.fromhex("a8 00 fff8ff 0000 00") + bytes.fromhex("0190 08 00")
        u = lzx.unpack(build_lzx(stream, image_size=257, text_size=257, bss_size=16, reloc_size=4))
        self.assertEqual(u.image, bytes(257))
        self.assertEqual(u.relocations, (200, 204))
        self.assertEqual(u.bss_size, 16)

    def test_unpack_time(self):
        # Each file is $a4 + stream + $ba bytes; the set-up moves all but the first $a4 up,
        # 8 bytes a pass: 25 passes of 60 cycles for these. Then the stub's 440 cycles of
        # set-up, 18 per flag bit, 10 per extra flag byte, 30 per literal, 18 per match or
        # end marker, the match or end itself, 22 per byte copied, 14 for no relocations
        # (or 36 + 82 each + 64 per extra delta byte), 28 per bss longword, and 110 to finish.
        head = 440 + 25 * 60
        # 7 bits; literals a, b, c; a long match of 6; the end.
        self.assertEqual(lzx.unpack(ABC_PROGRAM).cycles, head + 7 * 18 + 3 * 30 + 2 * 18 + 102 + 6 * 22 + 96 + 14 + 110)
        # 8 bits; literals x, y; a short match of 3; the end.
        short = build_lzx(bytes.fromhex("c5 7879 fe 0000 00"), image_size=5, text_size=5)
        self.assertEqual(lzx.unpack(short).cycles, head + 8 * 18 + 2 * 30 + 2 * 18 + 46 + 3 * 22 + 96 + 14 + 110)
        # 10 bits, so a second flag byte; eight literals; the end.
        refill = build_lzx(bytes.fromhex("ff 0102030405060708 40 0000 00"), image_size=8, text_size=8)
        self.assertEqual(lzx.unpack(refill).cycles, head + 10 * 18 + 10 + 8 * 30 + 18 + 96 + 14 + 110)
        # 5 bits; a literal; a long match of 256 with a length byte; the end; relocations
        # at 200 (a two-byte delta) and 204; 16 bytes of bss.
        stream = bytes.fromhex("a8 00 fff8ff 0000 00") + bytes.fromhex("0190 08 00")
        relocated = build_lzx(stream, image_size=257, text_size=257, bss_size=16, reloc_size=4)
        self.assertEqual(
            lzx.unpack(relocated).cycles,
            head + 5 * 18 + 30 + 2 * 18 + 114 + 256 * 22 + 96 + 36 + 2 * 82 + 64 + 4 * 28 + 110,
        )

    def test_rejects_match_before_start_of_output(self):
        with self.assertRaisesRegex(lzx.LzxError, "before the start"):
            lzx.unpack(build_lzx(bytes.fromhex("40 fff8ff"), image_size=256, text_size=0))

    def test_rejects_stream_without_end_marker(self):
        with self.assertRaisesRegex(lzx.LzxError, "runs into the unpack routine"):
            lzx.unpack(build_lzx(bytes.fromhex("ea 616263"), image_size=3, text_size=3))

    def test_rejects_header_size_mismatch(self):
        with self.assertRaisesRegex(lzx.LzxError, "header size"):
            lzx.unpack(build_lzx(bytes.fromhex("ea 616263 ffec 0000 00"), image_size=10, text_size=6))

    def test_rejects_unpacked_file(self):
        self.assertFalse(lzx.is_lzx(b"\x60\x00" + bytes(200)))
        with self.assertRaises(lzx.LzxError):
            lzx.unpack(b"\x60\x00" + bytes(200))


if __name__ == "__main__":
    unittest.main()
