"""The version lock.

Willow supports exactly one release. Every tool gets the game through
load_game(), which refuses disks that are not that release. version.toml holds
the expected hashes and nothing else from the game.
"""

from __future__ import annotations

import hashlib
import tomllib
from dataclasses import dataclass
from pathlib import Path

from . import lzx
from .config import REPO_ROOT, Config
from .disk import DiskError, Volume, load_image

LOCK_PATH = REPO_ROOT / "version.toml"

# Each disk carries an empty marker file that bootcot.r looks for.
MARKERS = {"system": "cotsys", "data": "cotdat"}
MAX_LISTED_PROBLEMS = 20


class VersionMismatch(Exception):
    pass


@dataclass(frozen=True)
class Game:
    volumes: dict[str, Volume]  # keyed by role: "system", "data"
    paths: dict[str, Path]
    main: lzx.Unpacked


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def load_game(config: Config, lock_path: Path = LOCK_PATH) -> Game:
    lock = tomllib.loads(lock_path.read_text(encoding="utf-8"))
    release = lock["release"]["name"]

    def refuse(problems: list[str]) -> VersionMismatch:
        listed = [f"  - {p}" for p in problems[:MAX_LISTED_PROBLEMS]]
        if len(problems) > MAX_LISTED_PROBLEMS:
            listed.append(f"  ... and {len(problems) - MAX_LISTED_PROBLEMS} more")
        return VersionMismatch(
            "these disks are not the release Willow supports.\n\n"
            + "\n".join(listed)
            + f"\n\nWillow targets exactly one release: {release}.\nProvide your own dump of that release."
        )

    volumes: dict[str, Volume] = {}
    paths: dict[str, Path] = {}
    for path in config.disks:
        try:
            volume = Volume(load_image(path))
            names = {e.name.lower() for e in volume.listdir()}
        except DiskError as e:
            raise refuse([str(e)]) from None
        roles = [role for role, marker in MARKERS.items() if marker in names]
        if len(roles) != 1:
            raise refuse([f"{path.name} is not a Cotton system or data disk"])
        role = roles[0]
        if role in volumes:
            raise refuse([f"{paths[role].name} and {path.name} are both the {role} disk"])
        volumes[role], paths[role] = volume, path
    missing = [role for role in MARKERS if role not in volumes]
    if missing:
        raise refuse([f"no {role} disk listed in the config" for role in missing])

    problems = []
    for role, volume in volumes.items():
        problems += _compare(role, volume, lock["disks"][role])

    main = lock["main"]
    unpacked = None
    try:
        unpacked = lzx.unpack(volumes[main["disk"]].read(main["path"]))
    except (DiskError, lzx.LzxError) as e:
        problems.append(f"{main['path']} cannot be unpacked: {e}")
    else:
        if sha256(unpacked.image) != main["unpacked_sha256"]:
            problems.append(f"{main['path']} unpacks to a different executable")

    if problems:
        raise refuse(problems)
    return Game({r: volumes[r] for r in MARKERS}, {r: paths[r] for r in MARKERS}, unpacked)


def _compare(role: str, volume: Volume, expected: dict) -> list[str]:
    ignore = tuple(expected.get("ignore", []))
    want: dict[str, str] = expected["files"]
    have: dict[str, str] = {}
    unreadable: set[str] = set()
    problems = []
    for path, entry in volume.walk():
        rel = path.lstrip("/")
        if entry.is_dir or rel.startswith(ignore):
            continue
        try:
            have[rel] = sha256(volume.read(entry))
        except DiskError as e:
            unreadable.add(rel)
            problems.append(f"{role} disk: {rel} cannot be read: {e}")
    for rel in sorted(want.keys() | have.keys()):
        if rel in unreadable:
            continue
        if rel not in have:
            problems.append(f"{role} disk: {rel} is missing")
        elif rel not in want:
            problems.append(f"{role} disk: {rel} is not part of the release")
        elif have[rel] != want[rel]:
            problems.append(f"{role} disk: {rel} differs")
    return problems
