/* Host-side test (Linux/WSL x86-64, gcc) of the 0.5 {multiplier} call stub: the same asm stub and decision code as
   src/sx/multiplier.c, driven by a fake "parser" that sets up the registers and stack frame the engine's block parser
   has at its unknown-block error call (game/server layout: this = r13, reader = r14, word = [rbp-0x59]; editor layout:
   this = r15, reader = rsi, word = [rsp+0x20]). Windows x64 calling convention is used throughout (ms_abi).
     gcc -O2 -o mult_stub_test tests/mult_stub_test.c && ./mult_stub_test
   Checks: "multiplier" is consumed (number stored, logger NOT called, callee-saved registers intact); any other word
   reaches the logger with the original 4 arguments; a wrong class or a missing number also falls back to the logger. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

#define MS __attribute__((ms_abi))
typedef struct { union { char buf[16]; char *ptr; } u; size_t size, cap; } MsString;
typedef struct { uint64_t rcx, rdx, r8, r9, rbx, rbp, rsi, rdi, r12, r13, r14, r15; } SxRegs;
#define MULT_MAGIC 0x4D550000u

static int g_multThisReg, g_multReaderReg, g_multFrameReg; static int32_t g_multFrameDisp;
static uint32_t g_multPadOff = 0x5C;
static void *g_vtPierce = (void *)0x1111, *g_vtBlast = (void *)0x2222;
static int peek_result = 1; static float float_result = 2.5f; static int peek_calls, float_calls;
static MS uint8_t fake_peek(void *rd, int kind, int z) { peek_calls++; return kind == 9 && rd == (void *)0xBEEF ? (uint8_t)peek_result : 0; }
static MS float fake_float(void *rd) { float_calls++; return rd == (void *)0xBEEF ? float_result : -1.0f; }
static MS uint8_t (*g_multPeek)(void *, int, int) = fake_peek;
static MS float (*g_multReadFloat)(void *) = fake_float;

static uint64_t log_args[4]; static int log_calls;
static MS int fake_logger(uint64_t a, uint64_t b, uint64_t c, uint64_t d) { log_args[0] = a; log_args[1] = b; log_args[2] = c; log_args[3] = d; log_calls++; return 77; }
__attribute__((used)) void *sx_mult_log_orig = (void *)fake_logger;

static uint64_t sx_reg(const SxRegs *r, int n)
{
    switch (n) {
    case 3: return r->rbx;  case 5: return r->rbp;  case 6: return r->rsi;  case 7: return r->rdi;
    case 12: return r->r12; case 13: return r->r13; case 14: return r->r14; case 15: return r->r15;
    }
    return 0;
}
/* identical logic to src/sx/multiplier.c (logging removed) */
__attribute__((used)) MS int sx_mult_unknown_block(SxRegs *r)
{
    const uint8_t *frame = g_multFrameReg == 4 ? (const uint8_t *)r + sizeof *r + 8 : (const uint8_t *)(uintptr_t)r->rbp;
    const MsString *w = (const MsString *)(frame + g_multFrameDisp);
    if (w->size != 10 || w->cap < 10) return 0;
    if (strncasecmp(w->cap > 15 ? w->u.ptr : w->u.buf, "multiplier", 10)) return 0;
    uint8_t *a = (uint8_t *)(uintptr_t)sx_reg(r, g_multThisReg);
    void *rd = (void *)(uintptr_t)sx_reg(r, g_multReaderReg);
    void **vt = a ? *(void ***)a : NULL;
    if ((void *)vt != g_vtPierce && (void *)vt != g_vtBlast) return 0;
    if (!g_multPeek(rd, 9, 0)) return 0;
    float v = g_multReadFloat(rd), c = v;
    if (!(c >= 0.0f)) c = c != c ? 1.0f : 0.0f;
    if (c > 65.535f) c = 65.535f;
    uint32_t q = (uint32_t)(c * 1000.0f + 0.5f);
    *(uint32_t *)(a + g_multPadOff) = MULT_MAGIC | q;
    return 1;
}
/* the stub, byte for byte the same asm */
__attribute__((naked)) void mult_stub(void)
{
    __asm__ volatile(
        "push %r15\n\t" "push %r14\n\t" "push %r13\n\t" "push %r12\n\t"
        "push %rdi\n\t" "push %rsi\n\t" "push %rbp\n\t" "push %rbx\n\t"
        "push %r9\n\t"  "push %r8\n\t"  "push %rdx\n\t" "push %rcx\n\t"
        "mov %rsp, %rcx\n\t"
        "sub $40, %rsp\n\t"
        "call sx_mult_unknown_block\n\t"
        "add $40, %rsp\n\t"
        "pop %rcx\n\t" "pop %rdx\n\t" "pop %r8\n\t"  "pop %r9\n\t"
        "pop %rbx\n\t" "pop %rbp\n\t" "pop %rsi\n\t" "pop %rdi\n\t"
        "pop %r12\n\t" "pop %r13\n\t" "pop %r14\n\t" "pop %r15\n\t"
        "test %eax, %eax\n\t"
        "jnz 1f\n\t"
        "jmp *sx_mult_log_orig(%rip)\n"
        "1:\n\t"
        "ret\n\t");
}

