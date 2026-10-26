// Human68k, reimplemented natively: processes, memory blocks and the DOS calls
// ($FFxx) the game makes. Memory blocks live in emulated RAM with the real
// layout, because programs read their own block and process headers:
//
//   +$00 previous block   +$04 owner process   +$08 end of block   +$0C next block
//
// A process's block continues with its 240-byte process block (PSP), so its
// program starts at block + $100.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace willow {

class Machine;

// Unwinds a running program back to the _EXEC that started it.
struct ProcessExit {
    uint16_t code;
    bool keep;           // _KEEPPR: stay resident
    uint32_t keep_size;  // bytes of the program to keep, from block + $100
};

class Human68k {
public:
    explicit Human68k(Machine &m) : m_(m) {}

    void init(uint32_t pool_start, uint32_t pool_end);
    // DOS call $FFxx returning to `pc`; arguments on the stack, result in d0. Calls that use
    // the disk take as long as the floppy drive would have, with interrupts running.
    void call(uint8_t number, uint32_t pc);

    // Load and run (mode 0). Loading takes the disk time of the file, and a packed program
    // then spends its unpacker's time before it starts.
    int32_t exec(const std::string &path, uint32_t cmdline, uint32_t env, uint32_t pc = 0);
    uint32_t malloc(uint32_t size, uint32_t owner);
    int32_t mfree(uint32_t ptr);
    int32_t setblock(uint32_t ptr, uint32_t size);
    uint32_t intvcs(uint16_t vector, uint32_t address);  // returns the previous address
    uint32_t intvcg(uint16_t vector) const;
    uint32_t current() const { return current_; }

private:
    struct Block {
        uint32_t start, end, owner;  // [start, end), header at start
    };
    void sync();  // rewrite the block headers in emulated memory
    Block *find(uint32_t start);
    uint32_t largest_gap(uint32_t *where) const;
    uint32_t vector_slot(uint16_t vector) const;

    Machine &m_;
    std::vector<Block> blocks_;  // sorted by address
    uint32_t pool_start_ = 0, pool_end_ = 0;
    uint32_t current_ = 0;  // the running process's block
    uint32_t last_exit_ = 0;
};

}  // namespace willow
