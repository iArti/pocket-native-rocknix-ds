/* the interface between recompiled code and the emulator running it. */

#ifndef RECOMP_H
#define RECOMP_H

#include <math.h>
#include <stdint.h>
#include <string.h>

/* code built with RECOMP_PINNED keeps the guest's registers in the host's,
   and is entered through recomp_enter alone: version 6, which a host that
   only knows 5 refuses. The Context is the same but for the fields at its
   end, which version 5 code never reads. */
#ifdef RECOMP_PINNED
#define RECOMP_ABI 6
#else
#define RECOMP_ABI 5
#endif

typedef struct Context Context;
#ifdef RECOMP_PINNED
/* a function of pinned code: entered_ is 0 from a call that knows it starts
   at the function's entry, anything else to start at ctx->r[15]. It gives
   back where the guest returned to, bit 0 set for Thumb, or RECOMP_EXITED
   with ctx->exit set. */
typedef uint32_t (*Code)(uint32_t);
#define RECOMP_FN(name) uint32_t name(uint32_t entered_)
#else
typedef void (*Code)(Context *);
#define RECOMP_FN(name) void name(Context *ctx)
#endif

typedef struct Host {
    uint8_t (*read8)(Context *, uint32_t);
    uint16_t (*read16)(Context *, uint32_t);
    uint32_t (*read32)(Context *, uint32_t);
    void (*write8)(Context *, uint32_t, uint8_t);
    void (*write16)(Context *, uint32_t, uint16_t);
    void (*write32)(Context *, uint32_t, uint32_t);
    /* runs one instruction the recompiler left to the interpreter. when it
       branches or fails the host sets exit and r15. */
    void (*interpret)(Context *, uint32_t address, uint32_t opcode);
    /* the code for an address, bit 0 set for Thumb, or null. */
    Code (*lookup)(Context *, uint32_t address);
} Host;

/* why the code gave control back to the host. */
enum {
    EXIT_NONE,
    /* an svc, its number is in svc and r15 points after it. */
    EXIT_SVC,
    /* the budget ran out, r15 is where to resume. */
    EXIT_BUDGET,
    /* anything else, the host carries on from r15. */
    EXIT_UNWIND,
};

struct Context {
    uint32_t r[16];
    uint8_t n, z, c, v, q, thumb, ge;
    /* whether ldrex marked exclusive_address. */
    uint8_t exclusive;
    int32_t budget;
    uint32_t exit;
    uint32_t svc;
    uint32_t depth;
    uint32_t exclusive_address;
    /* the read-only thread id register, which holds the thread's TLS
       address. */
    uint32_t tls;
    /* a host pointer for each 4 KiB page, or null when the host has to
       handle the access. */
    uint8_t *const *read_pages;
    uint8_t *const *write_pages;
    /* the VFP registers, s0 to s31 with dN in s2N and s2N+1, and fpscr,
       which the interpreter works on too. */
    uint32_t *vfp;
    uint32_t *fpscr;
    const Host *host;
    void *user;
    /* every guest address at the same offset from here, each page mapped,
       or null when the host has no such view. code built with
       RECOMP_FASTMEM reads and writes through it. */
    uint8_t *fastmem;
    /* for pinned code. the lowest the host's stack may go in guest calls,
       which recomp_enter sets: a function that finds it lower unwinds. */
    uintptr_t stack_floor;
    /* the host's table of the code it found for addresses lately, each in
       the slot its address picks, which pinned code looks in before it asks
       the host. RECOMP_FOUND slots. */
    const struct Found *found;
};

/* a slot of Context::found: code is null where the host found none. */
typedef struct Found {
    uint32_t address;
    Code code;
} Found;

#define RECOMP_FOUND 0x10000

typedef struct Entry {
    uint32_t address;
    Code code;
} Entry;

/* a module's code, whose entries are offsets from where it gets loaded. */
typedef struct Module {
    const char *name;
    /* where the host loaded it, which the code reads. */
    uint32_t *base;
    /* the end of its code, as an offset. */
    uint32_t size;
    uint32_t count;
    const Entry *entries;
} Module;

#ifdef _WIN32
#define RECOMP_EXPORT __declspec(dllexport)
#else
#define RECOMP_EXPORT __attribute__((visibility("default")))
#endif
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

/* RECOMP_PINNED, for AArch64 Linux built by gcc: the context, the view of
   memory, the budget and r0 to r14 each live in a register of the host's
   for as long as the code runs, rather than in the Context, which an
   in-order core like the Cortex-A55 pays for with a load and a store around
   every guest instruction. The registers are the host's own to keep across
   calls (x19 to x28) and, for r7 to r14, ones any call out of the code may
   change (x9 to x15 and x18), so those go to the Context around the few
   calls to the host. r15, the flags and everything else stay in the
   Context. recomp_enter, in entries.c, loads and stores them all.
   RECOMP_TABLES is for that file, which must leave the registers alone. */
