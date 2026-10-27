#include "machine.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>

#include "common.hpp"
#include "filesystem.hpp"
#include "human68k.hpp"
#include "iocs.hpp"

namespace willow {

static Machine *g_machine = nullptr;

static std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

Debug Debug::from_environment()
{
    auto env = [](const char *name) {
        const char *v = std::getenv(name);
        return std::string(v ? v : "");
    };
    Debug d;
    d.trace = !env("WILLOW_TRACE").empty();
    if (std::string v = env("WILLOW_TRACE_VECTOR"); !v.empty()) d.trace_vector = int(std::strtol(v.c_str(), nullptr, 16));
    d.watch = env("WILLOW_WATCH");
    d.opm_log = env("WILLOW_OPM_LOG");
    d.mute_fm = env("WILLOW_MUTE") == "fm";
    d.mute_adpcm = env("WILLOW_MUTE") == "adpcm";
    if (std::string v = env("WILLOW_SOLO"); !v.empty()) d.solo = std::atoi(v.c_str());
    return d;
}

Machine::Machine(Options options, std::function<uint64_t()> clock)
    : mem(options.ram_size), debug_(options.debug), clock_(std::move(clock))
{
    g_machine = this;
    mem.map(Mfp::kBase, Mfp::kBase + Memory::kIoPage, &mfp);
    mem.map(Opm::kBase, Opm::kBase + Memory::kIoPage, &opm);
    mem.map(Ppi::kBase, Ppi::kBase + Memory::kIoPage, &ppi);
    mem.map(Dmac::kBase, Dmac::kBase + Memory::kIoPage, &dmac);
    mem.map(Adpcm::kBase, Adpcm::kBase + Memory::kIoPage, &adpcm);
    if (!debug_.opm_log.empty()) opm_log_.reset(std::fopen(debug_.opm_log.c_str(), "w"));
    dmac.trace = adpcm.trace = debug_.trace;
    sound.mute_fm = debug_.mute_fm;
    sound.mute_adpcm = debug_.mute_adpcm;
    sound.solo = debug_.solo;
    opm.on_write = [this](uint8_t reg, uint8_t value) {
        if (opm_log_) std::fprintf(opm_log_.get(),"%llu %02x %02x\n", (unsigned long long)now(), reg, value);
        sound.advance(now());  // so the write lands on the right sample
        sound.write_opm(reg, value);
    };
    opm.clock = [this] { return now(); };
    // The video registers are read straight from storage by the renderer.
    mem.storage(0xE80000, 0xE82000);  // CRTC
    mem.storage(0xE82000, 0xE84000);  // video controller and palettes
    mem.storage(0xEB0000, 0xEC0000);  // sprites, BG, PCG
    mem.storage(0xE8E000, 0xE90000);  // system port: contrast and friends
    mem.poke8(0xE8E001, 0x0F);        // contrast as the machine boots
    mfp.gpip_inputs = [this] { return gpip(now()); };
    opm.on_irq = [this] { mfp.raise(Mfp::kOpm); };

    // Reset state as the IPL ROM and IOCS leave it.
    for (uint32_t v = 0; v < 256; v++) mem.poke32(v * 4, kRomVectors + v * 4);
    for (uint32_t n = 0; n < 256; n++) mem.poke32(kIocsTable + n * 4, kRomIocs + n * 4);
    mfp.write8(Mfp::kBase + 0x17, 0x40);  // vectors $40-$4F, automatic end of interrupt
    ctx.sr_hi = 0x27;                     // supervisor, interrupts masked
    video.begin_frame();

    fs = std::make_unique<FileSystem>(mem);
    if (!options.data_dir.empty()) {
        fs->mount(0, options.data_dir / "system");
        fs->mount(1, options.data_dir / "data");
    }
    if (!options.save_dir.empty()) fs->set_overlay(options.save_dir);
    for (const Mod &mod : options.mods) fs->add_mod(mod.dir / "files");
    fs->set_speedup(options.settings.disk_speedup);
    video.sprites_per_line = options.settings.sprite_limit ? Video::kSpritesPerLine : 128;
    dos = std::make_unique<Human68k>(*this);
    iocs = std::make_unique<Iocs>(*this);
    // WILLOW_WATCH=<hex address>, such as e8e001 (the contrast), watches a fixed address.
    if (!debug_.watch.empty() && debug_.watch.find('+') == std::string::npos)
        watch(uint32_t(std::strtoul(debug_.watch.c_str(), nullptr, 16)));
}

void Machine::watch(uint32_t address)
{
    // Logs every write the CPU makes to the byte, with the frame and beam line it happened on.
    mem.watch = address & 0xFFFFFF;
    mem.on_watch = [this](uint8_t value) {
        log("watch %s <- %02x  frame %llu line %d  poll %06x", debug_.watch.c_str(), value,
            (unsigned long long)frames_.load(), beam_line(now()), last_poll.load());
    };
}

Machine::~Machine()
{
    if (g_machine == this) g_machine = nullptr;
}

Machine &Machine::get()
{
    if (!g_machine) throw Fatal("no machine");
    return *g_machine;
}

// ---- programs ----

void Machine::add_program(const M68kProgram *program) { programs_.push_back(program); }

const M68kProgram *Machine::program(const std::string &file) const
{
    std::string want = lower(file);
    for (const M68kProgram *p : programs_)
        if (lower(p->file) == want) return p;
    return nullptr;
}

void Machine::load(const M68kProgram &p, uint32_t base)
{
    if (base + p.image_size + p.bss_size > mem.ram_size()) throw Fatal(format("%s does not fit at %06x", p.file, base));
    std::memcpy(mem.data() + base, p.image, p.image_size);
    std::memset(mem.data() + base + p.image_size, 0, p.bss_size);
    for (uint32_t i = 0; i < p.relocation_count; i++) {
        uint32_t at = base + p.relocations[i];
        mem.poke32(at, mem.peek32(at) + base);
    }
    *p.base = base;
    // WILLOW_WATCH=<file>+<hex offset>, such as cot.r+df322, watches a byte of a program.
    size_t plus = debug_.watch.find('+');
    if (plus != std::string::npos && lower(debug_.watch.substr(0, plus)) == lower(p.file))
        watch(base + uint32_t(std::strtoul(debug_.watch.c_str() + plus + 1, nullptr, 16)));
    unload(base, base + p.image_size);
    loaded_.push_back({&p, base});
    log("loaded %s at %06x (%u bytes + %u bss, %u relocations)", p.file, base, p.image_size, p.bss_size, p.relocation_count);
}

void Machine::unload(uint32_t start, uint32_t end)
{
    loaded_.erase(std::remove_if(loaded_.begin(), loaded_.end(),
                                 [&](const Loaded &l) { return l.base >= start && l.base < end; }),
                  loaded_.end());
}

void Machine::call(uint32_t address)
{
    address &= 0xFFFFFF;
    for (const Loaded &l : loaded_) {
        if (address < l.base || address >= l.base + l.program->image_size) continue;
        uint32_t offset = address - l.base;
        const M68kFuncEntry *begin = l.program->functions, *end = begin + l.program->function_count;
        const M68kFuncEntry *f = std::lower_bound(begin, end, offset, [](const M68kFuncEntry &e, uint32_t o) { return e.offset < o; });
        if (f != end && f->offset == offset) {
            f->func(&ctx);
            return;
        }
        throw Fatal(format("no recompiled function at %06x (%s + %x)", address, l.program->file, offset));
    }
    throw Fatal(format("jump to %06x, outside every loaded program", address));
}

// ---- CPU ----

void Machine::set_sr(uint16_t sr)
{
    bool was_super = supervisor(), now_super = sr & 0x2000;
    if (was_super != now_super) std::swap(ctx.a[7], ctx.other_sp);
    uint8_t old_mask = ctx.sr_hi & 7;
    ctx.sr_hi = uint8_t(sr >> 8) & 0xA7;
    m68k_set_ccr(&ctx, uint8_t(sr));
    if ((ctx.sr_hi & 7) < old_mask) m68k_pending = 1;  // something held back may be deliverable now
}

void Machine::exception(int vector, uint32_t ret, int level)
{
    uint32_t handler = mem.peek32(uint32_t(vector) * 4);
    if (handler >= Memory::kRomStart) {
        log_once(format("vector-%d", vector), "exception vector %d ($%02x) has no handler; ignored", vector, vector);
        ctx.ret = ret;
        return;
    }
    uint16_t sr = m68k_get_sr(&ctx);
    if (!supervisor()) std::swap(ctx.a[7], ctx.other_sp);
    ctx.sr_hi = uint8_t((ctx.sr_hi | 0x20) & ~0x80);
    if (level >= 0) ctx.sr_hi = uint8_t((ctx.sr_hi & ~7) | level);
    m68k_push32(&ctx, ret);
    ctx.a[7] -= 2;
    mem.write16(ctx.a[7], sr);
    uint32_t frame = ctx.a[7];
    // WILLOW_TRACE_VECTOR also logs every poll site (function entries, loop back-edges)
    // reached inside that vector's handler.
    if (debug_.trace) log("exception $%02x -> %06x, frame %06x, sr %04x", vector, handler, frame, sr);
    if (vector == debug_.trace_vector) tracing_++, m68k_pending = 1;
    call(handler);  // the handler's rte pops the frame and restores SR
    if (vector == debug_.trace_vector) tracing_--;
    if (debug_.trace) log("exception $%02x done, a7 %06x, ret %06x", vector, ctx.a[7], ctx.ret);
    if (ctx.ret != ret)
        throw Fatal(format("handler for vector $%02x (%06x) returned to %06x instead of %06x; frame was at %06x, "
                           "stack now %06x (other %06x), SR %04x",
                           vector, handler, ctx.ret, ret, frame, ctx.a[7], ctx.other_sp, m68k_get_sr(&ctx)));
}

void Machine::trap(int number, uint32_t ret)
{
    if (number == 15) {
        uint8_t call_number = uint8_t(ctx.d[0]);
        uint32_t handler = mem.peek32(kIocsTable + call_number * 4u);
        if (handler >= Memory::kRomStart) {
            iocs->call(call_number);
        } else {
            // A program replaced this IOCS call: run it as the IOCS dispatcher would.
            uint16_t sr = m68k_get_sr(&ctx);
            set_sr(uint16_t(sr | 0x2000));
            m68k_push32(&ctx, ret);
            call(handler);
            set_sr(uint16_t((sr & 0xFF00) | (m68k_get_sr(&ctx) & 0xFF)));
        }
        ctx.ret = ret;
        return;
    }
    exception(32 + number, ret);
}

void Machine::line_f(uint16_t opcode, uint32_t ret)
{
    if (opcode >> 8 != 0xFF) throw Fatal(format("FLOAT call $%04x is not supported", opcode));
    dos->call(uint8_t(opcode), ret);
    ctx.ret = ret;
}

int Machine::beam_line(uint64_t t) const
{
    uint64_t elapsed = t > frame_start_ ? t - frame_start_ : 0;
    int total = video.total_lines();
    return elapsed >= kFramePeriodNs ? total : int(elapsed / (kFramePeriodNs / total));
}

uint8_t Machine::gpip(uint64_t t) const
{
    int line = beam_line(t);
    bool vdisp = line >= video.display_start() && line < video.display_end();  // high while displaying
    bool hsync = (t / 16000) & 1;  // ~31 kHz, close enough for polling loops
    uint8_t v = 0x67;                               // idle lines read high
    if (!opm.irq()) v |= 0x08;                      // the OPM's IRQ line is active low
    if (vdisp) v |= 0x10;
    if (hsync) v |= 0x80;
    return v;
}

bool Machine::deliver(uint32_t pc)
{
    int mask = ctx.sr_hi & 7, vector = mfp.pending_vector();
    int level;
    if (vector >= 0 && mask < 6) {  // the MFP interrupts at level 6
        mfp.acknowledge(vector);
        level = 6;
    } else if (dmac.irq_pending() && mask < 3) {  // the DMA controller at level 3
        vector = dmac.acknowledge();
        level = 3;
    } else {
        return false;
    }
    interrupts_++;
    ctx.cycles += 44;  // the 68000's interrupt acknowledge and exception processing
    exception(vector, pc, level);
    return true;
}

void Machine::service(uint32_t pc)
{
    m68k_pending = tracing_ ? 1 : 0;
    ctx.deadline = ctx.cycles + kSliceCycles;
    last_poll = pc;
    if (tracing_) log("  poll %06x  a7 %06x  d0 %08x d7 %08x", pc, ctx.a[7], ctx.d[0], ctx.d[7]);
    if (stop_requested) throw Shutdown{};
    uint64_t t = now();
    pace(t);
    sound.advance(t);  // may finish a DMA transfer, which interrupts
    ppi.joystick[0] = input.port(0);
    ppi.joystick[1] = input.port(1);
    uint8_t code;
    if (t >= next_key_ && !mfp.receive_full() && input.next_key(code)) {
        mfp.receive(code);
        next_key_ = t + 4'000'000;
    }
    opm.update(t);
    video.update_contrast(t);
    beam(t, pc);
    while (deliver(pc)) {
    }
}

void Machine::beam(uint64_t t, uint32_t pc)
{
    // Handle the frame's events in line order: a raster match at R09 (whose handler may move
    // R09 further down), the end of the display (VSYNC), the end of the frame. Lines are drawn
    // up to each event first, so register changes made at an event affect only what follows.
    for (;;) {
        int total = video.total_lines(), now_line = beam_line(t);
        int r09 = video.raster_irq_line(), vend = std::min(video.display_end(), total - 1);
        int event = total + 1, kind = 0;
        if (r09 >= raster_from_ && r09 <= now_line && r09 < total) event = r09, kind = 1;
        if (!vblank_done_ && vend <= now_line && vend < event) event = vend, kind = 2;
        if (now_line >= total && total < event) event = total, kind = 3;
        if (!kind) {
            video.render_raster(rendered_, now_line);
            rendered_ = std::max(rendered_, now_line);
            return;
        }
        video.render_raster(rendered_, event);
        rendered_ = std::max(rendered_, event);
        if (kind == 1) {
            raster_from_ = r09 + 1;
            mfp.raise(Mfp::kRaster);
        } else if (kind == 2) {
            vblank_done_ = true;
            video.end_frame();
            frames_++;
            mfp.raise(Mfp::kVdisp);
            if (on_frame) on_frame();
        } else {
            frame_start_ += kFramePeriodNs;
            if (t >= frame_start_ + 2 * kFramePeriodNs) frame_start_ = t - (t - frame_start_) % kFramePeriodNs;  // after a stall
            rendered_ = raster_from_ = 0;
            vblank_done_ = false;
            video.begin_frame();
        }
        while (deliver(pc)) {
        }
    }
}

void Machine::pace(uint64_t t)
{
    if (!wait) return;
    using namespace std::chrono;
    int64_t host = duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
    if (!paced_) host_origin_ = host - int64_t(t), paced_ = true;
    int64_t ahead = int64_t(t) - (host - host_origin_);
    if (ahead > 1'000'000) {
        wait(uint64_t(ahead));
    } else if (ahead < -100'000'000) {
        host_origin_ = host - int64_t(t);  // the host stalled: carry on from here rather than race to catch up
    }
}

void Machine::stop(uint16_t sr, uint32_t pc)
{
    // The CPU idles until an interrupt: let time pass up to each service until one is taken.
    set_sr(sr);
    uint64_t frame = frames_, taken = interrupts_;
    while (interrupts_ == taken && frames_ == frame) {
        ctx.cycles = std::max(ctx.cycles, ctx.deadline);
        service(pc);
    }
}

void Machine::busy(uint64_t ns, uint32_t pc)
{
    uint64_t until = ctx.cycles + ns / kNsPerCycle;
    while (ctx.cycles < until) {
        ctx.cycles = std::min(until, std::max(ctx.cycles, ctx.deadline));
        service(pc);
    }
}

}  // namespace willow

