#include "boot.hpp"

#include "common.hpp"
#include "human68k.hpp"
#include "machine.hpp"

namespace willow {

// Low memory as this runtime lays it out (Human68k itself is native code here):
static constexpr uint32_t kIplStack = 0x2000;     // where the boot sector puts its stack
static constexpr uint32_t kBootSound = 0x3000;    // where the boot loader loads bootsnd.sys
static constexpr uint32_t kSystemStack = 0x6800;  // supervisor stack once Human68k is up
static constexpr uint32_t kEmptyLine = 0x7000;    // an empty command line for config.sys entries
static constexpr uint32_t kPoolStart = 0x10000;   // processes and memory blocks from here up

void boot(Machine &m)
{
    M68kContext &c = m.ctx;

    const M68kProgram *jingle = m.program("bootsnd.sys");
    if (!jingle) throw Fatal("bootsnd.sys is not linked in");
    c.sr_hi = 0x27;
    c.a[7] = kIplStack;
    m.load(*jingle, kBootSound);
    m68k_push32(&c, 0);
    m.call(kBootSound + jingle->entry);

    c.a[7] = kSystemStack;
    m.set_sr(0x2000);
    m.dos->init(kPoolStart, m.mem.ram_size());
    m.mem.poke16(kEmptyLine, 0);

    for (const char *program : {"ealogo.r", "bootcot.r"}) {
        int32_t code = m.dos->exec(program, kEmptyLine, 0);
        if (code < 0) throw Fatal(format("config.sys could not run %s (error %d)", program, code));
    }
    log("the shell exited");
}

}  // namespace willow