#if defined(RECOMP_PINNED) && !defined(RECOMP_TABLES)
#if !defined(__aarch64__) || !defined(__GNUC__) || defined(__clang__)
#error RECOMP_PINNED is for gcc on AArch64
#endif
#if !defined(RECOMP_FASTMEM) || defined(RECOMP_FLAG_LOCALS)
#error RECOMP_PINNED needs RECOMP_FASTMEM and goes without RECOMP_FLAG_LOCALS
#endif
register Context *ctx __asm__("x19");
register uint8_t *recomp_view __asm__("x20");
register int32_t recomp_budget __asm__("w21");
register uint32_t recomp_r0 __asm__("w22"), recomp_r1 __asm__("w23"), recomp_r2 __asm__("w24"), recomp_r3 __asm__("w25");
register uint32_t recomp_r4 __asm__("w26"), recomp_r5 __asm__("w27"), recomp_r6 __asm__("w28");
register uint32_t recomp_r7 __asm__("w9"), recomp_r8 __asm__("w10"), recomp_r9 __asm__("w11"), recomp_r10 __asm__("w12");
register uint32_t recomp_r11 __asm__("w13"), recomp_r12 __asm__("w14"), recomp_r13 __asm__("w15"), recomp_r14 __asm__("w18");
register uintptr_t recomp_sp __asm__("sp");
#define R0 recomp_r0
#define R1 recomp_r1
#define R2 recomp_r2
#define R3 recomp_r3
#define R4 recomp_r4
#define R5 recomp_r5
#define R6 recomp_r6
#define R7 recomp_r7
#define R8 recomp_r8
#define R9 recomp_r9
#define R10 recomp_r10
#define R11 recomp_r11
#define R12 recomp_r12
#define R13 recomp_r13
#define R14 recomp_r14
/* r7 to r14 to the Context and back, around a call to the host */
#define RECOMP_SPILL_VOLATILE() do { \
    ctx->r[7] = R7; ctx->r[8] = R8; ctx->r[9] = R9; ctx->r[10] = R10; \
    ctx->r[11] = R11; ctx->r[12] = R12; ctx->r[13] = R13; ctx->r[14] = R14; \
} while (0)
#define RECOMP_FILL_VOLATILE() do { \
    R7 = ctx->r[7]; R8 = ctx->r[8]; R9 = ctx->r[9]; R10 = ctx->r[10]; \
    R11 = ctx->r[11]; R12 = ctx->r[12]; R13 = ctx->r[13]; R14 = ctx->r[14]; \
} while (0)
/* all of them, around the interpreter, which works on the Context */
#define RECOMP_SPILL() do { \
    ctx->r[0] = R0; ctx->r[1] = R1; ctx->r[2] = R2; ctx->r[3] = R3; \
    ctx->r[4] = R4; ctx->r[5] = R5; ctx->r[6] = R6; \
    RECOMP_SPILL_VOLATILE(); \
} while (0)
#define RECOMP_FILL() do { \
    R0 = ctx->r[0]; R1 = ctx->r[1]; R2 = ctx->r[2]; R3 = ctx->r[3]; \
    R4 = ctx->r[4]; R5 = ctx->r[5]; R6 = ctx->r[6]; \
    RECOMP_FILL_VOLATILE(); \
} while (0)
#else
#define R0 (ctx->r[0])
#define R1 (ctx->r[1])
#define R2 (ctx->r[2])
#define R3 (ctx->r[3])
#define R4 (ctx->r[4])
#define R5 (ctx->r[5])
#define R6 (ctx->r[6])
#define R7 (ctx->r[7])
#define R8 (ctx->r[8])
#define R9 (ctx->r[9])
#define R10 (ctx->r[10])
#define R11 (ctx->r[11])
#define R12 (ctx->r[12])
#define R13 (ctx->r[13])
#define R14 (ctx->r[14])
#endif

static inline uint8_t (mem_read8)(Context *ctx, uint32_t address) {
    uint8_t *page = ctx->read_pages[address >> 12];
    if (LIKELY(page)) return page[address & 0xFFF];
    return ctx->host->read8(ctx, address);
}

static inline uint16_t (mem_read16)(Context *ctx, uint32_t address) {
    uint8_t *page = ctx->read_pages[address >> 12];
    uint32_t offset = address & 0xFFF;
    if (LIKELY(page && offset <= 0xFFE)) {
        return *(const uint16_t *)(page + offset);
    }
    return ctx->host->read16(ctx, address);
}

static inline uint32_t (mem_read32)(Context *ctx, uint32_t address) {
    uint8_t *page = ctx->read_pages[address >> 12];
    uint32_t offset = address & 0xFFF;
    if (LIKELY(page && offset <= 0xFFC)) {
        return *(const uint32_t *)(page + offset);
    }
    return ctx->host->read32(ctx, address);
}

static inline void (mem_write8)(Context *ctx, uint32_t address, uint8_t value) {
    uint8_t *page = ctx->write_pages[address >> 12];
    if (LIKELY(page)) page[address & 0xFFF] = value;
    else ctx->host->write8(ctx, address, value);
}

static inline void (mem_write16)(Context *ctx, uint32_t address, uint16_t value) {
    uint8_t *page = ctx->write_pages[address >> 12];
    uint32_t offset = address & 0xFFF;
    if (LIKELY(page && offset <= 0xFFE)) *(uint16_t *)(page + offset) = value;
    else ctx->host->write16(ctx, address, value);
}

