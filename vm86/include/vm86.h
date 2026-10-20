/*
 * vm86 - 8086 CPU core (subset) for in-game virtual machines.
 *
 * Design:
 *  - No threads, no internal clock, no dynamic memory except vm86_create().
 *  - The engine decides how many instructions run per frame (vm86_run).
 *  - Everything that is implemented behaves like a real 8086.
 *    Anything not implemented stops the CPU with VM86_ERR_UNSUPPORTED;
 *    it never behaves "differently".
 *  - The complete state can be saved and restored as a block of bytes.
 *
 * Plain C interface so it can be linked from Unreal, Unity (P/Invoke),
 * Godot (GDExtension) or WebAssembly.
 */
#ifndef VM86_H
#define VM86_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) && defined(VM86_BUILD_DLL)
#  define VM86_API __declspec(dllexport)
#elif defined(_WIN32) && defined(VM86_USE_DLL)
#  define VM86_API __declspec(dllimport)
#else
#  define VM86_API
#endif

#define VM86_MEM_SIZE (1u << 20) /* 1 MB, like the real 8086 */

typedef struct vm86 vm86_t;

/* Visible registers. */
typedef struct vm86_regs {
    uint16_t ax, bx, cx, dx;
    uint16_t si, di, bp, sp;
    uint16_t cs, ds, es, ss;
    uint16_t ip, flags;
} vm86_regs_t;

/* FLAGS bits */
enum {
    VM86_CF = 1u << 0,  VM86_PF = 1u << 2,  VM86_AF = 1u << 4,
    VM86_ZF = 1u << 6,  VM86_SF = 1u << 7,  VM86_TF = 1u << 8,
    VM86_IF = 1u << 9,  VM86_DF = 1u << 10, VM86_OF = 1u << 11
};

typedef enum vm86_status {
    VM86_RUNNING = 0,        /* can keep executing */
    VM86_HALTED,             /* executed HLT; woken up by vm86_request_interrupt */
    VM86_STOPPED,            /* the host called vm86_stop() */
    VM86_ERR_UNSUPPORTED     /* unsupported opcode; see vm86_error() */
} vm86_status_t;

/*
 * Host callbacks (the "motherboard" of your machine). Any of them may be NULL.
 *
 *  in8/out8:  I/O ports (video, keyboard, speaker...). 16-bit accesses are
 *             performed as two 8-bit accesses (port, port+1).
 *  interrupt: called on every INT n (and on the divide error, INT 0).
 *             Return 1 if the host handled the interrupt (e.g. BIOS/DOS
 *             services implemented in C): the CPU continues with the next
 *             instruction. Return 0 for the real behavior: the CPU jumps
 *             through the interrupt vector table at 0000:0000.
 */
typedef struct vm86_host {
    void *user;
    uint8_t (*in8)(void *user, uint16_t port);
    void    (*out8)(void *user, uint16_t port, uint8_t value);
    int     (*interrupt)(void *user, vm86_t *vm, uint8_t number);
} vm86_host_t;

typedef struct vm86_error_info {
    uint16_t cs, ip;         /* address of the instruction */
    uint8_t  opcode;         /* first byte after any prefixes */
    uint8_t  modrm_reg;      /* /r sub-opcode for groups, 0xFF if not applicable */
} vm86_error_info_t;

VM86_API vm86_t      *vm86_create(void);
VM86_API void         vm86_destroy(vm86_t *vm);
VM86_API void         vm86_reset(vm86_t *vm);           /* CPU to FFFF:0000, memory untouched */
VM86_API void         vm86_set_host(vm86_t *vm, const vm86_host_t *host);

VM86_API uint8_t     *vm86_memory(vm86_t *vm);          /* 1 MB, direct read/write access */
VM86_API vm86_regs_t *vm86_regs(vm86_t *vm);            /* direct read/write access */

/*
 * Loads a .COM-style program into the given segment, the way DOS does:
 * minimal PSP at seg:0000 containing INT 20h, program at seg:0100,
 * CS=DS=ES=SS=seg, IP=0100h, SP=FFFEh and a zero word on the stack
 * (so a final RET terminates the program).
 * Returns 0 on success, -1 if the program does not fit (max 65280 bytes).
 */
VM86_API int vm86_load_com(vm86_t *vm, const uint8_t *data, size_t len, uint16_t seg);

/*
 * Runs up to max_steps steps. A step is one instruction, or one iteration of
 * a REP-prefixed string instruction (so a 64000-byte REP STOSB does not freeze
 * the frame: it resumes on the next call, just like real hardware does when an
 * interrupt arrives). Returns the number of steps executed.
 * For reference, a 4.77 MHz 8088 ran roughly 0.3-0.5 million instructions per
 * second (~5,000-8,000 per frame at 60 Hz).
 */
VM86_API uint32_t      vm86_run(vm86_t *vm, uint32_t max_steps);
VM86_API vm86_status_t vm86_status(const vm86_t *vm);
VM86_API void          vm86_stop(vm86_t *vm);           /* from a callback, e.g. INT 20h */
VM86_API void          vm86_resume(vm86_t *vm);         /* status -> RUNNING */
VM86_API vm86_error_info_t vm86_error(const vm86_t *vm);

/*
 * Hardware interrupt (keyboard, timer...). Serviced between instructions
 * when IF=1. Also wakes the CPU from HLT.
 * There is a 16-entry queue; returns 0 if queued, -1 if the queue was full.
 */
VM86_API int vm86_request_interrupt(vm86_t *vm, uint8_t number);

/* Save/load the complete state (CPU + 1 MB of memory). The block is the raw
 * internal structure: valid for the same build on the same platform. */
VM86_API size_t vm86_state_size(void);
VM86_API size_t vm86_save_state(const vm86_t *vm, void *buf, size_t len);
VM86_API int    vm86_load_state(vm86_t *vm, const void *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif
