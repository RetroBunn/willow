"""Local configuration. Paths to user-supplied files come only from config.local.toml."""

from __future__ import annotations

import tomllib
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_CONFIG = REPO_ROOT / "config.local.toml"


class ConfigError(Exception):
    pass


@dataclass(frozen=True)
class Config:
    disks: tuple[Path, ...]
    out_dir: Path


def load_config(path: Path = DEFAULT_CONFIG) -> Config:
    path = Path(path)
    if not path.is_file():
        raise ConfigError(
            f"{path} not found. Copy config.example.toml to {path.name} and point it at your own dump."
        )
    try:
        raw = tomllib.loads(path.read_text(encoding="utf-8"))
    except tomllib.TOMLDecodeError as e:
        raise ConfigError(f"{path}: {e}") from None

    # Relative paths are resolved against the config file, not the working directory.
    base = path.parent
    disks = raw.get("game", {}).get("disks")
    if not disks or not isinstance(disks, list) or not all(isinstance(d, str) for d in disks):
        raise ConfigError(f"{path}: [game] disks must list your disk image files")
    resolved = tuple((base / Path(d).expanduser()).resolve() for d in disks)
    for disk in resolved:
        if not disk.is_file():
            raise ConfigError(f"{path}: disk image not found: {disk}")

    out_dir = raw.get("build", {}).get("out_dir", "build")
    return Config(resolved, (base / Path(out_dir).expanduser()).resolve())