static inline void (mem_write32)(Context *ctx, uint32_t address, uint32_t value) {
    uint8_t *page = ctx->write_pages[address >> 12];
    uint32_t offset = address & 0xFFF;
    if (LIKELY(page && offset <= 0xFFC)) *(uint32_t *)(page + offset) = value;
    else ctx->host->write32(ctx, address, value);
}

#define mem_read8(ctx, address) ({ \
    Context *__c = (ctx); \
    uint32_t __a = (address); \
    uint8_t *__p = __c->read_pages[__a >> 12]; \
    LIKELY(__p) ? __p[__a & 0xFFF] : __c->host->read8(__c, __a); \
})

#define mem_read16(ctx, address) ({ \
    Context *__c = (ctx); \
    uint32_t __a = (address); \
    uint8_t *__p = __c->read_pages[__a >> 12]; \
    uint32_t __off = __a & 0xFFF; \
    LIKELY(__p && __off <= 0xFFE) ? \
        *(const uint16_t *)(__p + __off) : \
        __c->host->read16(__c, __a); \
})

#define mem_read32(ctx, address) ({ \
    Context *__c = (ctx); \
    uint32_t __a = (address); \
    uint8_t *__p = __c->read_pages[__a >> 12]; \
    uint32_t __off = __a & 0xFFF; \
    LIKELY(__p && __off <= 0xFFC) ? \
        *(const uint32_t *)(__p + __off) : \
        __c->host->read32(__c, __a); \
})

#define mem_write8(ctx, address, value) do { \
    Context *__c = (ctx); \
    uint32_t __a = (address); \
    uint8_t __v = (value); \
    uint8_t *__p = __c->write_pages[__a >> 12]; \
    if (LIKELY(__p)) { \
        __p[__a & 0xFFF] = __v; \
    } else { \
        __c->host->write8(__c, __a, __v); \
    } \
} while (0)

#define mem_write16(ctx, address, value) do { \
    Context *__c = (ctx); \
    uint32_t __a = (address); \
    uint16_t __v = (value); \
    uint8_t *__p = __c->write_pages[__a >> 12]; \
    uint32_t __off = __a & 0xFFF; \
    if (LIKELY(__p && __off <= 0xFFE)) { \
        *(uint16_t *)(__p + __off) = __v; \
    } else { \
        __c->host->write16(__c, __a, __v); \
    } \
} while (0)

#define mem_write32(ctx, address, value) do { \
    Context *__c = (ctx); \
    uint32_t __a = (address); \
    uint32_t __v = (value); \
    uint8_t *__p = __c->write_pages[__a >> 12]; \
    uint32_t __off = __a & 0xFFF; \
    if (LIKELY(__p && __off <= 0xFFC)) { \
        *(uint32_t *)(__p + __off) = __v; \
    } else { \
        __c->host->write32(__c, __a, __v); \
    } \
} while (0)

#ifdef RECOMP_FASTMEM
/* one access at the address's own offset in the host's view of the guest's
   memory, which has every page mapped */
#undef mem_read8
#undef mem_read16
#undef mem_read32
#undef mem_write8
#undef mem_write16
#undef mem_write32
/* the view, which generated code holds in a local: a store through it
   could be to the context as far as the compiler knows, which would have it
   load ctx->fastmem again after every one */
#if defined(RECOMP_PINNED) && !defined(RECOMP_TABLES)
#define RECOMP_VIEW(ctx) recomp_view
#else
#define RECOMP_VIEW(ctx) ((ctx)->fastmem)
#endif
#define mem_read8(ctx, address) (*(const uint8_t *)(RECOMP_VIEW(ctx) + (uint32_t)(address)))
#define mem_read16(ctx, address) ({ uint16_t __v; __builtin_memcpy(&__v, RECOMP_VIEW(ctx) + (uint32_t)(address), 2); __v; })
#define mem_read32(ctx, address) ({ uint32_t __v; __builtin_memcpy(&__v, RECOMP_VIEW(ctx) + (uint32_t)(address), 4); __v; })
#define mem_write8(ctx, address, value) do { *(RECOMP_VIEW(ctx) + (uint32_t)(address)) = (uint8_t)(value); } while (0)
#define mem_write16(ctx, address, value) do { uint16_t __v = (value); __builtin_memcpy(RECOMP_VIEW(ctx) + (uint32_t)(address), &__v, 2); } while (0)
#define mem_write32(ctx, address, value) do { uint32_t __v = (value); __builtin_memcpy(RECOMP_VIEW(ctx) + (uint32_t)(address), &__v, 4); } while (0)
#endif

#define C_EQ (F_Z)
#define C_NE (!F_Z)
#define C_CS (F_C)
#define C_CC (!F_C)
#define C_MI (F_N)
#define C_PL (!F_N)
#define C_VS (F_V)
#define C_VC (!F_V)
#define C_HI (F_C && !F_Z)
#define C_LS (!F_C || F_Z)
#define C_GE (F_N == F_V)
#define C_LT (F_N != F_V)
#define C_GT (!F_Z && F_N == F_V)
#define C_LE (F_Z || F_N != F_V)

