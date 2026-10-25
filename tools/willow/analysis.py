"""Control-flow analysis: which bytes of a program are code, and where its functions start.

Recursive descent from the entry point and the configured roots follows every
branch and call, and resolves the two jump table shapes the game uses:

- `jmp d8(pc,dn)` into a run of `bra` instructions (state machines in cot.r)
- `move.w T(pc,dn),dn` then `jmp/jsr B(pc,dn)`: word offsets from B, with the
  entry count bounded by an earlier `and.w #mask,dn` (ctnsnd.r)

Relocated pointers into the code section are then tried as further entry
points, because that is how function pointer tables look in a relocatable
program. Such a candidate is kept only if everything reachable from it decodes
cleanly and lines up with the code already found.

Control flow from trusted code may leave the header's code section: cot.r keeps
a few routines at the start of its data section.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Iterator

from .m68k import DecodeError, Flow, Insn, Mode, decode
from .program import Program

TRUSTED = ("entry", "root", "call")


@dataclass
class Analysis:
    program: Program
    insns: dict[int, Insn] = field(default_factory=dict)
    functions: dict[int, str] = field(default_factory=dict)  # start -> entry, root, call or pointer
    labels: set[int] = field(default_factory=set)  # branch and jump table targets
    tables: dict[int, list[int]] = field(default_factory=dict)  # table jump -> code addresses it reaches
    unresolved: set[int] = field(default_factory=set)  # indirect jumps with unknown targets
    indirect_calls: set[int] = field(default_factory=set)  # calls through registers or memory
    errors: dict[int, str] = field(default_factory=dict)  # bad decodes reached from trusted code
    rejected: dict[int, str] = field(default_factory=dict)  # pointer candidates that are not code

    @property
    def code_bytes(self) -> int:
        return sum(i.length for i in self.insns.values())

    def listing(self) -> Iterator[str]:
        """A labeled disassembly, with the bytes no code reaches summarized as data."""
        code, addr = self.program.code, 0
        while addr < len(code):
            insn = self.insns.get(addr)
            if insn is None:
                start = addr
                while addr < len(code) and addr not in self.insns:
                    addr += 1
                yield f"; data {start:#08x}-{addr:#08x} ({addr - start} bytes)"
                continue
            if addr in self.functions:
                yield f"\nfn_{addr:06x}:  ; found by {self.functions[addr]}"
            elif addr in self.labels:
                yield f"loc_{addr:06x}:"
            yield f"    {addr:06x}  {code[addr:addr + insn.length].hex():<20} {insn}"
            addr += insn.length

    def summary(self) -> dict:
        return {
            "instructions": len(self.insns),
            "code_bytes": self.code_bytes,
            "functions": {f"{a:#x}": how for a, how in sorted(self.functions.items())},
            "jump_tables": {f"{a:#x}": [f"{t:#x}" for t in ts] for a, ts in sorted(self.tables.items())},
            "unresolved_jumps": [f"{a:#x}" for a in sorted(self.unresolved)],
            "indirect_calls": [f"{a:#x}" for a in sorted(self.indirect_calls)],
            "errors": {f"{a:#x}": why for a, why in sorted(self.errors.items())},
            "rejected_pointers": {f"{a:#x}": why for a, why in sorted(self.rejected.items())},
        }


def internal_target(program: Program, insn: Insn) -> int | None:
    """The static target of a transfer, if it lies inside the program."""
    if insn.target is None:
        return None
    if insn.op in ("jmp", "jsr"):
        op = insn.operands[0]
        if op.mode is Mode.ABSW:
            return None  # low memory or the system area
        if op.mode is Mode.ABSL and op.ext not in program.relocations:
            return None  # an absolute address outside the program, such as ROM
    return insn.target


def analyze(program: Program) -> Analysis:
    a = _Analyzer(program)
    a.explore(program.entry, "entry")
    for root in program.roots:
        a.explore(root, "root")
    for start, end in program.entry_ranges:
        a.explore(start, "root")
        for addr in [x for x in a.result.insns if start <= x < end]:
            a.result.functions.setdefault(addr, "range")
    for candidate in a.pointer_candidates():
        if candidate in a.result.insns:
            # Already decoded inside another function, but jumped to through a pointer too:
            # it needs to be an entry point of its own.
            a.result.functions.setdefault(candidate, "pointer")
        else:
            a.explore(candidate, "pointer")
    return a.result


class _Analyzer:
    def __init__(self, program: Program):
        self.p = program
        self.result = Analysis(program)
        self.owner: dict[int, int] = {}  # every word an instruction covers -> its start

    def decode(self, addr: int) -> Insn:
        return decode(self.p.code, addr)

    def table_targets(self, insn: Insn, run: list[Insn]) -> list[int] | None:
        """Code addresses a `jmp/jsr d8(pc,dn)` can reach, or None if the shape is unknown."""
        op = insn.operands[0]
        prev = run[-2] if len(run) > 1 else None
        if (prev and prev.op == "move" and prev.size == "w" and prev.operands[0].mode is Mode.PCINDEX
                and prev.operands[1].mode is Mode.DREG and prev.operands[1].reg == op.index
                and prev.operands[0].index == op.index):
            mask = next((i.operands[0].value for i in reversed(run[:-2])
                         if i.op in ("and", "andi") and i.operands[0].mode is Mode.IMM
                         and i.operands[1].mode is Mode.DREG and i.operands[1].reg == op.index), None)
            if mask is None:
                return None
            table, targets = prev.operands[0].value, []
            end = table + (mask + 1) * 2
            for at in range(table, end, 2):
                if at >= end:  # the table stops where the code it jumps into begins
                    break
                target = op.value + int.from_bytes(self.p.code[at : at + 2], "big", signed=True)
                # Indices the code can never produce may read garbage; skip what can't be code.
                if 0 <= target < len(self.p.code) and not target & 1:
                    targets.append(target)
                    if target > table:
                        end = min(end, target)
            return targets
        # Otherwise the jump lands in a table of slots starting at the base. Look back for how
        # the index was scaled (add.w dn,dn / lsl.w #k,dn give the slot size) and bounded
        # (cmp.w #n,dn first). Slots hold a branch, or an rts for "nothing to do".
        scale, bound = 0, None
        for i in reversed(run[:-1]):
            ops = i.operands
            if not ops or ops[-1].mode is not Mode.DREG or ops[-1].reg != op.index:
                continue
            if i.op == "add" and ops[0].mode is Mode.DREG and ops[0].reg == op.index:
                scale += 1
            elif i.op in ("lsl", "asl") and ops[0].mode is Mode.IMM:
                scale += ops[0].value
            elif i.op in ("cmp", "cmpi") and ops[0].mode is Mode.IMM:
                bound = ops[0].value
                break
            else:
                break  # the index register was set some other way
        # Some tables are based on the jump itself (`jmp *(pc,dn)`), entries starting right after it.
        self_based = op.value == insn.addr
        targets = [] if self_based else [op.value]
        if scale:  # fixed-size slots
            for k in range(bound + 1 if bound is not None else 256):
                at = op.value + (k << scale)
                if at < insn.addr + insn.length and self_based:
                    continue
                try:
                    entry = self.decode(at)
                except DecodeError:
                    break
                is_slot = (entry.op in ("bra", "jmp") and entry.target is not None) or entry.op == "rts"
                if bound is None and not is_slot:
                    break
                targets.append(at)
        else:  # the index arrives pre-scaled (from memory): a run of branches, each an entry,
            # possibly with nop padding or an rts where an index means "nothing to do"
            at = insn.addr + insn.length if self_based else op.value
            seen_branch = False
            while True:
                try:
                    entry = self.decode(at)
                except DecodeError:
                    break
                branch = entry.op in ("bra", "jmp") and entry.target is not None
                if not branch and (seen_branch or entry.op not in ("rts", "nop")):
                    break  # padding comes only before the branches
                seen_branch |= branch
                targets.append(at)
                at += entry.length
        return sorted(set(targets))

    def explore(self, start: int, origin: str) -> None:
        """Decode everything reachable from `start`.

        Trusted origins commit whatever decodes and record failures as errors. A
        pointer candidate commits nothing unless its whole closure is clean.
        """
        strict = origin not in TRUSTED
        res = self.result
        insns: dict[int, Insn] = {}
        owner: dict[int, int] = {}
        functions, labels, tables, errors = {start: origin}, set(), {}, {}
        unresolved, indirect_calls = set(), set()
        work = [start]
        while work:
            addr = work.pop()
            run: list[Insn] = []
            while addr not in res.insns and addr not in insns:
                problem = None
                inside = self.owner.get(addr, owner.get(addr))
                if inside is not None:
                    problem = f"{addr:#x} lands inside the instruction at {inside:#x}"
                elif not 0 <= addr < len(self.p.code):
                    problem = f"{addr:#x} is outside the program"
                else:
                    try:
                        insn = self.decode(addr)
                    except DecodeError as e:
                        problem = f"{e.reason} at {addr:#x}"
                    else:
                        tail = range(addr + 2, addr + insn.length, 2)
                        clash = next((a for a in tail if a in self.owner or a in owner), None)
                        if clash is not None:
                            problem = f"the instruction at {addr:#x} overlaps code at {clash:#x}"
                if problem:
                    if strict:
                        res.rejected[start] = problem
                        return
                    errors[addr] = problem
                    break
                insns[addr] = insn
                run.append(insn)
                for a in range(addr, addr + insn.length, 2):
                    owner[a] = addr
                if insn.flow in (Flow.BRANCH, Flow.JUMP, Flow.CALL):
                    target = internal_target(self.p, insn)
                    if target is not None:
                        if insn.flow is Flow.CALL:
                            functions.setdefault(target, "call")
                        else:
                            labels.add(target)
                        work.append(target)
                    elif insn.target is None:
                        found = self.table_targets(insn, run) if insn.operands[0].mode is Mode.PCINDEX else None
                        if found is not None:
                            tables[addr] = found
                            for t in found:
                                if insn.flow is Flow.CALL:
                                    functions.setdefault(t, "call")
                                else:
                                    labels.add(t)
                            work.extend(found)
                        elif insn.flow is Flow.CALL:
                            indirect_calls.add(addr)
                        else:
                            unresolved.add(addr)
                if insn.flow in (Flow.JUMP, Flow.RETURN, Flow.STOP):
                    break
                addr += insn.length
        res.insns.update(insns)
        self.owner.update(owner)
        for addr, how in functions.items():
            res.functions.setdefault(addr, how)
        res.labels |= labels
        res.tables.update(tables)
        res.unresolved |= unresolved
        res.indirect_calls |= indirect_calls
        res.errors.update(errors)

    def pointer_candidates(self) -> list[int]:
        code, text = self.p.code, self.p.text_size
        found = set()
        for r in self.p.relocations:
            value = int.from_bytes(code[r : r + 4], "big")
            if value < text and not value & 1:
                found.add(value)
        return sorted(found)
