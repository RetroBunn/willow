import tempfile
import unittest
from pathlib import Path

from tests.fixtures import build_image, wrap_dim
from tools.willow import disk

FILES = {
    "cotsys": b"",
    "config.sys": b"SHELL = bootcot.r\r\n",
    "big.bin": bytes(range(256)) * 9,  # spans three clusters
    "xobj/o0opt.dt": b"object data",
    "xobj/o1opt.dt": b"more",
}


class ImageTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(self.enterContext(tempfile.TemporaryDirectory()))
        self.image = build_image(FILES)

    def write(self, name: str, data: bytes) -> Path:
        path = self.tmp / name
        path.write_bytes(data)
        return path

    def test_dim_and_xdf_decode_to_the_same_image(self):
        self.assertEqual(disk.load_image(self.write("a.xdf", self.image)), self.image)
        self.assertEqual(disk.load_image(self.write("a.dim", wrap_dim(self.image))), self.image)

    def test_dim_omitted_tracks_read_back_unformatted(self):
        last = disk.TRACKS - 1
        loaded = disk.load_image(self.write("a.dim", wrap_dim(self.image, frozenset({last}))))
        self.assertEqual(loaded[: last * disk.TRACK_SIZE], self.image[: last * disk.TRACK_SIZE])
        self.assertEqual(loaded[last * disk.TRACK_SIZE :], b"\xe5" * disk.TRACK_SIZE)

    def test_rejects_truncated_dim(self):
        with self.assertRaisesRegex(disk.DiskError, "header implies"):
            disk.load_image(self.write("a.dim", wrap_dim(self.image)[:-1]))

    def test_rejects_unknown_format(self):
        with self.assertRaisesRegex(disk.DiskError, "not a 2HD DIM or XDF"):
            disk.load_image(self.write("a.img", b"\0" * 1000))


class VolumeTests(unittest.TestCase):
    def setUp(self):
        self.volume = disk.Volume(build_image(FILES))

    def test_walk_lists_every_file_and_directory(self):
        paths = [p for p, _ in self.volume.walk()]
        self.assertEqual(
            paths, ["/cotsys", "/config.sys", "/big.bin", "/xobj", "/xobj/o0opt.dt", "/xobj/o1opt.dt"]
        )

    def test_reads_files_across_clusters_and_in_subdirectories(self):
        for path, data in FILES.items():
            self.assertEqual(self.volume.read(path), data)

    def test_lookup_is_case_insensitive(self):
        self.assertEqual(self.volume.read("XOBJ/O1OPT.DT"), b"more")

    def test_missing_file(self):
        with self.assertRaisesRegex(disk.DiskError, "no such file"):
            self.volume.read("xobj/o9opt.dt")

    def test_entry_timestamp(self):
        self.assertEqual(self.volume.find("cotsys").timestamp, "1993-08-27 00:00:00")

    def test_detects_fat_loop(self):
        image = bytearray(build_image(FILES))
        fat = disk.RESERVED_SECTORS * disk.SECTOR_SIZE
        big = disk.Volume(bytes(image)).find("big.bin")
        # Point big.bin's second cluster back at its first.
        n = big.cluster + 1
        off = fat + n * 3 // 2
        image[off] = (image[off] & 0x0F) | (big.cluster << 4 & 0xF0)
        image[off + 1] = big.cluster >> 4
        with self.assertRaisesRegex(disk.DiskError, "loops"):
            disk.Volume(bytes(image)).read("big.bin")


if __name__ == "__main__":
    unittest.main()
