/* PersonaRM recompiled-module ABI (C). Generated code (tools/recomp_gen, personarm_native) includes
 * this header and nothing else, so it compiles with MSVC, GCC/Clang or TinyCC, linked into the binary
 * or built as a DLL/.so that personarm loads at run time (recomp/runtime/module_loader.h).
 *
 * prm_cpu and prm_module mirror personarm::recomp::CpuState and Module field for field; the C++ side
 * static_asserts the layout (recomp_runtime.cpp). Bump PRM_ABI_VERSION on any change here: a module
 * built for another version is refused and every on-device cache is rebuilt.
 *
 * A module library exports one function, `prm_modules` (prm_modules_fn): given the host's
 * PRM_ABI_VERSION it returns its modules, or null and *count = 0 for another version. */
#ifndef PRM_ABI_H
#define PRM_ABI_H

#define PRM_ABI_VERSION 1u

#if defined(_MSC_VER)
#include <stddef.h>
typedef size_t prm_size;
#else
typedef __SIZE_TYPE__ prm_size;
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned char prm_u8;
typedef unsigned int prm_u32;
typedef unsigned long long prm_u64;

typedef struct prm_cpu prm_cpu;
typedef void (*prm_fn)(prm_cpu* s);

typedef struct prm_entry { prm_u32 address; prm_fn fn; } prm_entry;

/* Callbacks returning C++ bool are declared as returning prm_u8 (the bool lives in the low byte). */
struct prm_cpu {
    prm_u32 gpr[32];
    prm_u32 hi, lo, pc;
    prm_u32 sr, cause, epc, badvaddr;
    void* gte;                                   /* host only: generated code goes through cop2 */
    void* mem;
    prm_u32 (*read)(void* mem, prm_u32 address, prm_u32 size);
    void (*write)(void* mem, prm_u32 address, prm_u32 value, prm_u32 size);
    const void* dispatcher;                      /* host only */
    const prm_fn* map;
    prm_u32 map_lo, map_words;
    prm_u64 fallbacks;
    prm_u8* ram;
    prm_u8* scratch;
    const prm_u32* watch;
    void (*code_write)(prm_cpu* s, prm_u32 ram_offset);
    prm_u32 ram_stall;
    prm_u64 stalls;
    prm_u64 cycles, deadline;
    prm_u8 (*service)(prm_cpu* s);
    prm_u8 (*external)(prm_cpu* s);
    void* host;
    prm_u8 stopped;
    prm_u32 next_pc;
    prm_u32 load_reg, load_val;
    prm_u32 pair_prev;
    /* COP2 (GTE): `word` is a COP2 instruction (command, MFC2/CFC2 return the register,
     * MTC2/CTC2 write `value`); LWC2/SWC2 come as MTC2/MFC2 of rt. */
    prm_u32 (*cop2)(prm_cpu* s, prm_u32 word, prm_u32 value);
    /* Dispatch miss in the flat map: the host's Dispatch (dispatcher, external, fallback count). */
    void (*dispatch)(prm_cpu* s, prm_u32 target);
};

typedef struct prm_module {
    const char* name;
    prm_u32 base, size;
    prm_u64 hash;
    const prm_entry* entries;
    prm_size count;
    const prm_entry* resume;
    prm_size resume_count;
    const prm_u32* code;
    prm_size code_count;
    const prm_u32* words;
    prm_u64 code_hash;
} prm_module;

typedef const prm_module* const* (*prm_modules_fn)(prm_u32 abi, prm_size* count);

#ifdef __cplusplus
}
#endif

#ifndef __cplusplus
/* ── Helpers for generated code: same semantics as recomp_runtime.h ───────────────────────── */
typedef prm_u8 u8; typedef signed char s8; typedef unsigned short u16; typedef short s16;
typedef prm_u32 u32; typedef int s32; typedef prm_u64 u64; typedef long long s64;

#if defined(_MSC_VER)
#define PRM_INLINE static __inline
#else
#define PRM_INLINE static inline
#endif
#if defined(_WIN32)
#define PRM_EXPORT __declspec(dllexport)
#else
#define PRM_EXPORT __attribute__((visibility("default")))
#endif

