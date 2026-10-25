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
