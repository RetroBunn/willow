/*
 * Willow 68000 interface: what recompiled code expects from the runtime.
 *
 * The code generator (tools/willow/codegen.py) emits one C function per 68000
 * function. Each one takes the CPU context, reaches emulated memory through the
 * m68k_read and m68k_write functions, and calls back into the runtime for traps,
 * system calls, exceptions, and jumps it could not resolve statically.
 *
 * Calls push the real return address on the emulated stack and returns pop it
 * into ctx->ret, so 68000 code that inspects or rewrites its stack still works.
 */
#ifndef WILLOW_M68K_H
#define WILLOW_M68K_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct M68kContext {
    uint32_t d[8];
    uint32_t a[8];             /* a[7] is the active stack pointer */
    uint32_t other_sp;         /* the inactive one: USP in supervisor mode, SSP in user mode */
    uint8_t x, n, z, v, c;     /* condition codes, each 0 or 1 */
    uint8_t sr_hi;             /* system byte of SR: trace, supervisor, interrupt mask */
    uint32_t ret;              /* where the last rts, rte or rtr returned to */
    uint64_t cycles;           /* 68000 clock cycles the code has taken so far */
    uint64_t deadline;         /* when the cycle count reaches this, the runtime has work due */
} M68kContext;

typedef void (*M68kFunc)(M68kContext *ctx);

typedef struct M68kFuncEntry {
    uint32_t offset;           /* from the program's load address */
    M68kFunc func;
} M68kFuncEntry;

/* One recompiled program, as the code generator describes it. */
typedef struct M68kProgram {
    const char *file;                /* its name on disk, as Human68k programs refer to it */
    const uint8_t *image;            /* text + data as loaded, before any self-decryption */
    uint32_t image_size;
    uint32_t bss_size;
    uint32_t entry;                  /* offset of the entry point */
    const uint32_t *relocations;     /* offsets of longwords that get the load address added */
    uint32_t relocation_count;
    const M68kFuncEntry *functions;  /* sorted by offset */
    uint32_t function_count;
    uint32_t *base;                  /* where the runtime loaded it */
    uint64_t unpack_cycles;          /* how long its LZX stub took to unpack it; 0 if not packed */
} M68kProgram;

/*
 * Time and interrupts. Each instruction adds its 68000 cycles to ctx->cycles,
 * which is the machine's clock. Generated code polls at every loop back-edge
 * and function entry: once the count reaches ctx->deadline, or the runtime has
 * raised m68k_pending, m68k_service brings the devices up to date and delivers
 * whatever interrupt is due, as if between instructions. `pc` is the address
 * of the instruction about to run, which goes into the exception frame.
 */
extern volatile uint32_t m68k_pending;
void m68k_service(M68kContext *ctx, uint32_t pc);
#define M68K_POLL(ctx, pc)                                                         \
    do {                                                                           \
        if (m68k_pending || (ctx)->cycles >= (ctx)->deadline) m68k_service(ctx, pc); \
    } while (0)

/* Memory. Addresses are 24 bits wide (the top byte is ignored) and big-endian. */
uint8_t m68k_read8(uint32_t addr);
uint16_t m68k_read16(uint32_t addr);
uint32_t m68k_read32(uint32_t addr);
void m68k_write8(uint32_t addr, uint8_t value);
void m68k_write16(uint32_t addr, uint16_t value);
void m68k_write32(uint32_t addr, uint32_t value);

/*
 * Transfers the code generator hands to the runtime. Each one leaves ctx->ret
 * set to where execution continues; `ret` is where it continues normally.
 */
void m68k_trap(M68kContext *ctx, int number, uint32_t ret);       /* trap #n: IOCS, or a vector the game installed */
void m68k_line_f(M68kContext *ctx, uint16_t opcode, uint32_t ret); /* Human68k DOS ($FFxx) and FLOAT ($FExx) calls */
void m68k_exception(M68kContext *ctx, int vector, uint32_t ret);   /* illegal, division by zero, chk, trapv */
void m68k_jump(M68kContext *ctx, uint32_t target);                 /* run the function at a computed address */
void m68k_set_sr(M68kContext *ctx, uint16_t sr);                   /* may switch stacks and change the interrupt mask */
void m68k_stop(M68kContext *ctx, uint16_t sr, uint32_t ret);       /* idle until an interrupt, which returns to ret */
void m68k_reset(M68kContext *ctx);

