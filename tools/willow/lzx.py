"""LZX 0.42 self-extracting executables (LZX.X).

cot.r, ealogo.r and cotins.r are .R programs packed with LZX.X. Each carries a
68000 stub that unpacks the program in place and jumps to it. The format here
was recovered from that stub (it is byte-identical in all three files):

    0x00  bra.s   stub
    0x04  "LZX 0.42"         (the stub overwrites this at run time)
    0x0e  long    entry point, offset from the load address
    0x12  long    memory needed: text + data + bss
    0x16  long    text size
    0x1a  long    bss size
    0x1e  long    relocation stream size (0 = no relocations)
    0x22  long    gap the stub moves the packed data up by before unpacking
    0xa4  ...     packed stream; the unpack routine sits in the file's tail

The stream is LZSS with flag bits taken MSB first from interleaved flag bytes:

    1             literal byte
    0 0 b b  o    short match: offset o-256, length bb+2
    0 1  hi lo    long match: offset ((hi:lo | 0xffff0000) >> 3), signed;
                  length (lo & 7) + 2, or if that field is 0 the next byte + 1;
                  a zero length byte ends the stream

Relocations follow the end marker as deltas between longword offsets. Each
delta is a little run of bytes whose low bit means "more follows", with the
continuation bias the stub's addx gives it.
"""

from __future__ import annotations

from dataclasses import dataclass

MAGIC = b"LZX 0.42"
MAGIC_OFFSET = 0x04
STREAM_OFFSET = 0xA4
STUB_TAIL_SIZE = 0xBA  # unpack routine + out-of-memory message at the file's end


class LzxError(Exception):
    pass


@dataclass(frozen=True)
class Unpacked:
    image: bytes  # text + data as linked for load address 0, relocations not applied
    entry: int
    text_size: int
    bss_size: int
    relocations: tuple[int, ...]  # offsets of longwords that get the load address added

    @property
    def data_size(self) -> int:
        return len(self.image) - self.text_size


def is_lzx(data: bytes) -> bool:
    return data[:2] == b"\x60\x26" and data[MAGIC_OFFSET : MAGIC_OFFSET + len(MAGIC)] == MAGIC


def _long(data: bytes, off: int) -> int:
    return int.from_bytes(data[off : off + 4], "big")


def unpack(data: bytes) -> Unpacked:
    if not is_lzx(data):
        raise LzxError("not an LZX 0.42 executable")
    entry, memory, text_size, bss_size, reloc_size = (_long(data, o) for o in range(0x0E, 0x22, 4))
    stream_end = len(data) - STUB_TAIL_SIZE

    pos = STREAM_OFFSET
    flags = bits = 0

    def byte() -> int:
        nonlocal pos
        if pos >= stream_end:
            raise LzxError("packed stream runs into the unpack routine")
        pos += 1
        return data[pos - 1]

    def bit() -> int:
        nonlocal flags, bits
        if bits == 0:
            flags, bits = byte(), 8
        bits -= 1
        flags <<= 1
        return flags >> 8 & 1

    out = bytearray()
    while True:
        if bit():
            out.append(byte())
            continue
        if not bit():
            length = (bit() << 1 | bit()) + 2
            offset = byte() - 0x100
        else:
            word = byte() << 8 | byte()
            offset = (word - 0x10000) >> 3
            length = word & 7
            if length:
                length += 2
            else:
                length = byte()
                if length == 0:
                    break
                length += 1
        src = len(out) + offset
        if src < 0:
            raise LzxError(f"match reaches {-src} bytes before the start of output")
        for i in range(length):
            out.append(out[src + i])

    relocations = []
    if reloc_size:
        offset = 0
        b = byte()
        while True:
            delta = 0
            while b & 1:
                delta = (delta + (b >> 1) + 1) << 7
                b = byte()
            offset += delta + (b >> 1)
            if offset + 4 > len(out):
                raise LzxError(f"relocation at {offset:#x} is outside the {len(out):#x}-byte image")
            relocations.append(offset)
            b = byte()
            if b == 0:
                break

    if len(out) + bss_size != memory:
        raise LzxError(f"unpacked {len(out):#x} bytes + bss {bss_size:#x} != header size {memory:#x}")
    if not text_size <= len(out) or entry >= len(out):
        raise LzxError("header text size or entry point lies outside the unpacked image")
    return Unpacked(bytes(out), entry, text_size, bss_size, tuple(relocations))