static inline uint32_t ror32(uint32_t value, uint32_t amount) {
    amount &= 31;
    return amount ? (value >> amount) | (value << (32 - amount)) : value;
}

/* saturates to 32 bits, setting q when it has to. */
static inline uint32_t saturate(Context *ctx, int64_t value) {
    if (value > INT32_MAX) { ctx->q = 1; return 0x7FFFFFFFu; }
    if (value < INT32_MIN) { ctx->q = 1; return 0x80000000u; }
    return (uint32_t)(int32_t)value;
}

/* keeps the low 32 bits of a sum, setting q when that loses some. */
static inline uint32_t accumulate(Context *ctx, int64_t value) {
    if (value != (int32_t)value) ctx->q = 1;
    return (uint32_t)value;
}

/* the bottom or top halfword, signed. */
#define HALF(value, top) ((int64_t)(int16_t)((top) ? (value) >> 16 : (value)))

/* saturates value to a signed or unsigned number of bits, noting when it
   had to. */
static inline int64_t saturate_signed(int64_t value, int bits, int *saturated) {
    int64_t max = ((int64_t)1 << (bits - 1)) - 1, min = -((int64_t)1 << (bits - 1));
    if (value > max) { *saturated = 1; return max; }
    if (value < min) { *saturated = 1; return min; }
    return value;
}

static inline int64_t saturate_unsigned(int64_t value, int bits, int *saturated) {
    int64_t max = bits >= 32 ? 0xFFFFFFFFll : ((int64_t)1 << bits) - 1;
    if (value > max) { *saturated = 1; return max; }
    if (value < 0) { *saturated = 1; return 0; }
    return value;
}

/* ssat and usat, setting q when the value did not fit. */
static inline uint32_t media_saturate(Context *ctx, int64_t value, int bits, int is_unsigned) {
    int saturated = 0;
    int64_t result = is_unsigned ? saturate_unsigned(value, bits, &saturated) : saturate_signed(value, bits, &saturated);
    if (saturated) ctx->q = 1;
    return (uint32_t)result;
}

/* ssat16 and usat16. */
static inline uint32_t media_saturate16(Context *ctx, uint32_t value, int bits, int is_unsigned) {
    int saturated = 0;
    uint32_t out = 0;
    for (int i = 0; i < 2; i++) {
        int64_t lane = (int16_t)(value >> (16 * i));
        int64_t result = is_unsigned ? saturate_unsigned(lane, bits, &saturated) : saturate_signed(lane, bits, &saturated);
        out |= ((uint32_t)result & 0xFFFF) << (16 * i);
    }
    if (saturated) ctx->q = 1;
    return out;
}

/* one lane of a parallel add or subtract, 8 or 16 bits wide. */
static inline uint32_t media_lane(uint32_t a, uint32_t b, int width, int sub, int is_signed, int saturating,
                                  int halving, int *ge) {
    uint32_t mask = width == 16 ? 0xFFFFu : 0xFFu;
    int32_t wide;
    if (is_signed) {
        int32_t x = width == 16 ? (int16_t)a : (int8_t)a, y = width == 16 ? (int16_t)b : (int8_t)b;
        wide = sub ? x - y : x + y;
        *ge = wide >= 0;
    } else {
        int32_t x = a & mask, y = b & mask;
        wide = sub ? x - y : x + y;
        /* no borrow for a subtract, a carry out for an add */
        *ge = sub ? wide >= 0 : wide > (int32_t)mask;
    }
    int unused = 0;
    int64_t value = wide;
    if (halving) value = wide >> 1;
    else if (saturating) value = is_signed ? saturate_signed(wide, width, &unused) : saturate_unsigned(wide, width, &unused);
    return (uint32_t)value & mask;
}

/* sadd16, uqsub8, shasx and the rest. op1 picks signed or unsigned and
   plain, saturating or halving, op2 the lanes. only the plain forms set
   ge. */
static inline uint32_t media_parallel(Context *ctx, int op1, int op2, uint32_t a, uint32_t b) {
    int is_signed = !(op1 & 4), saturating = (op1 & 3) == 2, halving = (op1 & 3) == 3;
    uint32_t result = 0;
    int ge = 0, g;
    if (op2 <= 3) {
        /* asx and sax cross the halves of b */
        int cross = op2 == 1 || op2 == 2;
        uint32_t b_lo = cross ? b >> 16 : b & 0xFFFF, b_hi = cross ? b & 0xFFFF : b >> 16;
        result = media_lane(a & 0xFFFF, b_lo, 16, op2 == 1 || op2 == 3, is_signed, saturating, halving, &g);
        if (g) ge |= 3;
        result |= media_lane(a >> 16, b_hi, 16, op2 == 2 || op2 == 3, is_signed, saturating, halving, &g) << 16;
        if (g) ge |= 12;
    } else {
        for (int i = 0; i < 4; i++) {
            result |= media_lane(a >> (8 * i), b >> (8 * i), 8, op2 == 7, is_signed, saturating, halving, &g) << (8 * i);
            if (g) ge |= 1 << i;
        }
    }
    if (!saturating && !halving) ctx->ge = ge;
    return result;
}

