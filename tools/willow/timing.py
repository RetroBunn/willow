"""MC68000 instruction timings, in clock cycles.

Recompiled code runs far faster than a 10 MHz 68000. The game paces itself on
the vertical blank, but when a frame's work took longer than a frame on the real
machine, the game slowed down, and that slowdown is part of how it plays. So the
generated code counts the cycles each instruction would have taken, and the
runtime's clock follows that count instead of the host's.

The numbers are the ones in the M68000 User's Manual, section 8, with no wait
states: the X68000's memory runs the 68000 at full speed, as MAME models it.
Everything here is static; the few timings that depend on data (branches taken,
Scc to a register, shift counts, multiplications, traps taken) are finished off
by the code generator.
"""

from __future__ import annotations

from .m68k import Insn, Mode

# Effective address calculation: (byte or word, long).
_EA = {
    Mode.DREG: (0, 0), Mode.AREG: (0, 0), Mode.IND: (4, 8), Mode.POSTINC: (4, 8),
    Mode.PREDEC: (6, 10), Mode.DISP: (8, 12), Mode.INDEX: (10, 14), Mode.ABSW: (8, 12),
    Mode.ABSL: (12, 16), Mode.PCDISP: (8, 12), Mode.PCINDEX: (10, 14), Mode.IMM: (4, 8),
}
# A move's destination: like _EA, except that -(An) costs no more than (An).
_MOVE_DST = {**_EA, Mode.PREDEC: (4, 8)}
# Control addressing: lea, pea, jmp, jsr.
_CONTROL = {
    Mode.IND: (4, 12, 8, 16), Mode.DISP: (8, 16, 10, 18), Mode.INDEX: (12, 20, 14, 22),
    Mode.ABSW: (8, 16, 10, 18), Mode.ABSL: (12, 20, 12, 20), Mode.PCDISP: (8, 16, 10, 18),
    Mode.PCINDEX: (12, 20, 14, 22),
}
# movem before the per-register cost: (memory to registers, registers to memory).
_MOVEM = {
    Mode.IND: (12, 8), Mode.POSTINC: (12, 8), Mode.PREDEC: (12, 8), Mode.DISP: (16, 12),
    Mode.INDEX: (18, 14), Mode.ABSW: (16, 12), Mode.ABSL: (20, 16), Mode.PCDISP: (16, 12),
    Mode.PCINDEX: (18, 14),
}
_REGISTER = (Mode.DREG, Mode.AREG)
_FIXED = {
    "moveq": 4, "exg": 6, "swap": 4, "ext": 4, "nop": 4, "link": 16, "unlk": 12,
    "rts": 16, "rte": 20, "rtr": 20, "bra": 10, "bsr": 18, "trapv": 4, "stop": 4,
    "reset": 132, "movep": 16,
    # Exceptions: trap, illegal, and the line-F traps Human68k uses for DOS calls.
    "trap": 34, "illegal": 34, "dos": 34, "fefunc": 34,
    # Divisions take 76 to 140 (divu) or 158 (divs) cycles depending on the operands.
    "divu": 128, "divs": 146,
    # Multiplications: 38 plus 2 per bit, which the generated code adds.
    "mulu": 38, "muls": 38,
    # Taken or not, and the loop counter, decide these; the generated code counts them.
    "bcc": 0, "dbcc": 0,
}
_LOGIC = ("add", "sub", "and", "or")
_IMMEDIATE = ("addi", "subi", "andi", "ori", "eori")


def ea(op, size: str | None) -> int:
    """Address calculation and operand fetch time."""
    return _EA[op.mode][size == "l"]


