/*
 * asm86 - assembler for a subset of NASM, targeting the vm86 8086.
 *
 * Goal: player code is written exactly as in real NASM and, for everything
 * supported, the output is byte-for-byte identical to "nasm -f bin".
 * Anything unsupported produces a clear error.
 *
 * Supported syntax:
 *   labels (with or without ':'), local labels (.loop), ';' comments
 *   numbers: 123, 0x1F, 1Fh, $1F, 0b101, 101b, 17q, 0o17, 'A', 'AB'
 *   expressions: + - * / // % %% << >> & | ^ ~ ! ( )  $  $$
 *   directives: org, db, dw, resb, resw, times, equ, bits 16, cpu 8086
 *   operands: registers, immediates, [bx+si+disp], [es:di], byte/word,
 *             short/near on jumps
 *   prefixes: rep, repe, repz, repne, repnz
 *
 * Source text is treated as bytes, like NASM: labels and strings may contain
 * any byte >= 80h (code page 437, UTF-8, ...).
 */
#ifndef ASM86_H
#define ASM86_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef VM86_API
#  if defined(_WIN32) && defined(VM86_BUILD_DLL)
#    define VM86_API __declspec(dllexport)
#  elif defined(_WIN32) && defined(VM86_USE_DLL)
#    define VM86_API __declspec(dllimport)
#  else
#    define VM86_API
#  endif
#endif

#define ASM86_MAX_ERRORS 32

typedef struct asm86_error {
    int line;               /* 1 = first line */
    char message[256];      /* always valid UTF-8 */
} asm86_error_t;

typedef struct asm86_symbol {
    char name[64];
    int32_t value;
    int is_label;           /* 1 = address, 0 = constant (equ) */
} asm86_symbol_t;

/* Line -> address mapping, for debuggers (highlighting the current line). */
typedef struct asm86_line {
    int line;
    uint16_t address;
    uint16_t size;
} asm86_line_t;

typedef struct asm86_result {
    uint8_t *code;          /* flat binary (.COM format when org 100h) */
    size_t size;
    uint16_t origin;        /* value of org */

    int error_count;        /* 0 = success */
    asm86_error_t errors[ASM86_MAX_ERRORS];

    asm86_symbol_t *symbols;
    int symbol_count;

    asm86_line_t *lines;
    int line_count;
} asm86_result_t;

/* Assembles the NUL-terminated source text. Returns 0 if there were no errors.
 * Always call asm86_free() afterwards, even when there were errors.
 * Errors are reported all at once, sorted by line. */
VM86_API int  asm86_assemble(const char *source, asm86_result_t *result);
VM86_API void asm86_free(asm86_result_t *result);

#ifdef __cplusplus
}
#endif
#endif