/* sel, each byte from a where its ge bit is set and from b where not. */
static inline uint32_t media_select(Context *ctx, uint32_t a, uint32_t b) {
    uint32_t out = 0;
    for (int i = 0; i < 4; i++) out |= ((ctx->ge >> i) & 1 ? a : b) & (0xFFu << (8 * i));
    return out;
}

/* adds the halves of a and b apart, for the accumulating packed extends. */
static inline uint32_t add_halves(uint32_t a, uint32_t b) {
    return ((a + b) & 0xFFFF) | (((a >> 16) + (b >> 16)) << 16);
}

/* usad8, the sum of the absolute differences of the bytes. */
static inline uint32_t media_usad8(uint32_t a, uint32_t b) {
    uint32_t sum = 0;
    for (int i = 0; i < 4; i++) {
        int32_t x = (a >> (8 * i)) & 0xFF, y = (b >> (8 * i)) & 0xFF;
        sum += (uint32_t)(x > y ? x - y : y - x);
    }
    return sum;
}

static inline uint32_t reverse_bits(uint32_t value) {
    value = ((value >> 1) & 0x55555555u) | ((value & 0x55555555u) << 1);
    value = ((value >> 2) & 0x33333333u) | ((value & 0x33333333u) << 2);
    value = ((value >> 4) & 0x0F0F0F0Fu) | ((value & 0x0F0F0F0Fu) << 4);
    return __builtin_bswap32(value);
}

/* shifts by a register, the amount is its bottom byte. */
static inline uint32_t shift_lsl(uint32_t value, uint32_t amount, uint8_t *carry) {
    amount &= 0xFF;
    if (amount == 0) return value;
    if (amount < 32) { *carry = (value >> (32 - amount)) & 1; return value << amount; }
    *carry = amount == 32 ? value & 1 : 0;
    return 0;
}

static inline uint32_t shift_lsr(uint32_t value, uint32_t amount, uint8_t *carry) {
    amount &= 0xFF;
    if (amount == 0) return value;
    if (amount < 32) { *carry = (value >> (amount - 1)) & 1; return value >> amount; }
    *carry = amount == 32 ? value >> 31 : 0;
    return 0;
}

static inline uint32_t shift_asr(uint32_t value, uint32_t amount, uint8_t *carry) {
    amount &= 0xFF;
    if (amount == 0) return value;
    if (amount < 32) { *carry = ((int32_t)value >> (amount - 1)) & 1; return (uint32_t)((int32_t)value >> amount); }
    *carry = value >> 31;
    return (uint32_t)((int32_t)value >> 31);
}

static inline uint32_t shift_ror(uint32_t value, uint32_t amount, uint8_t *carry) {
    amount &= 0xFF;
    if (amount == 0) return value;
    amount &= 31;
    if (amount == 0) { *carry = value >> 31; return value; }
    *carry = (value >> (amount - 1)) & 1;
    return ror32(value, amount);
}

#define FPSCR_FZ (1u << 24)
#define FPSCR_DN (1u << 25)
/* the short vector length, zero when an instruction works on one register. */
#define FPSCR_LEN (7u << 16)

static inline float (vfp_s)(Context *ctx, int r) {
    union { uint32_t u; float f; } val;
    val.u = ctx->vfp[r];
    return val.f;
}

static inline double (vfp_d)(Context *ctx, int d) {
    union { uint64_t u; double d; } val;
    val.u = ctx->vfp[2 * d] | ((uint64_t)ctx->vfp[2 * d + 1] << 32);
    return val.d;
}

/* stores a result, flushing a subnormal to zero when the guest asked. */
static inline void (vfp_set_s)(Context *ctx, int r, float value) {
    union { float f; uint32_t u; } val;
    val.f = value;
    uint32_t bits = val.u;
    if ((*ctx->fpscr & FPSCR_FZ) && !(bits & 0x7F800000u) && (bits & 0x007FFFFFu)) bits &= 0x80000000u;
    if ((bits & 0x7FFFFFFFu) > 0x7F800000u && (*ctx->fpscr & FPSCR_DN)) bits = 0x7FC00000u;
    ctx->vfp[r] = bits;
}

static inline void (vfp_set_d)(Context *ctx, int d, double value) {
    union { double d; uint64_t u; } val;
    val.d = value;
    uint64_t bits = val.u;
    if ((*ctx->fpscr & FPSCR_FZ) && !(bits & 0x7FF0000000000000ull) && (bits & 0x000FFFFFFFFFFFFFull))
        bits &= 0x8000000000000000ull;
    if ((bits & 0x7FFFFFFFFFFFFFFFull) > 0x7FF0000000000000ull && (*ctx->fpscr & FPSCR_DN)) bits = 0x7FF8000000000000ull;
    ctx->vfp[2 * d] = (uint32_t)bits;
    ctx->vfp[2 * d + 1] = (uint32_t)(bits >> 32);
}