/* fake parsers (SysV-callable, pure asm): build the engine parser's frame, load this/reader into the registers the
   engine uses, call the stub with the logger's 4 arguments (rcx=0xA1 edx=0x1F4 r8=0xA3 r9d=0x0BAD0001), then store
   rax and the callee-saved registers into out[]: {rax, rbx, r12, r13, r14, r15, rbp_ok, rsi, rdi}
   game/server: lea rbp,[rsp+0x79] (= rsp-0x47 before sub 0xc0), word at [rbp-0x59], this r13, reader r14
   editor:      word at [rsp+0x20], this r15, reader rsi */
void fake_game(const MsString *word, void *self, uint64_t *out);
void fake_editor(const MsString *word, void *self, uint64_t *out);
__asm__(
    ".text\n"
    "fake_game:\n\t"
    "push %rbp\n\t" "push %rbx\n\t" "push %r12\n\t" "push %r13\n\t" "push %r14\n\t" "push %r15\n\t"
    "sub $0xc8, %rsp\n\t"
    "mov %rdx, 0xc0(%rsp)\n\t"
    "lea 0x79(%rsp), %rbp\n\t"
    "movdqu (%rdi), %xmm0\n\t" "movdqu %xmm0, -0x59(%rbp)\n\t" "movdqu 16(%rdi), %xmm0\n\t" "movdqu %xmm0, -0x49(%rbp)\n\t"
    "mov %rsi, %r13\n\t" "mov $0xBEEF, %r14\n\t"
    "mov $0x1234, %rbx\n\t" "mov $0x5678, %r12\n\t" "mov $0x9ABC, %r15\n\t" "mov $0xDEF0, %rsi\n\t" "mov $0x4242, %rdi\n\t"
    "mov $0xA1, %rcx\n\t" "mov $0x1F4, %edx\n\t" "mov $0xA3, %r8\n\t" "mov $0x0BAD0001, %r9d\n\t"
    "call mult_stub\n\t"
    "mov 0xc0(%rsp), %rdx\n\t"
    "mov %rax, 0(%rdx)\n\t" "mov %rbx, 8(%rdx)\n\t" "mov %r12, 16(%rdx)\n\t" "mov %r13, 24(%rdx)\n\t" "mov %r14, 32(%rdx)\n\t" "mov %r15, 40(%rdx)\n\t"
    "lea 0x79(%rsp), %rcx\n\t" "xor %eax, %eax\n\t" "cmp %rcx, %rbp\n\t" "sete %al\n\t" "mov %rax, 48(%rdx)\n\t"
    "mov %rsi, 56(%rdx)\n\t" "mov %rdi, 64(%rdx)\n\t"
    "add $0xc8, %rsp\n\t"
    "pop %r15\n\t" "pop %r14\n\t" "pop %r13\n\t" "pop %r12\n\t" "pop %rbx\n\t" "pop %rbp\n\t"
    "ret\n"
    "fake_editor:\n\t"
    "push %rbp\n\t" "push %rbx\n\t" "push %r12\n\t" "push %r13\n\t" "push %r14\n\t" "push %r15\n\t"
    "sub $0xc8, %rsp\n\t"
    "mov %rdx, 0xc0(%rsp)\n\t"
    "movdqu (%rdi), %xmm0\n\t" "movdqu %xmm0, 0x20(%rsp)\n\t" "movdqu 16(%rdi), %xmm0\n\t" "movdqu %xmm0, 0x30(%rsp)\n\t"
    "mov %rsi, %r15\n\t" "mov $0xBEEF, %rsi\n\t"
    "mov $0x1234, %rbx\n\t" "mov $0x5678, %r12\n\t" "mov $0x7777, %r13\n\t" "mov $0x8888, %r14\n\t" "mov $0x3333, %rbp\n\t" "mov $0x4242, %rdi\n\t"
    "mov $0xA1, %rcx\n\t" "mov $0x1F4, %edx\n\t" "mov $0xA3, %r8\n\t" "mov $0x0BAD0001, %r9d\n\t"
    "call mult_stub\n\t"
    "mov 0xc0(%rsp), %rdx\n\t"
    "mov %rax, 0(%rdx)\n\t" "mov %rbx, 8(%rdx)\n\t" "mov %r12, 16(%rdx)\n\t" "mov %r13, 24(%rdx)\n\t" "mov %r14, 32(%rdx)\n\t" "mov %r15, 40(%rdx)\n\t"
    "xor %eax, %eax\n\t" "cmp $0x3333, %rbp\n\t" "sete %al\n\t" "mov %rax, 48(%rdx)\n\t"
    "mov %rsi, 56(%rdx)\n\t" "mov %rdi, 64(%rdx)\n\t"
    "add $0xc8, %rsp\n\t"
    "pop %r15\n\t" "pop %r14\n\t" "pop %r13\n\t" "pop %r12\n\t" "pop %rbx\n\t" "pop %rbp\n\t"
    "ret\n");

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL: " __VA_ARGS__); printf("\n"); } } while (0)
static MsString sso(const char *s) { MsString m; memset(&m, 0, sizeof m); strcpy(m.u.buf, s); m.size = strlen(s); m.cap = 15; return m; }
static MsString heap(char *s) { MsString m; m.u.ptr = s; m.size = strlen(s); m.cap = m.size | 15; return m; }

