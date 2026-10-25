import contextlib
import io
import tempfile
import unittest
from pathlib import Path

from tests.fixtures import ABC_PROGRAM, build_image, wrap_dim
from tools.willow import lzx
from tools.willow.__main__ import main
from tools.willow.config import Config, ConfigError, load_config
from tools.willow.lock import VersionMismatch, load_game, sha256

SYSTEM = {"cotsys": b"", "HUMAN.SYS": b"os", "cot.r": ABC_PROGRAM}
DATA = {"cotdat": b"", "xtext/cotxpt.dt": b"text", "user/cotsys.dat": b"scores"}


def lock_toml(unpacked_sha256: str = sha256(lzx.unpack(ABC_PROGRAM).image)) -> str:
    def files(table: dict[str, bytes], skip: str = "~") -> str:
        return "".join(f'"{k}" = "{sha256(v)}"\n' for k, v in table.items() if not k.startswith(skip))

    return f"""
[release]
name = "the test release"
[main]
disk = "system"
path = "cot.r"
unpacked_sha256 = "{unpacked_sha256}"
[disks.system]
ignore = []
[disks.system.files]
{files(SYSTEM)}
[disks.data]
ignore = ["user/"]
[disks.data.files]
{files(DATA, skip="user/")}
"""


class LockTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(self.enterContext(tempfile.TemporaryDirectory()))
        self.lock = self.tmp / "version.toml"
        self.lock.write_text(lock_toml(), encoding="utf-8")

    def disks(self, system=SYSTEM, data=DATA) -> list[Path]:
        paths = [self.tmp / "sys.dim", self.tmp / "data.xdf"]
        paths[0].write_bytes(wrap_dim(build_image(system)))
        paths[1].write_bytes(build_image(data))
        return paths

    def load(self, disks: list[Path]):
        return load_game(Config(tuple(disks), self.tmp / "build"), self.lock)

    def assertRefused(self, disks: list[Path], message: str):
        with self.assertRaises(VersionMismatch) as cm:
            self.load(disks)
        self.assertIn(message, str(cm.exception))
        self.assertIn("Provide your own dump of that release", str(cm.exception))

    def test_accepts_the_release_in_either_order(self):
        system, data = self.disks()
        for order in ([system, data], [data, system]):
            game = self.load(order)
            self.assertEqual(game.paths, {"system": system, "data": data})
            self.assertEqual(game.main.image, b"abcabcabc")

    def test_ignores_save_data(self):
        self.load(self.disks(data={**DATA, "user/cotsys.dat": b"new high score"}))

    def test_refuses_a_changed_asset(self):
        self.assertRefused(self.disks(data={**DATA, "xtext/cotxpt.dt": b"hacked"}), "xtext/cotxpt.dt differs")

    def test_refuses_a_missing_file(self):
        data = {k: v for k, v in DATA.items() if k != "xtext/cotxpt.dt"}
        self.assertRefused(self.disks(data=data), "xtext/cotxpt.dt is missing")

    def test_refuses_an_extra_file(self):
        self.assertRefused(self.disks(system={**SYSTEM, "trainer.r": b"!"}), "trainer.r is not part of the release")

    def test_refuses_a_different_main_executable(self):
        self.lock.write_text(lock_toml(unpacked_sha256="0" * 64), encoding="utf-8")
        self.assertRefused(self.disks(), "cot.r unpacks to a different executable")

    def test_refuses_a_missing_disk(self):
        self.assertRefused(self.disks()[:1], "no data disk")

    def test_refuses_a_foreign_disk(self):
        system, _ = self.disks()
        other = self.tmp / "other.xdf"
        other.write_bytes(build_image({"game.x": b"something else"}))
        self.assertRefused([system, other], "other.xdf is not a Cotton system or data disk")


class ConfigTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(self.enterContext(tempfile.TemporaryDirectory()))

    def test_missing_config_points_at_the_example(self):
        with self.assertRaisesRegex(ConfigError, "config.example.toml"):
            load_config(self.tmp / "config.local.toml")

    def test_relative_paths_resolve_against_the_config_file(self):
        (self.tmp / "dumps").mkdir()
        (self.tmp / "dumps" / "a.dim").write_bytes(b"")
        cfg = self.tmp / "config.local.toml"
        cfg.write_text('[game]\ndisks = ["dumps/a.dim"]\n', encoding="utf-8")
        config = load_config(cfg)
        self.assertEqual(config.disks, ((self.tmp / "dumps" / "a.dim").resolve(),))
        self.assertEqual(config.out_dir, (self.tmp / "build").resolve())

    def test_missing_disk_image(self):
        cfg = self.tmp / "config.local.toml"
        cfg.write_text('[game]\ndisks = ["nope.dim"]\n', encoding="utf-8")
        with self.assertRaisesRegex(ConfigError, "disk image not found"):
            load_config(cfg)

    def test_cli_refuses_disks_that_are_not_the_release(self):
        disk = self.tmp / "sys.xdf"
        disk.write_bytes(build_image(SYSTEM))
        cfg = self.tmp / "config.local.toml"
        cfg.write_text(f'[game]\ndisks = ["{disk.as_posix()}"]\n', encoding="utf-8")
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            self.assertEqual(main(["--config", str(cfg), "verify"]), 2)
        self.assertIn("not the release Willow supports", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