// ---- the interface recompiled code calls (runtime/include/willow/m68k.h) ----

using willow::Machine;

extern "C" {

volatile uint32_t m68k_pending = 0;

uint8_t m68k_read8(uint32_t addr) { return Machine::get().mem.read8(addr); }
uint16_t m68k_read16(uint32_t addr) { return Machine::get().mem.read16(addr); }
uint32_t m68k_read32(uint32_t addr) { return Machine::get().mem.read32(addr); }
void m68k_write8(uint32_t addr, uint8_t value) { Machine::get().mem.write8(addr, value); }
void m68k_write16(uint32_t addr, uint16_t value) { Machine::get().mem.write16(addr, value); }
void m68k_write32(uint32_t addr, uint32_t value) { Machine::get().mem.write32(addr, value); }

void m68k_trap(M68kContext *, int number, uint32_t ret) { Machine::get().trap(number, ret); }
void m68k_line_f(M68kContext *, uint16_t opcode, uint32_t ret) { Machine::get().line_f(opcode, ret); }
void m68k_exception(M68kContext *, int vector, uint32_t ret) { Machine::get().exception(vector, ret); }
void m68k_jump(M68kContext *, uint32_t target) { Machine::get().call(target); }
void m68k_set_sr(M68kContext *, uint16_t sr) { Machine::get().set_sr(sr); }
void m68k_stop(M68kContext *, uint16_t sr, uint32_t ret) { Machine::get().stop(sr, ret); }
void m68k_reset(M68kContext *) { willow::log_once("reset", "reset instruction ignored"); }
void m68k_service(M68kContext *, uint32_t pc) { Machine::get().service(pc); }

}
