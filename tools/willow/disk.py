"""X68000 2HD floppy images: container decoding (DIM, XDF) and the FAT12 filesystem.

Every image is normalized to a flat sector image before anything else looks at
it, so hashes and file reads do not depend on which container a dump came in.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Iterator

SECTOR_SIZE = 1024
SECTORS_PER_TRACK = 8
TRACKS = 154  # 77 cylinders x 2 heads
TRACK_SIZE = SECTOR_SIZE * SECTORS_PER_TRACK
IMAGE_SIZE = TRACK_SIZE * TRACKS  # 1,261,568 bytes

DIM_HEADER_SIZE = 0x100
DIM_SIGNATURE_OFFSET = 0xAB
DIM_SIGNATURE = b"DIFC HEADER  "
DIM_TYPE_2HD = 0x00
DIM_MAX_TRACKS = 170

# Standard 2HD layout. Disk 1 carries an MS-DOS BPB that agrees with this;
# disk 2 boots a custom IPL and has no BPB at all, so the layout is fixed here.
RESERVED_SECTORS = 1
FAT_COUNT = 2
FAT_SECTORS = 2
ROOT_ENTRIES = 192
DIR_ENTRY_SIZE = 32
ROOT_START = RESERVED_SECTORS + FAT_COUNT * FAT_SECTORS
ROOT_SECTORS = ROOT_ENTRIES * DIR_ENTRY_SIZE // SECTOR_SIZE
DATA_START = ROOT_START + ROOT_SECTORS
CLUSTER_COUNT = IMAGE_SIZE // SECTOR_SIZE - DATA_START + 2

ATTR_VOLUME = 0x08
ATTR_DIRECTORY = 0x10


class DiskError(Exception):
    pass


def load_image(path: Path) -> bytes:
    """Read a DIM or XDF file and return the flat 1,261,568-byte sector image."""
    data = Path(path).read_bytes()
    if len(data) == IMAGE_SIZE:
        return data
    if data[DIM_SIGNATURE_OFFSET : DIM_SIGNATURE_OFFSET + len(DIM_SIGNATURE)] == DIM_SIGNATURE:
        return _decode_dim(data, path)
    raise DiskError(f"{path}: not a 2HD DIM or XDF image ({len(data)} bytes)")


def _decode_dim(data: bytes, path: Path) -> bytes:
    if data[0] != DIM_TYPE_2HD:
        raise DiskError(f"{path}: DIM media type {data[0]:#04x} is not 2HD")
    flags = data[1 : 1 + DIM_MAX_TRACKS]
    if any(flags[TRACKS:]):
        raise DiskError(f"{path}: DIM marks tracks beyond {TRACKS}")
    # Tracks whose flag is clear are not stored; they read back as unformatted.
    present = sum(1 for f in flags[:TRACKS] if f)
    expected = DIM_HEADER_SIZE + present * TRACK_SIZE
    if len(data) != expected:
        raise DiskError(f"{path}: DIM is {len(data)} bytes, header implies {expected}")
    out = bytearray(b"\xe5" * IMAGE_SIZE)
    offset = DIM_HEADER_SIZE
    for track in range(TRACKS):
        if flags[track]:
            out[track * TRACK_SIZE : (track + 1) * TRACK_SIZE] = data[offset : offset + TRACK_SIZE]
            offset += TRACK_SIZE
    return bytes(out)


@dataclass(frozen=True)
class DirEntry:
    name: str
    attr: int
    cluster: int
    size: int
    date: int
    time: int

    @property
    def is_dir(self) -> bool:
        return bool(self.attr & ATTR_DIRECTORY)

    @property
    def timestamp(self) -> str:
        y, mo, d = 1980 + (self.date >> 9), (self.date >> 5) & 0xF, self.date & 0x1F
        h, mi, s = self.time >> 11, (self.time >> 5) & 0x3F, (self.time & 0x1F) * 2
        return f"{y:04}-{mo:02}-{d:02} {h:02}:{mi:02}:{s:02}"


class Volume:
    """Read-only view of a Human68k FAT12 floppy."""

    def __init__(self, image: bytes):
        if len(image) != IMAGE_SIZE:
            raise DiskError(f"sector image is {len(image)} bytes, expected {IMAGE_SIZE}")
        self.image = image
        fat_off = RESERVED_SECTORS * SECTOR_SIZE
        self._fat = image[fat_off : fat_off + FAT_SECTORS * SECTOR_SIZE]

    def sector(self, n: int, count: int = 1) -> bytes:
        return self.image[n * SECTOR_SIZE : (n + count) * SECTOR_SIZE]

    def _next_cluster(self, n: int) -> int:
        off = n * 3 // 2
        pair = self._fat[off] | self._fat[off + 1] << 8
        return pair >> 4 if n & 1 else pair & 0xFFF

    def chain(self, first: int) -> list[int]:
        clusters, n = [], first
        while 2 <= n < CLUSTER_COUNT:
            if len(clusters) >= CLUSTER_COUNT:
                raise DiskError(f"FAT chain from cluster {first} loops")
            clusters.append(n)
            n = self._next_cluster(n)
        if n < 0xFF8:
            raise DiskError(f"FAT chain from cluster {first} ends on bad entry {n:#05x}")
        return clusters

    def _read_chain(self, first: int) -> bytes:
        return b"".join(self.sector(DATA_START + c - 2) for c in self.chain(first))

    def listdir(self, directory: DirEntry | None = None) -> list[DirEntry]:
        raw = self.sector(ROOT_START, ROOT_SECTORS) if directory is None else self._read_chain(directory.cluster)
        entries = []
        for off in range(0, len(raw), DIR_ENTRY_SIZE):
            e = raw[off : off + DIR_ENTRY_SIZE]
            if e[0] == 0x00:
                break
            if e[0] == 0xE5 or e[11] & ATTR_VOLUME or e[0] == ord("."):
                continue
            entries.append(_parse_entry(e))
        return entries

    def walk(self, directory: DirEntry | None = None, prefix: str = "") -> Iterator[tuple[str, DirEntry]]:
        for entry in self.listdir(directory):
            path = f"{prefix}/{entry.name}"
            yield path, entry
            if entry.is_dir:
                yield from self.walk(entry, path)

    def find(self, path: str) -> DirEntry:
        """Look up a slash-separated path. Human68k names are case-insensitive."""
        directory = None
        for part in [p for p in path.split("/") if p]:
            for entry in self.listdir(directory):
                if entry.name.lower() == part.lower():
                    directory = entry
                    break
            else:
                raise DiskError(f"no such file on disk: {path}")
        if directory is None:
            raise DiskError("the root directory is not a file")
        return directory

    def read(self, entry: DirEntry | str) -> bytes:
        if isinstance(entry, str):
            entry = self.find(entry)
        if entry.is_dir:
            raise DiskError(f"{entry.name} is a directory")
        if entry.size == 0:
            return b""
        data = self._read_chain(entry.cluster)
        if len(data) < entry.size:
            raise DiskError(f"{entry.name}: chain holds {len(data)} bytes, entry says {entry.size}")
        return data[: entry.size]


def _parse_entry(e: bytes) -> DirEntry:
    raw_name = bytes([0xE5]) + e[1:8] if e[0] == 0x05 else e[0:8]
    # Human68k keeps 10 more name bytes (NUL padded) in the field MS-DOS reserves.
    stem = (raw_name.rstrip(b" ") + e[12:22].rstrip(b"\0")).decode("shift_jis", "replace")
    ext = e[8:11].rstrip(b" ").decode("shift_jis", "replace")
    return DirEntry(
        name=f"{stem}.{ext}" if ext else stem,
        attr=e[11],
        time=int.from_bytes(e[22:24], "little"),
        date=int.from_bytes(e[24:26], "little"),
        cluster=int.from_bytes(e[26:28], "little"),
        size=int.from_bytes(e[28:32], "little"),
    )