static void run(const char *name, int editor, MsString w, void *vt, int expectHandled, uint32_t expectWord)
{
    uint8_t obj[0x60]; memset(obj, 0xCC, sizeof obj); *(void **)obj = vt; *(uint32_t *)(obj + 0x5C) = 0x12345678;
    uint64_t out[9]; memset(out, 0, sizeof out);
    int logs0 = log_calls; memset(log_args, 0, sizeof log_args);
    if (editor) { g_multThisReg = 15; g_multReaderReg = 6; g_multFrameReg = 4; g_multFrameDisp = 0x20; fake_editor(&w, obj, out); }
    else        { g_multThisReg = 13; g_multReaderReg = 14; g_multFrameReg = 5; g_multFrameDisp = -0x59; fake_game(&w, obj, out); }
    printf("%-44s rax=%llu log=%d word=%08X\n", name, (unsigned long long)out[0], log_calls - logs0, *(uint32_t *)(obj + 0x5C));
    CHECK(out[1] == 0x1234 && out[2] == 0x5678 && out[6] == 1 && out[8] == 0x4242, "callee-saved registers clobbered");
    if (editor) { CHECK(out[3] == 0x7777 && out[4] == 0x8888 && out[5] == (uint64_t)(uintptr_t)obj && out[7] == 0xBEEF, "editor regs clobbered"); }
    else        { CHECK(out[3] == (uint64_t)(uintptr_t)obj && out[4] == 0xBEEF && out[5] == 0x9ABC && out[7] == 0xDEF0, "game regs clobbered"); }
    if (expectHandled) {
        CHECK(out[0] == 1 && log_calls == logs0, "should be consumed without logging");
        CHECK(*(uint32_t *)(obj + 0x5C) == expectWord, "stored %08X, expected %08X", *(uint32_t *)(obj + 0x5C), expectWord);
    } else {
        CHECK(out[0] == 77 && log_calls == logs0 + 1, "should reach the engine logger");
        CHECK(log_args[0] == 0xA1 && log_args[1] == 0x1F4 && log_args[2] == 0xA3 && log_args[3] == 0x0BAD0001, "logger got wrong arguments");
        CHECK(*(uint32_t *)(obj + 0x5C) == 0x12345678, "object modified on the fallback path");
    }
    for (int i = 8; i < 0x5C; i++) CHECK(obj[i] == 0xCC, "object byte +0x%X modified", i);
}

int main(void)
{
    char longw[] = "some_much_longer_block_word";
    float_result = 2.5f;  run("game: {multiplier 2.5} pierce", 0, sso("multiplier"), g_vtPierce, 1, MULT_MAGIC | 2500);
    float_result = 0.5f;  run("editor: {multiplier 0.5} blast", 1, sso("multiplier"), g_vtBlast, 1, MULT_MAGIC | 500);
    float_result = 1.0f;  run("game: {Multiplier 1} (case)", 0, sso("Multiplier"), g_vtBlast, 1, MULT_MAGIC | 1000);
    float_result = -3.0f; run("game: {multiplier -3} -> 0", 0, sso("multiplier"), g_vtPierce, 1, MULT_MAGIC | 0);
    float_result = 999.f; run("editor: {multiplier 999} -> 65.535", 1, sso("multiplier"), g_vtPierce, 1, MULT_MAGIC | 65535);
    float_result = 0.0f / 0.0f; run("game: {multiplier NaN} -> 1", 0, sso("multiplier"), g_vtPierce, 1, MULT_MAGIC | 1000);
    run("game: other word {multiplied 2}", 0, sso("multiplied"), g_vtPierce, 0, 0);
    run("editor: other word {effects_x}", 1, sso("effects_x"), g_vtBlast, 0, 0);
    run("game: long heap word", 0, heap(longw), g_vtPierce, 0, 0);
    run("editor: {multiplier} in another class", 1, sso("multiplier"), (void *)0x3333, 0, 0);
    peek_result = 0; run("game: {multiplier \"x\"} (no number)", 0, sso("multiplier"), g_vtPierce, 0, 0); peek_result = 1;
    printf(fails ? "%d FAILED\n" : "ALL PASSED\n", fails);
    return fails != 0;
}
