"""Synthetic disks and LZX programs, so tests never need the real game."""

from __future__ import annotations

from tools.willow import disk, lzx

DATE_1993_08_27 = (13 << 9) | (8 << 5) | 27


def build_image(files: dict[str, bytes]) -> bytes:
    """A flat 2HD sector image holding `files`, keyed "name.ext" or "dir/name.ext"."""
    image = bytearray(disk.IMAGE_SIZE)
    fat = {0: 0xFFE, 1: 0xFFF}
    next_free = 2

    def alloc(data: bytes) -> int:
        nonlocal next_free
        first, count = next_free, max(1, -(-len(data) // disk.SECTOR_SIZE))
        for i in range(count):
            cluster = first + i
            fat[cluster] = cluster + 1 if i < count - 1 else 0xFFF
            off = (disk.DATA_START + cluster - 2) * disk.SECTOR_SIZE
            chunk = data[i * disk.SECTOR_SIZE : (i + 1) * disk.SECTOR_SIZE]
            image[off : off + len(chunk)] = chunk
        next_free += count
        return first

    def entry(name: str, attr: int, cluster: int, size: int) -> bytes:
        stem, _, ext = name.partition(".")
        return (
            stem[:8].encode().ljust(8)
            + ext.encode().ljust(3)
            + bytes([attr])
            + stem[8:18].encode().ljust(10, b"\0")
            + (0).to_bytes(2, "little")
            + DATE_1993_08_27.to_bytes(2, "little")
            + cluster.to_bytes(2, "little")
            + size.to_bytes(4, "little")
        )

    root, subdirs = [], {}
    for path, data in files.items():
        parent, _, name = path.rpartition("/")
        if parent:
            subdirs.setdefault(parent, []).append((name, data))
        else:
            root.append(entry(name, 0x20, alloc(data), len(data)))
    for name, children in subdirs.items():
        listing = b"".join(entry(child, 0x20, alloc(data), len(data)) for child, data in children)
        root.append(entry(name, disk.ATTR_DIRECTORY, alloc(listing), 0))

    root_off = disk.ROOT_START * disk.SECTOR_SIZE
    image[root_off : root_off + len(b"".join(root))] = b"".join(root)
    table = bytearray(disk.FAT_SECTORS * disk.SECTOR_SIZE)
    for n, value in fat.items():
        off = n * 3 // 2
        if n & 1:
            table[off] = (table[off] & 0x0F) | (value << 4 & 0xF0)
            table[off + 1] = value >> 4
        else:
            table[off] = value & 0xFF
            table[off + 1] = (table[off + 1] & 0xF0) | (value >> 8)
    for copy in range(disk.FAT_COUNT):
        off = (disk.RESERVED_SECTORS + copy * disk.FAT_SECTORS) * disk.SECTOR_SIZE
        image[off : off + len(table)] = table
    return bytes(image)


def wrap_dim(image: bytes, omit_tracks: frozenset[int] = frozenset()) -> bytes:
    header = bytearray(disk.DIM_HEADER_SIZE)
    header[0] = disk.DIM_TYPE_2HD
    for track in range(disk.TRACKS):
        header[1 + track] = track not in omit_tracks
    header[disk.DIM_SIGNATURE_OFFSET : disk.DIM_SIGNATURE_OFFSET + len(disk.DIM_SIGNATURE)] = disk.DIM_SIGNATURE
    tracks = (
        image[t * disk.TRACK_SIZE : (t + 1) * disk.TRACK_SIZE] for t in range(disk.TRACKS) if t not in omit_tracks
    )
    return bytes(header) + b"".join(tracks)


def build_lzx(stream: bytes, *, image_size: int, text_size: int, bss_size: int = 0, reloc_size: int = 0) -> bytes:
    """Wrap a hand-assembled packed stream in an LZX 0.42 header and a blank unpack-routine tail."""
    header = bytearray(lzx.STREAM_OFFSET)
    header[0:2] = b"\x60\x26"
    header[lzx.MAGIC_OFFSET : lzx.MAGIC_OFFSET + len(lzx.MAGIC)] = lzx.MAGIC
    fields = (0, image_size + bss_size, text_size, bss_size, reloc_size, 0)
    for off, value in zip(range(0x0E, 0x26, 4), fields):
        header[off : off + 4] = value.to_bytes(4, "big")
    return bytes(header) + stream + bytes(lzx.STUB_TAIL_SIZE)


# Flags 1110_1010: literal a, b, c; long match -3 x6; end marker.
ABC_STREAM = bytes.fromhex("ea 616263 ffec 0000 00")
ABC_PROGRAM = build_lzx(ABC_STREAM, image_size=9, text_size=6)