#define vfp_s(ctx, r) ({ \
    union { uint32_t u; float f; } __v; \
    __v.u = (ctx)->vfp[r]; \
    __v.f; \
})

#define vfp_set_s(ctx, r, val) do { \
    union { float f; uint32_t u; } __v; \
    __v.f = (val); \
    uint32_t __b = __v.u; \
    if ((*((ctx)->fpscr) & FPSCR_FZ) && !(__b & 0x7F800000u) && (__b & 0x007FFFFFu)) __b &= 0x80000000u; \
    if ((__b & 0x7FFFFFFFu) > 0x7F800000u && (*((ctx)->fpscr) & FPSCR_DN)) __b = 0x7FC00000u; \
    (ctx)->vfp[r] = __b; \
} while (0)

/* the comparison result in fpscr's top bits, the nzcv encoding. */
static inline void vfp_compare(Context *ctx, double a, double b) {
    uint32_t flags = (a != a || b != b) ? 0x3 : a == b ? 0x6 : a < b ? 0x8 : 0x2;
    *ctx->fpscr = (*ctx->fpscr & 0x0FFFFFFFu) | flags << 28;
}

/* conversions to integers, rounding toward zero and saturating. */
static inline uint32_t vfp_to_s32(double value) {
    if (value != value) return 0;
    if (value >= 2147483647.0) return 0x7FFFFFFFu;
    if (value <= -2147483648.0) return 0x80000000u;
    return (uint32_t)(int32_t)value;
}

static inline uint32_t vfp_to_u32(double value) {
    if (value != value) return 0;
    if (value >= 4294967295.0) return 0xFFFFFFFFu;
    if (value <= 0.0) return 0;
    return (uint32_t)value;
}

/* the arithmetic, numbered the way the code generator passes it, from the
   multiply-accumulates to vsqrt, whose operand is b. */
static inline float vfp_apply_s(int op, float a, float b, float acc) {
    switch (op) {
    case 0: return acc + a * b;
    case 1: return acc - a * b;
    case 2: return -acc + a * b;
    case 3: return -acc - a * b;
    case 4: return a * b;
    case 5: return -(a * b);
    case 6: return a + b;
    case 7: return a - b;
    case 8: return a / b;
    default: return sqrtf(b);
    }
}

static inline double vfp_apply_d(int op, double a, double b, double acc) {
    switch (op) {
    case 0: return acc + a * b;
    case 1: return acc - a * b;
    case 2: return -acc + a * b;
    case 3: return -acc - a * b;
    case 4: return a * b;
    case 5: return -(a * b);
    case 6: return a + b;
    case 7: return a - b;
    case 8: return a / b;
    case 9: return b;
    case 10: return fabs(b);
    case 11: return -b;
    default: return sqrt(b);
    }
}

/* where register reg is on step i of a short vector, going around its bank. */
static inline int vfp_step(int reg, int bank, int i, int stride) {
    return (reg & ~(bank - 1)) | ((reg + i * stride) & (bank - 1));
}

/* an operation on a short vector, which fpscr's len and stride shape. m
   stays put when it is in the first bank. */
static void vfp_vector(Context *ctx, int op, int wide, int d, int n, int m) {
    uint32_t fpscr = *ctx->fpscr;
    int length = ((fpscr >> 16) & 7) + 1;
    int stride = ((fpscr >> 20) & 3) == 3 ? 2 : 1;
    int bank = wide ? 4 : 8;
    for (int i = 0; i < length; i++) {
        int dd = vfp_step(d, bank, i, stride), nn = vfp_step(n, bank, i, stride);
        int mm = m < bank ? m : vfp_step(m, bank, i, stride);
        if (wide && op >= 9 && op <= 11) {
            /* the same for doubles, whose bits can be two singles that read
               as a subnormal double and would be flushed */
            uint32_t low = ctx->vfp[2 * (mm & 15)], high = ctx->vfp[2 * (mm & 15) + 1];
            ctx->vfp[2 * (dd & 15)] = low;
            ctx->vfp[2 * (dd & 15) + 1] = op == 9 ? high : op == 10 ? high & 0x7FFFFFFFu : high ^ 0x80000000u;
        } else if (wide) {
            vfp_set_d(ctx, dd & 15, vfp_apply_d(op, vfp_d(ctx, nn & 15), vfp_d(ctx, mm & 15), vfp_d(ctx, dd & 15)));
        } else if (op >= 9 && op <= 11) {
            /* vmov, vabs and vneg move the bits untouched */
            uint32_t bits = ctx->vfp[mm];
            ctx->vfp[dd] = op == 9 ? bits : op == 10 ? bits & 0x7FFFFFFFu : bits ^ 0x80000000u;
        } else {
            vfp_set_s(ctx, dd, vfp_apply_s(op, vfp_s(ctx, nn), vfp_s(ctx, mm), vfp_s(ctx, dd)));
        }
    }
}

#define RECOMP_DEPTH_LIMIT 2048

/* the flags as a function sees them. built with RECOMP_FLAG_LOCALS they are
   locals of the functions that use them, stored where code outside reads
   them and loaded back after a call, otherwise the context's own */