PRM_INLINE u32 LoadLe(const u8* p, u32 size) {
    u32 v = p[0];
    if (size >= 2) v |= (u32)p[1] << 8;
    if (size == 4) v |= ((u32)p[2] << 16) | ((u32)p[3] << 24);
    return v;
}
PRM_INLINE void StoreLe(u8* p, u32 v, u32 size) {
    p[0] = (u8)v;
    if (size >= 2) p[1] = (u8)(v >> 8);
    if (size == 4) { p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
}
PRM_INLINE u32 Read(prm_cpu* s, u32 address, u32 size) {
    const u32 p = address & 0x1FFFFFFFu;
    if (s->ram && p < 0x00800000u) {
        const u32 o = p & 0x1FFFFFu;
        s->cycles += s->ram_stall;
        s->stalls += s->ram_stall;
        if (o + size <= 0x200000u) return LoadLe(s->ram + o, size);
        {
            u32 v = 0, i;
            for (i = 0; i < size; ++i) v |= (u32)s->ram[(o + i) & 0x1FFFFFu] << (8 * i);
            return v;
        }
    }
    if (s->scratch && p - 0x1F800000u <= 0x400u - size) return LoadLe(s->scratch + (p & 0x3FFu), size);
    return s->read(s->mem, address, size);
}
PRM_INLINE void Write(prm_cpu* s, u32 address, u32 value, u32 size) {
    const u32 p = address & 0x1FFFFFFFu;
    if (s->ram && p < 0x00800000u) {
        const u32 o = p & 0x1FFFFFu;
        u32 i;
        if (o + size <= 0x200000u) {
            StoreLe(s->ram + o, value, size);
            if (s->watch && ((s->watch[o >> 7] >> ((o >> 2) & 31u)) & 1u)) s->code_write(s, o);
            return;
        }
        for (i = 0; i < size; ++i) {
            const u32 b = (o + i) & 0x1FFFFFu;
            s->ram[b] = (u8)(value >> (8 * i));
            if (s->watch && ((s->watch[b >> 7] >> ((b >> 2) & 31u)) & 1u)) s->code_write(s, b);
        }
        return;
    }
    if (s->scratch && p - 0x1F800000u <= 0x400u - size) { StoreLe(s->scratch + (p & 0x3FFu), value, size); return; }
    s->write(s->mem, address, value, size);
}
PRM_INLINE int Stop(prm_cpu* s, u32 pc, u32 next) {
    if (!s->service || !s->service(s)) return 0;
    s->pc = pc;
    s->next_pc = next;
    s->stopped = 1;
    return 1;
}
PRM_INLINE int StopLoad(prm_cpu* s, u32 pc, u32 reg, u32 prev) {
    if (!Stop(s, pc, 0)) return 0;
    s->load_reg = reg;
    s->load_val = s->gpr[reg];
    s->gpr[reg] = prev;
    return 1;
}
PRM_INLINE void RaiseException(prm_cpu* s, u32 exc_code, u32 pc, int delay_slot) {
    s->epc = delay_slot ? pc - 4 : pc;
    s->cause = (s->cause & ~0x8000007Cu) | (exc_code << 2) | (delay_slot ? 0x80000000u : 0);
    s->sr = (s->sr & ~0x3Fu) | ((s->sr << 2) & 0x3Fu);
    s->pc = (s->sr & 0x00400000u) ? 0xBFC00180u : 0x80000080u;
}
PRM_INLINE void Dispatch(prm_cpu* s, u32 target) {
    s->pc = target;
    if (s->map) {
        const u32 i = (target - s->map_lo) >> 2;
        if (!(target & 3u) && i < s->map_words && s->map[i]) { s->map[i](s); return; }
    }
    s->dispatch(s, target);
}
PRM_INLINE void Div(prm_cpu* s, u32 a, u32 b) {
    if (b == 0) { s->lo = ((s32)a < 0) ? 1u : 0xFFFFFFFFu; s->hi = a; }
    else if (a == 0x80000000u && b == 0xFFFFFFFFu) { s->lo = 0x80000000u; s->hi = 0; }
    else { s->lo = (u32)((s32)a / (s32)b); s->hi = (u32)((s32)a % (s32)b); }
}
PRM_INLINE void Divu(prm_cpu* s, u32 a, u32 b) {
    if (b == 0) { s->lo = 0xFFFFFFFFu; s->hi = a; }
    else { s->lo = a / b; s->hi = a % b; }
}
#endif /* !__cplusplus */

#endif /* PRM_ABI_H */