static inline uint16_t m68k_get_sr(const M68kContext *c)
{
    return (uint16_t)(c->sr_hi << 8 | c->x << 4 | c->n << 3 | c->z << 2 | c->v << 1 | c->c);
}

static inline void m68k_set_ccr(M68kContext *c, uint8_t ccr)
{
    c->x = ccr >> 4 & 1;
    c->n = ccr >> 3 & 1;
    c->z = ccr >> 2 & 1;
    c->v = ccr >> 1 & 1;
    c->c = ccr & 1;
}

static inline void m68k_push32(M68kContext *c, uint32_t value)
{
    c->a[7] -= 4;
    m68k_write32(c->a[7], value);
}

static inline uint32_t m68k_pop32(M68kContext *c)
{
    uint32_t value = m68k_read32(c->a[7]);
    c->a[7] += 4;
    return value;
}

/* Arithmetic with condition codes, one set per operand size. */
#define WILLOW_M68K_SIZED(N, T)                                                       \
    static inline void m68k_logic##N(M68kContext *c, T r)                              \
    {                                                                                  \
        c->n = (uint8_t)(r >> (N - 1));                                                \
        c->z = r == 0;                                                                 \
        c->v = 0;                                                                      \
        c->c = 0;                                                                      \
    }                                                                                  \
    static inline T m68k_add##N(M68kContext *c, T s, T d)                              \
    {                                                                                  \
        T r = (T)(d + s);                                                              \
        c->n = (uint8_t)(r >> (N - 1));                                                \
        c->z = r == 0;                                                                 \
        c->v = (uint8_t)((T)((s ^ r) & (d ^ r)) >> (N - 1));                           \
        c->c = c->x = (uint8_t)((T)((s & d) | (~r & (s | d))) >> (N - 1));             \
        return r;                                                                      \
    }                                                                                  \
    static inline T m68k_sub##N(M68kContext *c, T s, T d)                              \
    {                                                                                  \
        T r = (T)(d - s);                                                              \
        c->n = (uint8_t)(r >> (N - 1));                                                \
        c->z = r == 0;                                                                 \
        c->v = (uint8_t)((T)((s ^ d) & (r ^ d)) >> (N - 1));                           \
        c->c = c->x = (uint8_t)((T)((s & ~d) | (r & ~d) | (s & r)) >> (N - 1));        \
        return r;                                                                      \
    }                                                                                  \
    static inline void m68k_cmp##N(M68kContext *c, T s, T d)                           \
    {                                                                                  \
        uint8_t x = c->x;                                                              \
        m68k_sub##N(c, s, d);                                                          \
        c->x = x;                                                                      \
    }                                                                                  \
    static inline T m68k_addx##N(M68kContext *c, T s, T d)                             \
    {                                                                                  \
        T r = (T)(d + s + c->x);                                                       \
        c->n = (uint8_t)(r >> (N - 1));                                                \
        if (r != 0) c->z = 0;                                                          \
        c->v = (uint8_t)((T)((s ^ r) & (d ^ r)) >> (N - 1));                           \
        c->c = c->x = (uint8_t)((T)((s & d) | (~r & (s | d))) >> (N - 1));             \
        return r;                                                                      \
    }                                                                                  \
    static inline T m68k_subx##N(M68kContext *c, T s, T d)                             \
    {                                                                                  \
        T r = (T)(d - s - c->x);                                                       \
        c->n = (uint8_t)(r >> (N - 1));                                                \
        if (r != 0) c->z = 0;                                                          \
        c->v = (uint8_t)((T)((s ^ d) & (r ^ d)) >> (N - 1));                           \
        c->c = c->x = (uint8_t)((T)((s & ~d) | (r & ~d) | (s & r)) >> (N - 1));        \
        return r;                                                                      \
    }                                                                                  \
    /* Shifts and rotates, one bit at a time so every count is exact. */             \
    static inline T m68k_asl##N(M68kContext *c, T v, unsigned count)                   \
    {                                                                                  \
        c->v = 0;                                                                      \
        c->c = 0;                                                                      \
        for (count &= 63; count; count--) {                                            \
            uint8_t out = (uint8_t)(v >> (N - 1));                                     \
            v = (T)(v << 1);                                                           \
            if ((uint8_t)(v >> (N - 1)) != out) c->v = 1;                              \
            c->c = c->x = out;                                                         \
        }                                                                              \
        c->n = (uint8_t)(v >> (N - 1));                                                \
        c->z = v == 0;                                                                 \
        return v;                                                                      \
    }                                                                                  \
    static inline T m68k_asr##N(M68kContext *c, T v, unsigned count)                   \
    {                                                                                  \
        c->c = 0;                                                                      \
        for (count &= 63; count; count--) {                                            \
            c->c = c->x = v & 1;                                                       \
            v = (T)((v >> 1) | (v & ((T)1 << (N - 1))));                               \
        }                                                                              \
        m68k_flags_nz##N(c, v);                                                        \
        return v;                                                                      \
    }                                                                                  \
    static inline T m68k_lsl##N(M68kContext *c, T v, unsigned count)                   \
    {                                                                                  \
        c->c = 0;                                                                      \
        for (count &= 63; count; count--) {                                            \
            c->c = c->x = (uint8_t)(v >> (N - 1));                                     \
            v = (T)(v << 1);                                                           \
        }                                                                              \
        m68k_flags_nz##N(c, v);                                                        \
        return v;                                                                      \
    }                                                                                  \
    static inline T m68k_lsr##N(M68kContext *c, T v, unsigned count)                   \
    {                                                                                  \
        c->c = 0;                                                                      \
        for (count &= 63; count; count--) {                                            \
            c->c = c->x = v & 1;                                                       \
            v = (T)(v >> 1);                                                           \
        }                                                                              \
        m68k_flags_nz##N(c, v);                                                        \
        return v;                                                                      \
    }                                                                                  \
    static inline T m68k_rol##N(M68kContext *c, T v, unsigned count)                   \
    {                                                                                  \
        c->c = 0;                                                                      \
        for (count &= 63; count; count--) {                                            \
            c->c = (uint8_t)(v >> (N - 1));                                            \
            v = (T)((v << 1) | c->c);                                                  \
        }                                                                              \
        m68k_flags_nz##N(c, v);                                                        \
        return v;                                                                      \
    }                                                                                  \
    static inline T m68k_ror##N(M68kContext *c, T v, unsigned count)                   \
    {                                                                                  \
        c->c = 0;                                                                      \
        for (count &= 63; count; count--) {                                            \
            c->c = v & 1;                                                              \
            v = (T)((v >> 1) | ((T)c->c << (N - 1)));                                  \
        }                                                                              \
        m68k_flags_nz##N(c, v);                                                        \
        return v;                                                                      \
    }                                                                                  \
    static inline T m68k_roxl##N(M68kContext *c, T v, unsigned count)                  \
    {                                                                                  \
        c->c = c->x;                                                                   \
        for (count &= 63; count; count--) {                                            \
            uint8_t out = (uint8_t)(v >> (N - 1));                                     \
            v = (T)((v << 1) | c->x);                                                  \
            c->c = c->x = out;                                                         \
        }                                                                              \
        m68k_flags_nz##N(c, v);                                                        \
        return v;                                                                      \
    }                                                                                  \
    static inline T m68k_roxr##N(M68kContext *c, T v, unsigned count)                  \
    {                                                                                  \
        c->c = c->x;                                                                   \
        for (count &= 63; count; count--) {                                            \
            uint8_t out = v & 1;                                                       \
            v = (T)((v >> 1) | ((T)c->x << (N - 1)));                                  \
            c->c = c->x = out;                                                         \
        }                                                                              \
        m68k_flags_nz##N(c, v);                                                        \
        return v;                                                                      \
    }

