"""The 68000 programs Willow recompiles, as listed in programs.toml."""

from __future__ import annotations

import tomllib
from dataclasses import dataclass
from pathlib import Path

from . import lzx
from .config import REPO_ROOT
from .lock import Game

PROGRAMS_PATH = REPO_ROOT / "programs.toml"


@dataclass(frozen=True)
class Program:
    name: str
    image: bytes  # text + data as loaded at address 0; this is what goes into memory
    code: bytes  # the same with self-decrypting stages applied; this is what gets decoded
    text_size: int  # the header's code section size; a hint, since code can spill into data
    bss_size: int
    entry: int
    relocations: frozenset[int]  # offsets of longwords that get the load address added
    roots: tuple[int, ...]  # entry points reached only indirectly, through vectors or hooks
    file: str = ""  # its name on disk
    entry_ranges: tuple[tuple[int, int], ...] = ()  # [start, end): every instruction is an entry point


def load_programs(game: Game, path: Path = PROGRAMS_PATH) -> dict[str, Program]:
    programs = {}
    for name, spec in tomllib.loads(path.read_text(encoding="utf-8")).items():
        data = game.volumes[spec["disk"]].read(spec["path"])
        roots = tuple(spec.get("roots", ()))
        if lzx.is_lzx(data):
            u = lzx.unpack(data)
            image, text, bss, entry, relocs = u.image, u.text_size, u.bss_size, u.entry, frozenset(u.relocations)
        else:
            # A plain .R image: no header, so no section split or relocations to go on.
            image, text, bss, entry, relocs = data, len(data), 0, 0, frozenset()
        code = decrypt(image, spec.get("decrypt", ()))
        ranges = tuple((s, e) for s, e in spec.get("entry_ranges", ()))
        programs[name] = Program(name, image, code, text, bss, entry, relocs, roots, spec["path"], ranges)
    return programs


def decrypt(image: bytes, stages) -> bytes:
    """Apply the startup's in-place decryption stages statically."""
    out = bytearray(image)
    key = 0
    for stage in stages:
        start, end = stage["key_sum"]
        key ^= sum(out[start:end]) & 0xFF
        mask = (key << 4 | key >> 4) & 0xFF
        start, end = stage["range"]
        for i in range(start, end):
            out[i] ^= mask
    return bytes(out)
