// Starting the game the way the real machine does.
#pragma once

namespace willow {

class Machine;

// Runs on the game thread until the shell exits or the machine is stopped:
//   1. the boot loader's part: bootsnd.sys at $3000, called as a subroutine
//   2. HUMAN.SYS's part: memory pool, user mode, interrupts on
//   3. config.sys: PROGRAM = ealogo.r, then SHELL = bootcot.r, which runs cot.r
void boot(Machine &m);

}  // namespace willow
