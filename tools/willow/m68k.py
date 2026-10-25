"""MC68000 instruction decoder.

Decodes one instruction into a structured form that the analyzer, and later the
recompiler, work from. It covers the plain 68000 instruction set. 68010-and-later
encodings are rejected, which also helps tell code from data. Human68k uses the
line-F space for DOS calls ($FFxx) and its floating point package ($FExx), so
those decode as instructions too.
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum

MASK32 = 0xFFFFFFFF
CONDITIONS = ("t", "f", "hi", "ls", "cc", "cs", "ne", "eq", "vc", "vs", "pl", "mi", "ge", "lt", "gt", "le")
SIZES = ("b", "w", "l")
# Human68k DOS calls that end the process: _EXIT, _KEEPPR, _EXIT2.
DOS_NO_RETURN = frozenset({0x00, 0x31, 0x4C})


class DecodeError(Exception):
    def __init__(self, addr: int, reason: str):
        super().__init__(f"{addr:#08x}: {reason}")
        self.addr = addr
        self.reason = reason


class Mode(Enum):
    DREG = "Dn"
    AREG = "An"
    IND = "(An)"
    POSTINC = "(An)+"
    PREDEC = "-(An)"
    DISP = "d16(An)"
    INDEX = "d8(An,Xn)"
    ABSW = "abs.w"
    ABSL = "abs.l"
    PCDISP = "d16(PC)"
    PCINDEX = "d8(PC,Xn)"
    IMM = "#imm"
    REGLIST = "reglist"
    SR = "sr"
    CCR = "ccr"
    USP = "usp"


class Flow(Enum):
    NEXT = "next"  # continues with the next instruction
    BRANCH = "branch"  # conditional: the target or the next instruction
    JUMP = "jump"  # unconditional transfer, no fall-through
    CALL = "call"  # subroutine call that returns to the next instruction
    RETURN = "return"  # rts, rte, rtr
    STOP = "stop"  # never continues: illegal, or a DOS call that ends the process


@dataclass(frozen=True)
class Operand:
    mode: Mode
    reg: int = 0  # register number; for REGLIST a mask, bit 0 = d0 ... bit 15 = a7
    value: int = 0  # displacement, absolute address or immediate; for PC modes the base address
    index: int = 0  # INDEX/PCINDEX index register: 0-7 = d0-d7, 8-15 = a0-a7
    index_long: bool = False
    ext: int | None = None  # address of this operand's extension words

    def __str__(self) -> str:
        m, r, v = self.mode, self.reg, self.value
        xn = f"{'da'[self.index >> 3]}{self.index & 7}.{'l' if self.index_long else 'w'}"
        if m is Mode.DREG:
            return f"d{r}"
        if m is Mode.AREG:
            return f"a{r}"
        if m is Mode.IND:
            return f"(a{r})"
        if m is Mode.POSTINC:
            return f"(a{r})+"
        if m is Mode.PREDEC:
            return f"-(a{r})"
        if m is Mode.DISP:
            return f"{_hex(v)}(a{r})"
        if m is Mode.INDEX:
            return f"{_hex(v)}(a{r},{xn})"
        if m is Mode.ABSW:
            return f"${v & 0xFFFF:x}.w"
        if m is Mode.ABSL:
            return f"${v:x}.l"
        if m is Mode.PCDISP:
            return f"${v:x}(pc)"
        if m is Mode.PCINDEX:
            return f"${v:x}(pc,{xn})"
        if m is Mode.IMM:
            return f"#{_hex(v)}"
        if m is Mode.REGLIST:
            return _reglist(r)
        return m.value


@dataclass(frozen=True)
class Insn:
    addr: int
    length: int
    op: str  # bcc, dbcc and scc carry their condition in `cond`
    size: str | None
    operands: tuple[Operand, ...] = ()
    cond: str | None = None
    flow: Flow = Flow.NEXT
    target: int | None = None  # static target of a branch, call or jump

    @property
    def mnemonic(self) -> str:
        if self.cond is not None:
            name = {"bcc": "b", "dbcc": "db", "scc": "s"}[self.op] + self.cond
            name = "dbra" if name == "dbf" else name
        else:
            name = self.op
        return f"{name}.{self.size}" if self.size and self.op != "moveq" else name

    def __str__(self) -> str:
        args = [str(o) for o in self.operands]
        if self.op in ("bra", "bsr", "bcc", "dbcc"):
            args.append(f"${self.target:x}")
        return f"{self.mnemonic:<9}{','.join(args)}".rstrip()


def decode(mem: bytes, addr: int, base: int = 0) -> Insn:
    """Decode the instruction at `addr`; `mem` holds the program as loaded at `base`."""
    if addr & 1:
        raise DecodeError(addr, "odd address")
    r = _Reader(mem, addr, base)
    w = r.word()
    return _LINES[w >> 12](r, w)


# Effective-address kinds: modes 0-6, then the mode-7 forms abs.w, abs.l, d16(PC), d8(PC,Xn), #imm.
_ALL = frozenset(range(12))
_DATA = _ALL - {1}
_MEM = _DATA - {0}
_CTRL = frozenset({2, 5, 6, 7, 8, 9, 10})
_ALT = _ALL - {9, 10, 11}
_DALT = _DATA & _ALT
_MALT = _MEM & _ALT
_CALT = _CTRL & _ALT


def _hex(v: int) -> str:
    return f"-${-v:x}" if v < 0 else f"${v:x}"


def _sext8(v: int) -> int:
    return v - 0x100 if v & 0x80 else v


def _sext16(v: int) -> int:
    return v - 0x10000 if v & 0x8000 else v


def _reverse16(v: int) -> int:
    return int(f"{v:016b}"[::-1], 2)


def _reglist(mask: int) -> str:
    parts = []
    for bank, name in ((mask & 0xFF, "d"), (mask >> 8, "a")):
        n = 0
        while n < 8:
            if bank >> n & 1:
                end = n
                while end < 7 and bank >> (end + 1) & 1:
                    end += 1
                parts.append(f"{name}{n}" if end == n else f"{name}{n}-{name}{end}")
                n = end
            n += 1
    return "/".join(parts)


def _d(n: int) -> Operand:
    return Operand(Mode.DREG, n)


def _a(n: int) -> Operand:
    return Operand(Mode.AREG, n)


def _q(v: int) -> Operand:
    return Operand(Mode.IMM, value=v)


class _Reader:
    def __init__(self, mem: bytes, addr: int, base: int):
        self.mem, self.base, self.start, self.pos = mem, base, addr, addr

    def invalid(self, reason: str) -> DecodeError:
        return DecodeError(self.start, reason)

    def word(self) -> int:
        off = self.pos - self.base
        if off < 0 or off + 2 > len(self.mem):
            raise self.invalid("runs past the end of the image")
        self.pos += 2
        return self.mem[off] << 8 | self.mem[off + 1]

    def long(self) -> int:
        return self.word() << 16 | self.word()

    def imm(self, size: str) -> Operand:
        ext = self.pos
        if size == "l":
            return Operand(Mode.IMM, value=self.long(), ext=ext)
        value = self.word()
        if size == "b":
            # A byte immediate sits in the low byte of a word. Assemblers leave the high
            # byte zero, or sign-extend into it; anything else is not real code.
            if value >> 8 not in (0x00, 0xFF) or value >> 8 == 0xFF and value < 0xFF80:
                raise self.invalid("byte immediate with a stray high byte")
            value &= 0xFF
        return Operand(Mode.IMM, value=value, ext=ext)

    def brief(self) -> tuple[int, int, bool]:
        w = self.word()
        if w & 0x0700:
            raise self.invalid("68020 index extension word")
        return _sext8(w & 0xFF), w >> 12, bool(w & 0x0800)

    def ea(self, mode: int, reg: int, size: str | None, allowed: frozenset[int]) -> Operand:
        kind = mode if mode < 7 else 7 + reg
        if kind not in allowed or (kind == 1 and size == "b"):
            raise self.invalid("addressing mode not allowed here")
        if kind <= 4:
            return Operand((Mode.DREG, Mode.AREG, Mode.IND, Mode.POSTINC, Mode.PREDEC)[kind], reg)
        ext = self.pos
        if kind == 5:
            return Operand(Mode.DISP, reg, _sext16(self.word()), ext=ext)
        if kind == 6:
            d8, index, long_ = self.brief()
            return Operand(Mode.INDEX, reg, d8, index, long_, ext)
        if kind == 7:
            return Operand(Mode.ABSW, value=_sext16(self.word()) & MASK32, ext=ext)
        if kind == 8:
            return Operand(Mode.ABSL, value=self.long(), ext=ext)
        if kind == 9:
            return Operand(Mode.PCDISP, value=(ext + _sext16(self.word())) & MASK32, ext=ext)
        if kind == 10:
            d8, index, long_ = self.brief()
            return Operand(Mode.PCINDEX, value=(ext + d8) & MASK32, index=index, index_long=long_, ext=ext)
        return self.imm(size or "w")

    def make(self, op: str, size: str | None, *operands: Operand, cond: str | None = None,
             flow: Flow = Flow.NEXT, target: int | None = None) -> Insn:
        if target is not None and target & 1:
            raise self.invalid("odd branch target")
        return Insn(self.start, self.pos - self.start, op, size, operands, cond, flow, target)


def _fields(w: int) -> tuple[int, int, int, int]:
    """Register in bits 11-9, op mode in bits 8-6, EA mode and register."""
    return (w >> 9) & 7, (w >> 6) & 7, (w >> 3) & 7, w & 7


_BITOPS = ("btst", "bchg", "bclr", "bset")
_IMMOPS = {0: "ori", 1: "andi", 2: "subi", 3: "addi", 5: "eori", 6: "cmpi"}


def _line0(r: _Reader, w: int) -> Insn:
    dn, _, mode, reg = _fields(w)
    if w & 0x100:
        if mode == 1:
            opm = (w >> 6) & 3
            ext = r.pos
            mem = Operand(Mode.DISP, reg, _sext16(r.word()), ext=ext)
            ops = (mem, _d(dn)) if opm < 2 else (_d(dn), mem)
            return r.make("movep", "l" if opm & 1 else "w", *ops)
        kind = (w >> 6) & 3
        dst = r.ea(mode, reg, "b", _DATA if kind == 0 else _DALT)
        return r.make(_BITOPS[kind], "l" if mode == 0 else "b", _d(dn), dst)
    if dn == 4:
        kind = (w >> 6) & 3
        bit = r.imm("b")
        dst = r.ea(mode, reg, "b", _DATA - {11} if kind == 0 else _DALT)
        return r.make(_BITOPS[kind], "l" if mode == 0 else "b", bit, dst)
    if dn not in _IMMOPS:
        raise r.invalid("68010+ or undefined line-0 instruction")
    name = _IMMOPS[dn]
    if dn in (0, 1, 5) and w & 0xFF in (0x3C, 0x7C):
        to_sr = w & 0xFF == 0x7C
        src = r.imm("w" if to_sr else "b")
        return r.make(name, "w" if to_sr else "b", src, Operand(Mode.SR if to_sr else Mode.CCR))
    sz = (w >> 6) & 3
    if sz == 3:
        raise r.invalid("undefined size")
    src = r.imm(SIZES[sz])
    return r.make(name, SIZES[sz], src, r.ea(mode, reg, SIZES[sz], _DALT))


def _move(r: _Reader, w: int) -> Insn:
    size = {1: "b", 3: "w", 2: "l"}[w >> 12]
    dreg, dmode, mode, reg = _fields(w)
    src = r.ea(mode, reg, size, _ALL)
    if dmode == 1:
        if size == "b":
            raise r.invalid("movea.b does not exist")
        return r.make("movea", size, src, _a(dreg))
    return r.make("move", size, src, r.ea(dmode, dreg, size, _DALT))


def _movem(r: _Reader, w: int, to_regs: bool) -> Insn:
    size = "l" if w & 0x40 else "w"
    mask = r.word()
    _, _, mode, reg = _fields(w)
    if to_regs:
        return r.make("movem", size, r.ea(mode, reg, size, _CTRL | {3}), Operand(Mode.REGLIST, mask))
    ea = r.ea(mode, reg, size, _CALT | {4})
    # In predecrement mode the mask runs backwards: bit 0 = a7 ... bit 15 = d0.
    return r.make("movem", size, Operand(Mode.REGLIST, _reverse16(mask) if mode == 4 else mask), ea)


def _line4(r: _Reader, w: int) -> Insn:
    an, opm, mode, reg = _fields(w)
    if opm == 7:
        return r.make("lea", None, r.ea(mode, reg, "l", _CTRL), _a(an))
    if opm == 6:
        return r.make("chk", "w", r.ea(mode, reg, "w", _DATA), _d(an))
    if w & 0x100:
        raise r.invalid("68020 chk.l or undefined line-4 instruction")
    hi, sz = (w >> 8) & 0xF, (w >> 6) & 3
    if hi in (0x0, 0x2, 0x4, 0x6):
        if sz == 3:
            if hi == 0x0:
                return r.make("move", "w", Operand(Mode.SR), r.ea(mode, reg, "w", _DALT))
            if hi == 0x2:
                raise r.invalid("move from ccr is 68010+")
            return r.make("move", "w", r.ea(mode, reg, "w", _DATA), Operand(Mode.CCR if hi == 0x4 else Mode.SR))
        return r.make(("negx", "clr", "neg", "not")[hi >> 1], SIZES[sz], r.ea(mode, reg, SIZES[sz], _DALT))
    if hi == 0x8:
        if sz == 0:
            return r.make("nbcd", "b", r.ea(mode, reg, "b", _DALT))
        if sz == 1:
            if mode == 0:
                return r.make("swap", None, _d(reg))
            return r.make("pea", None, r.ea(mode, reg, "l", _CTRL))
        if mode == 0:
            return r.make("ext", "w" if sz == 2 else "l", _d(reg))
        return _movem(r, w, to_regs=False)
    if hi == 0xA:
        if w == 0x4AFC:
            return r.make("illegal", None, flow=Flow.STOP)
        if sz == 3:
            return r.make("tas", "b", r.ea(mode, reg, "b", _DALT))
        return r.make("tst", SIZES[sz], r.ea(mode, reg, SIZES[sz], _DALT))
    if hi == 0xC:
        if sz < 2:
            raise r.invalid("68020 long multiply or divide")
        return _movem(r, w, to_regs=True)
    if hi == 0xE:
        if sz == 2 or sz == 3:
            dst = r.ea(mode, reg, None, _CTRL)
            static = dst.value if dst.mode in (Mode.ABSW, Mode.ABSL, Mode.PCDISP) else None
            if sz == 2:
                return r.make("jsr", None, dst, flow=Flow.CALL, target=static)
            return r.make("jmp", None, dst, flow=Flow.JUMP, target=static)
        if sz == 1:
            if 0x4E40 <= w <= 0x4E4F:
                return r.make("trap", None, _q(w & 0xF))
            if 0x4E50 <= w <= 0x4E57:
                return r.make("link", None, _a(reg), r.imm("w"))
            if 0x4E58 <= w <= 0x4E5F:
                return r.make("unlk", None, _a(reg))
            if 0x4E60 <= w <= 0x4E67:
                return r.make("move", "l", _a(reg), Operand(Mode.USP))
            if 0x4E68 <= w <= 0x4E6F:
                return r.make("move", "l", Operand(Mode.USP), _a(reg))
            if w == 0x4E72:
                return r.make("stop", None, r.imm("w"))
            simple = {0x4E70: ("reset", Flow.NEXT), 0x4E71: ("nop", Flow.NEXT), 0x4E73: ("rte", Flow.RETURN),
                      0x4E75: ("rts", Flow.RETURN), 0x4E76: ("trapv", Flow.NEXT), 0x4E77: ("rtr", Flow.RETURN)}
            if w in simple:
                name, flow = simple[w]
                return r.make(name, None, flow=flow)
    raise r.invalid("68010+ or undefined line-4 instruction")


def _line5(r: _Reader, w: int) -> Insn:
    data, _, mode, reg = _fields(w)
    sz = (w >> 6) & 3
    if sz == 3:
        cond = CONDITIONS[(w >> 8) & 0xF]
        if mode == 1:
            target = (r.pos + _sext16(r.word())) & MASK32
            flow = Flow.NEXT if cond == "t" else Flow.BRANCH
            return r.make("dbcc", None, _d(reg), cond=cond, flow=flow, target=target)
        return r.make("scc", None, r.ea(mode, reg, "b", _DALT), cond=cond)
    size = SIZES[sz]
    return r.make("subq" if w & 0x100 else "addq", size, _q(data or 8), r.ea(mode, reg, size, _ALT))


def _line6(r: _Reader, w: int) -> Insn:
    cond = (w >> 8) & 0xF
    base = r.pos
    disp = _sext16(r.word()) if w & 0xFF == 0 else _sext8(w & 0xFF)
    target = (base + disp) & MASK32
    if cond == 0:
        return r.make("bra", None, flow=Flow.JUMP, target=target)
    if cond == 1:
        return r.make("bsr", None, flow=Flow.CALL, target=target)
    return r.make("bcc", None, cond=CONDITIONS[cond], flow=Flow.BRANCH, target=target)


def _line7(r: _Reader, w: int) -> Insn:
    if w & 0x100:
        raise r.invalid("undefined line-7 instruction")
    return r.make("moveq", "l", _q(_sext8(w & 0xFF)), _d((w >> 9) & 7))


def _arith(r: _Reader, name: str, dn: int, opm: int, mode: int, reg: int,
           src_ok: frozenset[int], dst_ok: frozenset[int]) -> Insn:
    size = SIZES[opm & 3]
    if opm < 4:
        return r.make(name, size, r.ea(mode, reg, size, src_ok), _d(dn))
    return r.make(name, size, _d(dn), r.ea(mode, reg, size, dst_ok))


def _extended(r: _Reader, name: str, dn: int, mode: int, reg: int, size: str) -> Insn:
    """abcd, sbcd, addx, subx: register to register, or predecrement memory to memory."""
    if mode == 0:
        return r.make(name, size, _d(reg), _d(dn))
    return r.make(name, size, Operand(Mode.PREDEC, reg), Operand(Mode.PREDEC, dn))


def _line8(r: _Reader, w: int) -> Insn:
    dn, opm, mode, reg = _fields(w)
    if opm in (3, 7):
        return r.make("divu" if opm == 3 else "divs", "w", r.ea(mode, reg, "w", _DATA), _d(dn))
    if opm == 4 and mode < 2:
        return _extended(r, "sbcd", dn, mode, reg, "b")
    return _arith(r, "or", dn, opm, mode, reg, _DATA, _MALT)


def _addsub(r: _Reader, w: int) -> Insn:
    name = "sub" if w >> 12 == 9 else "add"
    dn, opm, mode, reg = _fields(w)
    if opm in (3, 7):
        size = "w" if opm == 3 else "l"
        return r.make(name + "a", size, r.ea(mode, reg, size, _ALL), _a(dn))
    if opm >= 4 and mode < 2:
        return _extended(r, name + "x", dn, mode, reg, SIZES[opm & 3])
    return _arith(r, name, dn, opm, mode, reg, _ALL, _MALT)


def _lineB(r: _Reader, w: int) -> Insn:
    dn, opm, mode, reg = _fields(w)
    if opm in (3, 7):
        size = "w" if opm == 3 else "l"
        return r.make("cmpa", size, r.ea(mode, reg, size, _ALL), _a(dn))
    size = SIZES[opm & 3]
    if opm < 3:
        return r.make("cmp", size, r.ea(mode, reg, size, _ALL), _d(dn))
    if mode == 1:
        return r.make("cmpm", size, Operand(Mode.POSTINC, reg), Operand(Mode.POSTINC, dn))
    return r.make("eor", size, _d(dn), r.ea(mode, reg, size, _DALT))


def _lineC(r: _Reader, w: int) -> Insn:
    dn, opm, mode, reg = _fields(w)
    if opm in (3, 7):
        return r.make("mulu" if opm == 3 else "muls", "w", r.ea(mode, reg, "w", _DATA), _d(dn))
    if opm == 4 and mode < 2:
        return _extended(r, "abcd", dn, mode, reg, "b")
    if opm == 5 and mode < 2:
        make = _d if mode == 0 else _a
        return r.make("exg", None, make(dn), make(reg))
    if opm == 6 and mode == 1:
        return r.make("exg", None, _d(dn), _a(reg))
    return _arith(r, "and", dn, opm, mode, reg, _DATA, _MALT)


_SHIFTS = ("as", "ls", "rox", "ro")


def _lineE(r: _Reader, w: int) -> Insn:
    count, _, mode, reg = _fields(w)
    direction = "l" if w & 0x100 else "r"
    sz = (w >> 6) & 3
    if sz == 3:
        if w & 0x800:
            raise r.invalid("68020 bit field instruction")
        return r.make(_SHIFTS[(w >> 9) & 3] + direction, "w", r.ea(mode, reg, "w", _MALT))
    src = _d(count) if w & 0x20 else _q(count or 8)
    return r.make(_SHIFTS[(w >> 3) & 3] + direction, SIZES[sz], src, _d(reg))


def _lineA(r: _Reader, w: int) -> Insn:
    raise r.invalid("line-A opcode")


def _lineF(r: _Reader, w: int) -> Insn:
    if w >> 8 == 0xFF:
        call = w & 0xFF
        return r.make("dos", None, _q(call), flow=Flow.STOP if call in DOS_NO_RETURN else Flow.NEXT)
    if w >> 8 == 0xFE:
        return r.make("fefunc", None, _q(w & 0xFF))
    raise r.invalid("line-F coprocessor opcode")


_LINES = (_line0, _move, _move, _move, _line4, _line5, _line6, _line7,
          _line8, _addsub, _lineA, _lineB, _lineC, _addsub, _lineE, _lineF)