/* N and Z from a result, V cleared; shifts and rotates set C themselves. */
#define WILLOW_M68K_NZ(N, T)                                                          \
    static inline void m68k_flags_nz##N(M68kContext *c, T r)                           \
    {                                                                                  \
        c->n = (uint8_t)(r >> (N - 1));                                                \
        c->z = r == 0;                                                                 \
        c->v = 0;                                                                      \
    }

WILLOW_M68K_NZ(8, uint8_t)
WILLOW_M68K_NZ(16, uint16_t)
WILLOW_M68K_NZ(32, uint32_t)
WILLOW_M68K_SIZED(8, uint8_t)
WILLOW_M68K_SIZED(16, uint16_t)
WILLOW_M68K_SIZED(32, uint32_t)

/* Set bits, for the multiplications' timing. */
static inline unsigned m68k_bits16(uint16_t v)
{
    unsigned n = 0;
    for (; v; v &= (uint16_t)(v - 1)) n++;
    return n;
}

static inline uint32_t m68k_mulu(M68kContext *c, uint16_t s, uint16_t d)
{
    uint32_t r = (uint32_t)s * d;
    m68k_logic32(c, r);
    return r;
}

static inline uint32_t m68k_muls(M68kContext *c, uint16_t s, uint16_t d)
{
    uint32_t r = (uint32_t)((int32_t)(int16_t)s * (int16_t)d);
    m68k_logic32(c, r);
    return r;
}

