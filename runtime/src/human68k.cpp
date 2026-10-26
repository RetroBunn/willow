#include "human68k.hpp"

#include <algorithm>

#include "common.hpp"
#include "filesystem.hpp"
#include "machine.hpp"

namespace willow {

static constexpr uint32_t kHeader = 0x10, kProcessHeader = 0x100;
static constexpr uint32_t kNoMemory = 0x82000000, kNotEnough = 0x81000000;
static constexpr int32_t kBadFunction = -1, kBadBlock = -9;

static uint32_t align(uint32_t n) { return (n + 15) & ~15u; }

static std::string basename(const std::string &path)
{
    size_t cut = path.find_last_of("\\/:");
    return cut == std::string::npos ? path : path.substr(cut + 1);
}

void Human68k::init(uint32_t pool_start, uint32_t pool_end)
{
    pool_start_ = pool_start;
    pool_end_ = pool_end;
    // The shell everything descends from: a block with a process header and no program.
    blocks_ = {{pool_start, pool_start + kProcessHeader, 0}};
    current_ = pool_start;
    sync();
}

void Human68k::sync()
{
    Memory &mem = m_.mem;
    for (size_t i = 0; i < blocks_.size(); i++) {
        const Block &b = blocks_[i];
        mem.poke32(b.start + 0x0, i ? blocks_[i - 1].start : 0);
        mem.poke32(b.start + 0x4, b.owner);
        mem.poke32(b.start + 0x8, b.end);
        mem.poke32(b.start + 0xC, i + 1 < blocks_.size() ? blocks_[i + 1].start : 0);
    }
}

Human68k::Block *Human68k::find(uint32_t start)
{
    for (Block &b : blocks_)
        if (b.start == start) return &b;
    return nullptr;
}

uint32_t Human68k::largest_gap(uint32_t *where) const
{
    uint32_t best = 0, at = 0, prev_end = pool_start_;
    for (size_t i = 0; i <= blocks_.size(); i++) {
        uint32_t next = i < blocks_.size() ? blocks_[i].start : pool_end_;
        if (next > prev_end && next - prev_end > best) best = next - prev_end, at = prev_end;
        if (i < blocks_.size()) prev_end = blocks_[i].end;
    }
    if (where) *where = at;
    return best;
}

uint32_t Human68k::malloc(uint32_t size, uint32_t owner)
{
    uint32_t need = align(size + kHeader), prev_end = pool_start_;
    for (size_t i = 0; i <= blocks_.size(); i++) {
        uint32_t next = i < blocks_.size() ? blocks_[i].start : pool_end_;
        if (size <= 0xFFFFFF && next >= prev_end && next - prev_end >= need) {
            blocks_.insert(blocks_.begin() + long(i), {prev_end, prev_end + need, owner});
            sync();
            return prev_end + kHeader;
        }
        if (i < blocks_.size()) prev_end = blocks_[i].end;
    }
    uint32_t gap = largest_gap(nullptr);
    return gap > kHeader ? kNotEnough | ((gap - kHeader) & 0xFFFFFF) : kNoMemory;
}

int32_t Human68k::mfree(uint32_t ptr)
{
    if (ptr == 0) {  // everything the current process allocated
        blocks_.erase(std::remove_if(blocks_.begin(), blocks_.end(), [&](const Block &b) { return b.owner == current_; }),
                      blocks_.end());
        sync();
        return 0;
    }
    auto it = std::find_if(blocks_.begin(), blocks_.end(), [&](const Block &b) { return b.start == ptr - kHeader; });
    if (it == blocks_.end()) return kBadBlock;
    blocks_.erase(it);
    sync();
    return 0;
}

int32_t Human68k::setblock(uint32_t ptr, uint32_t size)
{
    for (size_t i = 0; i < blocks_.size(); i++) {
        Block &b = blocks_[i];
        if (b.start != ptr - kHeader) continue;
        uint32_t limit = i + 1 < blocks_.size() ? blocks_[i + 1].start : pool_end_;
        if (size > 0xFFFFFF || ptr + align(size) > limit) return int32_t(kNotEnough | ((limit - ptr) & 0xFFFFFF));
        b.end = ptr + align(size);
        sync();
        return 0;
    }
    return kBadBlock;
}

uint32_t Human68k::vector_slot(uint16_t vector) const
{
    if (vector < 0x100) return vector * 4u;
    if (vector < 0x200) return Machine::kIocsTable + (vector - 0x100) * 4u;
    return 0;  // DOS call vectors ($FFxx) are not redirected
}

uint32_t Human68k::intvcs(uint16_t vector, uint32_t address)
{
    uint32_t slot = vector_slot(vector);
    if (!slot && vector >= 0x100) {
        log_once(format("intvcs-%x", vector), "_INTVCS for $%04x ignored", vector);
        return 0;
    }
    uint32_t old = m_.mem.peek32(slot);
    m_.mem.poke32(slot, address);
    return old;
}

uint32_t Human68k::intvcg(uint16_t vector) const
{
    uint32_t slot = vector_slot(vector);
    return slot || vector == 0 ? m_.mem.peek32(slot) : 0;
}

int32_t Human68k::exec(const std::string &path, uint32_t cmdline, uint32_t env, uint32_t pc)
{
    const M68kProgram *program = m_.program(basename(path));
    if (!program) {
        log("_EXEC: no recompiled program named %s", path.c_str());
        return err::kFileNotFound;
    }
    // Like Human68k, give the new process the largest free area; it shrinks itself with _SETBLOCK.
    uint32_t start;
    uint32_t size = largest_gap(&start);
    uint32_t text = start + kProcessHeader, end = start + size;
    if (size < kProcessHeader + program->image_size + program->bss_size) return int32_t(kNoMemory);
    uint32_t parent = current_;
    blocks_.push_back({start, end, parent});
    std::sort(blocks_.begin(), blocks_.end(), [](const Block &a, const Block &b) { return a.start < b.start; });
    sync();

    Memory &mem = m_.mem;
    for (uint32_t a = start + kHeader; a < text; a++) mem.poke8(a, 0);
    mem.poke32(start + 0x10, env);
    mem.poke32(start + 0x20, cmdline);
    mem.poke32(start + 0x30, text + program->image_size);
    mem.poke32(start + 0x34, text + program->image_size + program->bss_size);
    mem.poke32(start + 0x38, end);
    mem.put_cstring(start + 0x80, "A:\\");
    mem.put_cstring(start + 0xC4, basename(path));
    // The file is read in, and the recompiled image goes in its place.
    if (m_.fs->charge_load(path) < 0) log("_EXEC: %s is not on the disks, so loading it takes no time", path.c_str());
    m_.busy(m_.fs->take_disk_time(), pc);
    m_.load(*program, text);

    // The new process gets fresh registers and its own user stack; _EXEC hands back d0 only.
    // The cycle count is the machine's clock, so it carries on through both switches.
    M68kContext saved = m_.ctx;
    uint32_t ssp = m_.supervisor() ? m_.ctx.a[7] : m_.ctx.other_sp;
    M68kContext &c = m_.ctx;
    c = M68kContext{};
    c.cycles = saved.cycles;
    c.deadline = saved.deadline;
    c.a[0] = start;
    c.a[1] = text + program->image_size + program->bss_size;
    c.a[2] = cmdline;
    c.a[3] = env;
    c.a[4] = text + program->entry;
    c.a[7] = end;
    c.other_sp = ssp;
    c.sr_hi = 0;  // user mode, interrupts enabled
    current_ = start;
    log("running %s", basename(path).c_str());
    m_.busy(program->unpack_cycles * Machine::kNsPerCycle, text + program->entry);

    ProcessExit result{0, false, 0};
    try {
        m68k_push32(&c, 0);
        m_.call(text + program->entry);
        log("%s returned without _EXIT", basename(path).c_str());
    } catch (const ProcessExit &e) {
        result = e;
    }

    if (result.keep) {
        find(start)->end = align(text + result.keep_size);
        log("%s stays resident (%u bytes)", basename(path).c_str(), result.keep_size);
    } else {
        blocks_.erase(std::remove_if(blocks_.begin(), blocks_.end(),
                                     [&](const Block &b) { return b.start == start || b.owner == start; }),
                      blocks_.end());
        m_.unload(start, end);
        m_.fs->close_owned(start);
        log("%s exited with code %u", basename(path).c_str(), result.code);
    }
    sync();
    current_ = parent;
    saved.cycles = c.cycles;
    saved.deadline = c.deadline;
    m_.ctx = saved;
    last_exit_ = (result.keep ? 0x10000u : 0u) | result.code;
    return result.code;
}

void Human68k::call(uint8_t number, uint32_t pc)
{
    M68kContext &c = m_.ctx;
    Memory &mem = m_.mem;
    FileSystem &fs = *m_.fs;
    uint32_t sp = c.a[7];
    auto w = [&](uint32_t off) { return mem.peek16(sp + off); };
    auto l = [&](uint32_t off) { return mem.peek32(sp + off); };
    auto str = [&](uint32_t off) { return mem.cstring(l(off)); };
    int32_t r = 0;

    switch (number) {
    case 0x00: throw ProcessExit{0, false, 0};                       // _EXIT
    case 0x09: log("text: %s", from_sjis(str(0)).c_str()); break;    // _PRINT
    case 0x0E: r = fs.chgdrv(w(0)); break;                           // _CHGDRV
    case 0x0F: r = fs.drvctrl(w(0) >> 8, w(0) & 0xFF); break;        // _DRVCTRL
    case 0x19: r = fs.current_drive(); break;                        // _CURDRV
    case 0x20: {                                                     // _SUPER
        uint32_t arg = l(0);
        if (arg == 0) {  // the user stack carries on as the supervisor stack
            if (m_.supervisor()) {
                r = -26;
                break;
            }
            r = int32_t(c.other_sp);
            c.other_sp = c.a[7];
            c.sr_hi |= 0x20;
        } else {
            uint32_t usp = c.a[7];
            c.sr_hi &= ~0x20;
            c.other_sp = arg;
            c.a[7] = usp;
        }
        break;
    }
    case 0x23: log_once(format("conctrl-%d", w(0)), "_CONCTRL mode %d ignored", w(0)); break;
    case 0x25: r = int32_t(intvcs(w(0), l(2))); break;               // _INTVCS
    case 0x31: throw ProcessExit{w(4), true, l(0)};                  // _KEEPPR
    case 0x34: r = fs.drvxchg(w(0), w(2)); break;                    // _DRVXCHG
    case 0x35: r = int32_t(intvcg(w(0))); break;                     // _INTVCG
    case 0x3B: r = fs.chdir(str(0)); break;                          // _CHDIR
    case 0x3C: r = fs.create(str(0), w(4), current_); break;         // _CREATE
    case 0x3D: r = fs.open(str(0), w(4), current_); break;           // _OPEN
    case 0x3E: r = fs.close(w(0)); break;                            // _CLOSE
    case 0x3F: r = fs.read(w(0), l(2), l(6)); break;                 // _READ
    case 0x40: r = fs.write(w(0), l(2), l(6)); break;                // _WRITE
    case 0x42: r = fs.seek(w(0), int32_t(l(2)), w(6)); break;        // _SEEK
    case 0x48: r = int32_t(malloc(l(0), current_)); break;           // _MALLOC
    case 0x49: r = mfree(l(0)); break;                               // _MFREE
    case 0x4A: r = setblock(l(0), l(4)); break;                      // _SETBLOCK
    case 0x4B:                                                       // _EXEC
        if (w(0) != 0) {
            log("_EXEC mode %d is not supported", w(0));
            r = kBadFunction;
        } else {
            r = exec(str(2), l(6), l(10), pc);
        }
        break;
    case 0x4C: throw ProcessExit{w(0), false, 0};                    // _EXIT2
    case 0x4D: r = int32_t(last_exit_); break;                       // _WAIT
    case 0x4E: r = fs.files(l(0), str(4), w(8)); break;              // _FILES
    case 0x4F: r = fs.nfiles(l(0)); break;                           // _NFILES
    default:
        log_once(format("dos-%02x", number), "DOS call $FF%02X is not implemented", number);
        r = kBadFunction;
    }
    m_.busy(fs.take_disk_time(), pc);
    c.d[0] = uint32_t(r);
}

}  // namespace willow
