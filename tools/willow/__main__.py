"""Willow toolchain entry point: python -m tools.willow <command>"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from . import codegen, lzx
from .analysis import analyze
from .config import DEFAULT_CONFIG, Config, ConfigError, load_config
from .lock import Game, VersionMismatch, load_game, sha256
from .program import load_programs


def cmd_verify(game: Game, config: Config) -> None:
    print("OK: these disks are the supported release.")
    for role, volume in game.volumes.items():
        files = sum(1 for _, e in volume.walk() if not e.is_dir)
        print(f"  {role + ' disk':12} {files:3} files  {game.paths[role]}")
    m = game.main
    print(
        f"  main executable: text {m.text_size:#x}, data {m.data_size:#x}, bss {m.bss_size:#x}, "
        f"{len(m.relocations)} relocations"
    )
    print(f"  unpacked sha256: {sha256(m.image)}")


def cmd_ls(game: Game, config: Config) -> None:
    for role, volume in game.volumes.items():
        print(f"{role} disk:")
        for path, e in volume.walk():
            size = "<dir>" if e.is_dir else f"{e.size:,}"
            print(f"  {path:28} {size:>9}  {e.timestamp}")


def cmd_extract(game: Game, config: Config) -> None:
    """Write every file to <out_dir>/disks/<role>/, and LZX programs unpacked to <out_dir>/unpacked/."""
    for role, volume in game.volumes.items():
        for path, e in volume.walk():
            rel = path.lstrip("/")
            dest = config.out_dir / "disks" / role / rel
            if e.is_dir:
                dest.mkdir(parents=True, exist_ok=True)
                continue
            dest.parent.mkdir(parents=True, exist_ok=True)
            data = volume.read(e)
            dest.write_bytes(data)
            if lzx.is_lzx(data):
                _write_unpacked(config.out_dir / "unpacked" / role / rel, lzx.unpack(data))
    print(f"extracted to {config.out_dir}")


def _write_unpacked(dest: Path, u: lzx.Unpacked) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.with_suffix(".bin").write_bytes(u.image)
    info = {
        "entry": u.entry,
        "text_size": u.text_size,
        "data_size": u.data_size,
        "bss_size": u.bss_size,
        "relocations": list(u.relocations),
    }
    dest.with_suffix(".json").write_text(json.dumps(info, indent=1) + "\n", encoding="utf-8")


def cmd_analyze(game: Game, config: Config) -> None:
    """Find the code and functions of each program; write listings to <out_dir>/analysis/."""
    out = config.out_dir / "analysis"
    out.mkdir(parents=True, exist_ok=True)
    for name, program in load_programs(game).items():
        a = analyze(program)
        (out / f"{name}.lst").write_text("\n".join(a.listing()) + "\n", encoding="utf-8")
        (out / f"{name}.json").write_text(json.dumps(a.summary(), indent=1) + "\n", encoding="utf-8")
        in_text = sum(i.length for addr, i in a.insns.items() if addr < program.text_size)
        print(
            f"{name}: {len(a.insns)} instructions in {len(a.functions)} functions, "
            f"{100 * in_text / program.text_size:.1f}% of the code section; "
            f"{len(a.tables)} jump tables, {len(a.unresolved)} unresolved jumps, {len(a.errors)} errors"
        )
    print(f"listings written to {out}")


def cmd_recomp(game: Game, config: Config) -> None:
    """Translate each program to C in <out_dir>/recomp/<program>/."""
    out = config.out_dir / "recomp"
    programs = load_programs(game)
    for name, program in programs.items():
        gen = codegen.generate(analyze(program), program.file)
        files = codegen.write(gen, out / name)
        lines = sum(f.read_text(encoding="utf-8").count("\n") for f in files)
        print(f"{name}: {len(gen.functions)} functions, {len(files)} files, {lines:,} lines of C")
    codegen.write_index(list(programs), out)
    print(f"C written to {out}")


COMMANDS = {
    "verify": (cmd_verify, "check your disks against the supported release"),
    "ls": (cmd_ls, "list the files on both disks"),
    "extract": (cmd_extract, "extract all files and unpack the LZX executables"),
    "analyze": (cmd_analyze, "find the code and functions of each program"),
    "recomp": (cmd_recomp, "translate each program to C"),
}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="python -m tools.willow", description=__doc__)
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG, help="default: config.local.toml")
    sub = parser.add_subparsers(dest="command", required=True)
    for name, (_, help_text) in COMMANDS.items():
        sub.add_parser(name, help=help_text)
    args = parser.parse_args(argv)

    try:
        config = load_config(args.config)
        game = load_game(config)
    except (ConfigError, VersionMismatch) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    COMMANDS[args.command][0](game, config)
    return 0


if __name__ == "__main__":
    sys.exit(main())