#ifdef RECOMP_FLAG_LOCALS
#define F_N f_n
#define F_Z f_z
#define F_C f_c
#define F_V f_v
#define RECOMP_FLAGS_DECL uint8_t f_n = ctx->n, f_z = ctx->z, f_c = ctx->c, f_v = ctx->v;
#define RECOMP_FLAGS_OUT() do { ctx->n = f_n; ctx->z = f_z; ctx->c = f_c; ctx->v = f_v; } while (0)
#define RECOMP_FLAGS_IN() do { f_n = ctx->n; f_z = ctx->z; f_c = ctx->c; f_v = ctx->v; } while (0)
#else
#define F_N (ctx->n)
#define F_Z (ctx->z)
#define F_C (ctx->c)
#define F_V (ctx->v)
#define RECOMP_FLAGS_DECL
#define RECOMP_FLAGS_OUT() do {} while (0)
#define RECOMP_FLAGS_IN() do {} while (0)
#endif
#define RECOMP_SYNC_OUT() do {} while (0)
#define RECOMP_SYNC_IN() do {} while (0)

#if defined(RECOMP_PINNED) && !defined(RECOMP_TABLES)
/* what a function gives back when it did not return: ctx->exit says why
   and r15 where to go on. */
#define RECOMP_EXITED 0xFFFFFFFFu

#define RECOMP_HEAD
#define RECOMP_ENTRY(entry, label, state) \
    uint32_t target; \
    if (LIKELY(!entered_)) goto label; \
    target = ctx->r[15];
/* a function that calls others looks at the host's stack first: guest calls
   are host calls, and a guest that calls without returning, or deeper than
   the stack has room for, unwinds to the host and starts over from r15. */
#define RECOMP_ENTRY_CALLS(entry, label, state) \
    uint32_t target; \
    if (UNLIKELY(recomp_sp < ctx->stack_floor)) { \
        if (!entered_) ctx->r[15] = (entry); \
        ctx->exit = EXIT_UNWIND; \
        return RECOMP_EXITED; \
    } \
    if (LIKELY(!entered_)) goto label; \
    target = ctx->r[15];

/* runs whatever code there is for r15: the host's table of what it found
   lately says without a call, the host itself otherwise. */
static inline uint32_t recomp_call(void) {
    uint32_t key = ctx->r[15] | ctx->thumb;
    const Found *slot = &ctx->found[(key >> 1) & (RECOMP_FOUND - 1)];
    Code code;
    if (UNLIKELY(recomp_sp < ctx->stack_floor)) {
        ctx->exit = EXIT_UNWIND;
        return RECOMP_EXITED;
    }
    if (LIKELY(slot->address == key)) {
        code = slot->code;
    } else {
        RECOMP_SPILL_VOLATILE();
        code = ctx->host->lookup(ctx, key);
        RECOMP_FILL_VOLATILE();
    }
    if (LIKELY(code)) return code(1);
    ctx->exit = EXIT_UNWIND;
    return RECOMP_EXITED;
}

/* after a call: back at lr is the plain return. Anything else is the code
   having left, or a return to somewhere else, which goes through dispatch. */
#define RECOMP_CAME_BACK(ret, lr) \
    if (UNLIKELY((ret) != (lr))) { \
        if (ctx->exit) return (ret); \
        ctx->thumb = (ret) & 1; \
        target = (ret) & ((ret) & 1 ? ~1u : ~3u); \
        goto dispatch; \
    }

#define CALL_TO(name, address, lr) do { uint32_t ret_ = name(0); RECOMP_CAME_BACK(ret_, (lr)) } while (0)
#define CALL_ELSEWHERE(lr) do { uint32_t ret_ = recomp_call(); RECOMP_CAME_BACK(ret_, (lr)) } while (0)
#define CALL(code) do { uint32_t ret_ = code(1); if (UNLIKELY(ctx->exit)) return ret_; } while (0)

#define TAIL_TO(name, address) return name(0)
#define TAIL_ELSEWHERE(address) do { ctx->r[15] = (address); return recomp_call(); } while (0)

/* a return that may switch instruction sets, RECOMP_THUMB the function's
   own, and the two that cannot. */
#define RETURN_TO(value) do { \
    uint32_t v_ = (value); \
    if (UNLIKELY((v_ & 1) != RECOMP_THUMB)) ctx->thumb = v_ & 1; \
    return v_; \
} while (0)
#define RETURN_ARM(value) return (value) & ~3u
#define RETURN_THUMB(value) return (value) | 1u

/* a jump that may switch to Thumb. */
#define JUMP_TO(value) do { \
    uint32_t v_ = (value); \
    ctx->thumb = v_ & 1; \
    target = v_ & (ctx->thumb ? ~1u : ~3u); \
    goto dispatch; \
} while (0)

/* a block runs whole while there is any budget, which it may then take
   below nothing: the host counts what ran from what is left. */
#define BUDGET(address, count) \
    if (UNLIKELY(recomp_budget < 0)) { \
        ctx->r[15] = (address); \
        ctx->exit = EXIT_BUDGET; \
        return RECOMP_EXITED; \
    } \
    recomp_budget -= (count);