def cycles(insn: Insn) -> int:
    """Clock cycles the instruction takes, apart from the data-dependent parts."""
    op, size, ops = insn.op, insn.size, insn.operands
    long = size == "l"
    if op in _FIXED:
        n = _FIXED[op]
        if op in ("divu", "divs", "mulu", "muls"):
            n += ea(ops[0], "w")
        if op == "movep" and long:
            n = 24
        return n
    if op == "move":
        src, dst = ops
        if src.mode is Mode.SR:
            return 6 if dst.mode is Mode.DREG else 8 + ea(dst, "w")
        if dst.mode in (Mode.SR, Mode.CCR):
            return 12 + ea(src, "w")
        if Mode.USP in (src.mode, dst.mode):
            return 4
        return 4 + ea(src, size) + _MOVE_DST[dst.mode][long]
    if op == "movea":
        return 4 + ea(ops[0], size)
    if op in ("lea", "pea", "jmp", "jsr"):
        return _CONTROL[ops[0].mode][("lea", "pea", "jmp", "jsr").index(op)]
    if op in ("clr", "neg", "negx", "not"):
        if ops[0].mode in _REGISTER:
            return 6 if long else 4
        return (12 if long else 8) + ea(ops[0], size)
    if op == "nbcd":
        return 6 if ops[0].mode is Mode.DREG else 8 + ea(ops[0], "b")
    if op == "tst":
        return 4 + ea(ops[0], size)
    if op == "movem":
        per = 8 if long else 4
        if ops[0].mode is Mode.REGLIST:
            mask, mem, to_regs = ops[0].reg, ops[1], False
        else:
            mem, mask, to_regs = ops[0], ops[1].reg, True
        return _MOVEM[mem.mode][0 if to_regs else 1] + per * bin(mask).count("1")
    if op in _LOGIC or op == "cmp":
        src, dst = ops
        if dst.mode is Mode.DREG:  # <ea>,Dn
            if not long:
                return 4 + ea(src, size)
            base = 8 if op != "cmp" and src.mode in _REGISTER + (Mode.IMM,) else 6
            return base + ea(src, size)
        return (12 if long else 8) + ea(dst, size)  # Dn,<ea>
    if op == "eor":
        dst = ops[1]
        if dst.mode is Mode.DREG:
            return 8 if long else 4
        return (12 if long else 8) + ea(dst, size)
    if op in ("adda", "suba"):
        src = ops[0]
        if not long:
            return 8 + ea(src, size)
        return (8 if src.mode in _REGISTER + (Mode.IMM,) else 6) + ea(src, size)
    if op == "cmpa":
        return 6 + ea(ops[0], size)
    if op in _IMMEDIATE:
        dst = ops[1]
        if dst.mode in (Mode.SR, Mode.CCR):
            return 20
        if dst.mode is Mode.DREG:
            return (14 if op == "andi" else 16) if long else 8
        return (20 if long else 12) + ea(dst, size)
    if op == "cmpi":
        dst = ops[1]
        if dst.mode is Mode.DREG:
            return 14 if long else 8
        return (12 if long else 8) + ea(dst, size)
    if op in ("addq", "subq"):
        dst = ops[1]
        if dst.mode is Mode.DREG:
            return 8 if long else 4
        if dst.mode is Mode.AREG:
            return 8
        return (12 if long else 8) + ea(dst, size)
    if op in ("addx", "subx", "abcd", "sbcd"):
        if ops[0].mode is Mode.DREG:
            return 8 if long else 4 if op in ("addx", "subx") else 6
        return 30 if long else 18
    if op == "cmpm":
        return 20 if long else 12
    if op in ("asl", "asr", "lsl", "lsr", "rol", "ror", "roxl", "roxr"):
        if len(ops) == 1:  # memory, by one bit
            return 8 + ea(ops[0], "w")
        count = 2 * ops[0].value if ops[0].mode is Mode.IMM else 0  # a register count is added at run time
        return (8 if long else 6) + count
    if op in ("btst", "bchg", "bclr", "bset"):
        num, dst = ops
        dynamic = num.mode is Mode.DREG
        if dst.mode is Mode.DREG:
            return {"btst": 6, "bchg": 8, "bset": 8, "bclr": 10}[op] + (0 if dynamic else 4)
        return (4 if op == "btst" else 8) + (0 if dynamic else 4) + ea(dst, "b")
    if op == "scc":  # to a register, 2 more when the condition holds, which the generated code adds
        return 4 if ops[0].mode is Mode.DREG else 8 + ea(ops[0], "b")
    if op == "tas":
        return 4 if ops[0].mode is Mode.DREG else 14 + ea(ops[0], "b")
    if op == "chk":
        return 10 + ea(ops[0], "w")
    raise ValueError(f"no timing for {insn.mnemonic} at {insn.addr:#x}")