/* Divisions return -1 on division by zero (the caller raises the exception), 1 on
 * overflow (the register is left alone), and 0 when the result was stored. */
static inline int m68k_divu(M68kContext *c, uint32_t *reg, uint16_t s)
{
    uint32_t q, r;
    c->c = 0;
    if (s == 0) return -1;
    q = *reg / s;
    r = *reg % s;
    if (q > 0xFFFF) {
        c->v = 1;
        c->n = 1;
        return 1;
    }
    *reg = r << 16 | q;
    c->n = (uint8_t)(q >> 15 & 1);
    c->z = q == 0;
    c->v = 0;
    return 0;
}

static inline int m68k_divs(M68kContext *c, uint32_t *reg, uint16_t s)
{
    int64_t dividend = (int32_t)*reg, divisor = (int16_t)s, q, r;
    c->c = 0;
    if (divisor == 0) return -1;
    q = dividend / divisor;
    r = dividend % divisor;
    if (q < -32768 || q > 32767) {
        c->v = 1;
        c->n = 1;
        return 1;
    }
    *reg = (uint32_t)(uint16_t)r << 16 | (uint16_t)q;
    c->n = (uint8_t)((uint16_t)q >> 15);
    c->z = q == 0;
    c->v = 0;
    return 0;
}

/* Packed BCD. N and V are undefined on the 68000; these follow common practice. */
static inline uint8_t m68k_abcd(M68kContext *c, uint8_t s, uint8_t d)
{
    unsigned r = (s & 0x0F) + (d & 0x0F) + c->x;
    unsigned v = ~r;
    if (r > 9) r += 6;
    r += (s & 0xF0) + (d & 0xF0);
    c->c = c->x = r > 0x99;
    if (c->c) r -= 0xA0;
    c->v = (uint8_t)((v & r) >> 7 & 1);
    c->n = (uint8_t)(r >> 7 & 1);
    if ((uint8_t)r) c->z = 0;
    return (uint8_t)r;
}

static inline uint8_t m68k_sbcd(M68kContext *c, uint8_t s, uint8_t d)
{
    unsigned r = (d & 0x0F) - (s & 0x0F) - c->x;
    unsigned v = ~r;
    if (r > 9) r -= 6;
    r += (d & 0xF0) - (s & 0xF0);
    c->c = c->x = r > 0x99;
    if (c->c) r += 0xA0;
    r &= 0xFF;
    c->v = (uint8_t)((v & r) >> 7 & 1);
    c->n = (uint8_t)(r >> 7 & 1);
    if (r) c->z = 0;
    return (uint8_t)r;
}

static inline uint8_t m68k_nbcd(M68kContext *c, uint8_t d)
{
    unsigned r = (0x9A - d - c->x) & 0xFF;
    if (r != 0x9A) {
        unsigned v = ~r;
        if ((r & 0x0F) == 0x0A) r = (r & 0xF0) + 0x10;
        r &= 0xFF;
        c->v = (uint8_t)((v & r) >> 7 & 1);
        if (r) c->z = 0;
        c->c = c->x = 1;
    } else {
        c->v = 0;
        c->c = c->x = 0;
        r = 0;
    }
    c->n = (uint8_t)(r >> 7 & 1);
    return (uint8_t)r;
}

#ifdef __cplusplus
}
#endif

#endif