#define SVC(next, number) do { \
    ctx->r[15] = (next); \
    ctx->svc = (number); \
    ctx->exit = EXIT_SVC; \
    return RECOMP_EXITED; \
} while (0)

#define INTERPRET(address, opcode) do { \
    RECOMP_SPILL(); \
    ctx->host->interpret(ctx, (address), (opcode)); \
    RECOMP_FILL(); \
    if (UNLIKELY(ctx->exit)) return RECOMP_EXITED; \
} while (0)

#else
/* a guest call, leaving the caller too when the host has to take over. */
#define CALL(code) do { \
    RECOMP_SYNC_OUT(); \
    if (UNLIKELY(++ctx->depth > RECOMP_DEPTH_LIMIT)) { ctx->depth--; ctx->exit = EXIT_UNWIND; return; } \
    code(ctx); \
    ctx->depth--; \
    if (UNLIKELY(ctx->exit)) return; \
    RECOMP_SYNC_IN(); \
} while (0)

/* runs whatever code the host has for r15. */
static inline void recomp_call(Context *ctx) {
    Code code = ctx->host->lookup(ctx, ctx->r[15] | ctx->thumb);
    if (code) code(ctx);
    else ctx->exit = EXIT_UNWIND;
}

/* how a function starts: at its entry when that is where r15 is, or at
   the label r15 names. */
#define RECOMP_HEAD uint8_t *const view = ctx->fastmem;
#define RECOMP_ENTRY(entry, label, state) \
    uint32_t target = ctx->r[15]; \
    if (LIKELY(target == (entry) && (state))) goto label;
/* the same for a function that calls others */
#define RECOMP_ENTRY_CALLS(entry, label, state) RECOMP_ENTRY(entry, label, state)

/* after a call, anything but a plain return to the next instruction, lr
   with bit 0 set for Thumb, goes through dispatch. */
#define RECOMP_RETURNED(lr) \
    if (UNLIKELY(ctx->r[15] != ((lr) & ~1u) || ctx->thumb != ((lr) & 1u))) { target = ctx->r[15]; goto dispatch; }

/* a call to a function of the code's, which starts at address, and one to
   wherever r15 says, both coming back to lr. */
#define CALL_TO(name, address, lr) do { ctx->r[15] = (address); CALL(name); RECOMP_RETURNED(lr) } while (0)
#define CALL_ELSEWHERE(lr) do { CALL(recomp_call); RECOMP_RETURNED(lr) } while (0)

/* a jump to another function, and one to an address this function has no
   label for, which the host may know. */
#define TAIL_TO(name, address) do { ctx->r[15] = (address); CALL(name); RECOMP_SYNC_OUT(); return; } while (0)
#define TAIL_ELSEWHERE(address) do { ctx->r[15] = (address); CALL(recomp_call); RECOMP_SYNC_OUT(); return; } while (0)

/* a return that stays in ARM, and one that stays in Thumb. */
#define RETURN_ARM(value) do { ctx->r[15] = (value) & ~3u; RECOMP_SYNC_OUT(); return; } while (0)
#define RETURN_THUMB(value) do { ctx->r[15] = (value) & ~1u; RECOMP_SYNC_OUT(); return; } while (0)

/* a return that may switch to Thumb. */
#define RETURN_TO(value) do { \
    uint32_t v_ = (value); \
    RECOMP_SYNC_OUT(); \
    ctx->thumb = v_ & 1; \
    ctx->r[15] = v_ & (ctx->thumb ? ~1u : ~3u); \
    return; \
} while (0)

/* a jump that may switch to Thumb. */
#define JUMP_TO(value) do { \
    uint32_t v_ = (value); \
    ctx->thumb = v_ & 1; \
    target = v_ & (ctx->thumb ? ~1u : ~3u); \
    goto dispatch; \
} while (0)

#define BUDGET(address, count) \
    if (UNLIKELY((ctx->budget -= (count)) < 0)) { \
        ctx->budget += (count); \
        RECOMP_SYNC_OUT(); \
        ctx->r[15] = (address); \
        ctx->exit = EXIT_BUDGET; \
        return; \
    }

#define SVC(next, number) do { \
    RECOMP_SYNC_OUT(); \
    ctx->r[15] = (next); \
    ctx->svc = (number); \
    ctx->exit = EXIT_SVC; \
    return; \
} while (0)

#define INTERPRET(address, opcode) do { \
    RECOMP_SYNC_OUT(); \
    ctx->host->interpret(ctx, (address), (opcode)); \
    if (UNLIKELY(ctx->exit)) return; \
    RECOMP_SYNC_IN(); \
} while (0)

#endif

/* a function written by hand to run instead of the one at address, odd for
   Thumb, or at an offset in a module. see docs/overrides.md. */
#define RECOMP_OVERRIDE(address) RECOMP_FN(override_##address)
#define RECOMP_OVERRIDE_IN(module, offset) RECOMP_FN(override_##module##_##offset)

/* the generated function an override replaces, to run with CALL. */
#define RECOMP_ORIGINAL(address) original_##address
#define RECOMP_ORIGINAL_IN(module, offset) original_##module##_##offset

#endif
