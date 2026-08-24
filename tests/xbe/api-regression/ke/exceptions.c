/*
 * Exception dispatch: hardware traps (#PF, #DE, #BP, #UD, #MF, #XF),
 * software raises (RtlRaiseException / RtlRaiseStatus), handler search
 * order, unwind notifications, and continue-execution -- the whole
 * trap -> KiDispatchException -> RtlDispatchException -> RtlUnwind
 * chain, none of which any other workload reaches.
 *
 * Frames are hand-rolled EXCEPTION_REGISTRATION records on fs:[0] (the
 * KPCR starts with the NT_TIB, so this is the same list kernel SEH
 * uses).  Frames must live on the current stack: the dispatcher
 * validates them against the thread's stack limits.
 *
 * RtlUnwind does not preserve callee-saved registers, so a handler
 * that unwinds must touch only globals afterwards and leave with
 * longjmp (which restores every register from the jmp_buf).
 */

#include "../harness.h"
#include <setjmp.h>
#include <string.h>

#ifndef STATUS_ACCESS_VIOLATION
#define STATUS_ACCESS_VIOLATION ((NTSTATUS)0xC0000005L)
#endif
#ifndef STATUS_ILLEGAL_INSTRUCTION
#define STATUS_ILLEGAL_INSTRUCTION ((NTSTATUS)0xC000001DL)
#endif
#ifndef STATUS_INTEGER_DIVIDE_BY_ZERO
#define STATUS_INTEGER_DIVIDE_BY_ZERO ((NTSTATUS)0xC0000094L)
#endif
#ifndef STATUS_BREAKPOINT
#define STATUS_BREAKPOINT ((NTSTATUS)0x80000003L)
#endif
#ifndef STATUS_FLOAT_DIVIDE_BY_ZERO
#define STATUS_FLOAT_DIVIDE_BY_ZERO ((NTSTATUS)0xC000008EL)
#endif
#ifndef STATUS_FLOAT_MULTIPLE_TRAPS
#define STATUS_FLOAT_MULTIPLE_TRAPS ((NTSTATUS)0xC00002B5L)
#endif

/* EXCEPTION_DISPOSITION values (not in nxdk headers). */
#define XD_CONTINUE_EXECUTION 0
#define XD_CONTINUE_SEARCH    1

/* A virtual address mapped in neither kernel: above any title
 * allocation, below kernel space. */
#define UNMAPPED_VA 0x60000000

typedef struct seh_frame {
    struct seh_frame *next;
    void *handler;
} seh_frame;

static jmp_buf g_jb;
static volatile NTSTATUS g_code;
static volatile ULONG g_flags;
static volatile ULONG g_nparams;
static ULONG_PTR g_info[4];
static void *volatile g_addr;
static volatile int g_search_hits;   /* handlers that declined */
static volatile int g_unwind_seen;   /* EXCEPTION_UNWIND notifications */

static void reset_state(void)
{
    g_code = 0;
    g_flags = 0xFFFFFFFF;
    g_nparams = 0;
    memset(g_info, 0, sizeof(g_info));
    g_addr = NULL;
    g_search_hits = 0;
    g_unwind_seen = 0;
}

static void push_frame(seh_frame *f, void *handler)
{
    f->handler = handler;
    __asm__ volatile(
        "movl %%fs:0, %%eax\n\t"
        "movl %%eax, %0\n\t"
        "movl %1, %%fs:0"
        : "=m"(f->next) : "r"(f) : "eax", "memory");
}

static void pop_frame(seh_frame *f)
{
    __asm__ volatile(
        "movl %0, %%eax\n\t"
        "movl %%eax, %%fs:0"
        :: "m"(f->next) : "eax", "memory");
}

static void record(EXCEPTION_RECORD *rec)
{
    g_code = rec->ExceptionCode;
    g_flags = rec->ExceptionFlags;
    g_nparams = rec->NumberParameters;
    g_addr = rec->ExceptionAddress;
    for (ULONG i = 0; i < 4 && i < rec->NumberParameters; i++)
        g_info[i] = rec->ExceptionInformation[i];
}

/* Record the exception, unwind back to this frame, longjmp home. */
static int __cdecl h_record_unwind(EXCEPTION_RECORD *rec, void *frame,
                                   CONTEXT *ctx, void *disp)
{
    (void)ctx; (void)disp;
    if (rec->ExceptionFlags & EXCEPTION_UNWIND) {
        g_unwind_seen++;
        return XD_CONTINUE_SEARCH;
    }
    record(rec);
    RtlUnwind(frame, NULL, rec, 0);
    /* Callee-saved registers are trash here; g_jb is absolute. */
    longjmp(g_jb, 1);
}

/* Decline; count first-chance and unwind visits separately. */
static int __cdecl h_decline(EXCEPTION_RECORD *rec, void *frame,
                             CONTEXT *ctx, void *disp)
{
    (void)frame; (void)ctx; (void)disp;
    if (rec->ExceptionFlags & EXCEPTION_UNWIND)
        g_unwind_seen++;
    else
        g_search_hits++;
    return XD_CONTINUE_SEARCH;
}

/* Repair the faulting access by committing the page it touched, then
 * resume: the instruction re-executes and succeeds.  No context access
 * needed, so it works regardless of the kernel's CONTEXT layout.
 * Every failure leg unwinds out with a distinct longjmp value so the
 * test reports instead of dying unhandled. */
static volatile NTSTATUS g_alloc_status;
static volatile int g_fault_count;

static int __cdecl h_commit_and_continue(EXCEPTION_RECORD *rec, void *frame,
                                         CONTEXT *ctx, void *disp)
{
    (void)ctx; (void)disp;
    if (rec->ExceptionFlags & EXCEPTION_UNWIND)
        return XD_CONTINUE_SEARCH;
    record(rec);

    if (++g_fault_count > 3) {
        /* ContinueExecution is not retrying successfully -- bail. */
        RtlUnwind(frame, NULL, rec, 0);
        longjmp(g_jb, 3);
    }

    PVOID base = (PVOID)(g_info[1] & ~0xFFF);
    SIZE_T size = 0x1000;
    g_alloc_status = NtAllocateVirtualMemory(&base, 0, &size,
                                             MEM_COMMIT | MEM_RESERVE,
                                             PAGE_READWRITE);
    if (!NT_SUCCESS(g_alloc_status)) {
        RtlUnwind(frame, NULL, rec, 0);
        longjmp(g_jb, 2);
    }
    return XD_CONTINUE_EXECUTION;
}

/* Unwind recovery resumes with the handler's EFLAGS -- notably IF=0
 * when the trap gate never re-enabled interrupts (#BP and the fp
 * traps), and RtlUnwind does not restore flags on either kernel -- so
 * every recovery point must re-enable interrupts itself. */
static void irq_on(void)
{
    __asm__ volatile("sti");
}

/* Run provoke() under h_record_unwind; return true if an exception
 * with the expected code arrived and control resumed by unwind. */
static bool expect_exception(void (*provoke)(void), NTSTATUS expected)
{
    seh_frame f;
    reset_state();
    if (setjmp(g_jb) == 0) {
        push_frame(&f, h_record_unwind);
        provoke();
        pop_frame(&f);
        FAIL_AND_RETURN("no exception was raised");
    }
    /* The handler unwound up to (not through) our frame. */
    irq_on();
    pop_frame(&f);
    if (g_code != expected)
        FAIL_AND_RETURN("code: got 0x%08x expected 0x%08x",
                        (unsigned)g_code, (unsigned)expected);
    ASSERT_NOT_NULL(g_addr);
    return true;
}

/* ---- provokers ---------------------------------------------------------- */

static void provoke_read_av(void)
{
    __asm__ volatile("movl 0x60000000, %%eax" ::: "eax", "memory");
}

static void provoke_write_av(void)
{
    __asm__ volatile("movl %%eax, 0x60000000" ::: "memory");
}

static void provoke_div0(void)
{
    __asm__ volatile(
        "xorl %%edx, %%edx\n\t"
        "movl $1, %%eax\n\t"
        "xorl %%ecx, %%ecx\n\t"
        "divl %%ecx"
        ::: "eax", "ecx", "edx");
}

static void provoke_ud2(void)
{
    __asm__ volatile("ud2");
}

static void provoke_x87_div0(void)
{
    /* Unmask the x87 zero-divide exception, compute 1.0/0.0, and force
     * delivery with fwait. */
    unsigned short cw, unmasked;
    __asm__ volatile("fnstcw %0" : "=m"(cw));
    unmasked = cw & ~0x0004;
    __asm__ volatile(
        "fnclex\n\t"
        "fldcw %0\n\t"
        "fld1\n\t"
        "fldz\n\t"
        "fdivp\n\t"
        "fwait"
        :: "m"(unmasked));
}

static void fpu_cleanup(void)
{
    /* Reset the FPU wholesale: masks restored, stack cleared, no
     * pending exceptions to detonate at the next fp instruction. */
    __asm__ volatile("fninit");
}

static unsigned int g_mxcsr_saved;

static void provoke_sse_div0(void)
{
    float num[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    float den[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    unsigned int unmasked;
    __asm__ volatile("stmxcsr %0" : "=m"(g_mxcsr_saved));
    unmasked = g_mxcsr_saved & ~0x0200;  /* unmask ZM */
    __asm__ volatile(
        "ldmxcsr %2\n\t"
        "movups %0, %%xmm0\n\t"
        "movups %1, %%xmm1\n\t"
        "divps %%xmm1, %%xmm0"
        :: "m"(num[0]), "m"(den[0]), "m"(unmasked) : "xmm0", "xmm1");
}

static void sse_cleanup(void)
{
    __asm__ volatile("ldmxcsr %0" :: "m"(g_mxcsr_saved));
}

/* ---- tests -------------------------------------------------------------- */

static bool t_raise_exception_roundtrip(void)
{
    seh_frame f;
    EXCEPTION_RECORD rec;
    reset_state();

    memset(&rec, 0, sizeof(rec));
    rec.ExceptionCode = (NTSTATUS)0xE0DEAD01;
    rec.NumberParameters = 3;
    rec.ExceptionInformation[0] = 0x11111111;
    rec.ExceptionInformation[1] = 0x22222222;
    rec.ExceptionInformation[2] = 0x33333333;

    if (setjmp(g_jb) == 0) {
        push_frame(&f, h_record_unwind);
        RtlRaiseException(&rec);
        pop_frame(&f);
        FAIL_AND_RETURN("RtlRaiseException returned");
    }
    irq_on();
    pop_frame(&f);
    ASSERT_EQ_U32(g_code, 0xE0DEAD01);
    ASSERT_EQ_U32(g_nparams, 3);
    ASSERT_EQ_U32(g_info[0], 0x11111111);
    ASSERT_EQ_U32(g_info[2], 0x33333333);
    ASSERT_NOT_NULL(g_addr);
    return true;
}

static bool t_raise_status(void)
{
    seh_frame f;
    reset_state();
    if (setjmp(g_jb) == 0) {
        push_frame(&f, h_record_unwind);
        RtlRaiseStatus(STATUS_INVALID_PARAMETER);
        pop_frame(&f);
        FAIL_AND_RETURN("RtlRaiseStatus returned");
    }
    irq_on();
    pop_frame(&f);
    ASSERT_NTSTATUS(g_code, STATUS_INVALID_PARAMETER);
    return true;
}

static bool t_read_access_violation(void)
{
    if (!expect_exception(provoke_read_av, STATUS_ACCESS_VIOLATION))
        return false;
    ASSERT_TRUE(g_nparams >= 2);
    ASSERT_EQ_U32((ULONG)g_info[0], 0);           /* read */
    ASSERT_EQ_U32((ULONG)g_info[1], UNMAPPED_VA); /* faulting VA */
    return true;
}

static bool t_write_access_violation(void)
{
    if (!expect_exception(provoke_write_av, STATUS_ACCESS_VIOLATION))
        return false;
    ASSERT_TRUE(g_nparams >= 2);
    ASSERT_EQ_U32((ULONG)g_info[0], 1);           /* write */
    ASSERT_EQ_U32((ULONG)g_info[1], UNMAPPED_VA);
    return true;
}

static bool t_integer_divide_by_zero(void)
{
    return expect_exception(provoke_div0, STATUS_INTEGER_DIVIDE_BY_ZERO);
}

static bool t_illegal_instruction(void)
{
    return expect_exception(provoke_ud2, STATUS_ILLEGAL_INSTRUCTION);
}

static void provoke_int3(void)
{
    __asm__ volatile("int3");
}

/* NT rewinds a kernel breakpoint's context onto the int3 itself (a
 * debugger is expected to step over it), so SEH recovery must unwind
 * rather than continue -- continuing would re-execute the int3. */
static bool t_breakpoint(void)
{
    return expect_exception(provoke_int3, STATUS_BREAKPOINT);
}

/* ContinueExecution with the fault repaired in memory: the write
 * faults on an uncommitted page, the handler commits it, and the
 * retried instruction succeeds. */
static bool t_continue_after_commit(void)
{
    seh_frame f;
    int jv;
    reset_state();
    g_alloc_status = 0;
    g_fault_count = 0;

    jv = setjmp(g_jb);
    if (jv == 0) {
        push_frame(&f, h_commit_and_continue);
        __asm__ volatile("movl $0x1234ABCD, 0x61000000" ::: "memory");
        pop_frame(&f);
    } else {
        /* A failure leg unwound out of the handler. */
        irq_on();
        pop_frame(&f);
        if (jv == 2)
            FAIL_AND_RETURN("handler alloc -> 0x%08x",
                            (unsigned)g_alloc_status);
        FAIL_AND_RETURN("retry kept faulting (%d)", g_fault_count);
    }

    ASSERT_NTSTATUS(g_code, STATUS_ACCESS_VIOLATION);
    ASSERT_EQ_U32(*(volatile ULONG *)0x61000000, 0x1234ABCD);

    PVOID base = (PVOID)0x61000000;
    SIZE_T size = 0;
    NTSTATUS s = NtFreeVirtualMemory(&base, &size, MEM_RELEASE);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    return true;
}

static bool t_search_order_and_unwind(void)
{
    /* outer handles, inner declines: the dispatcher must visit the
     * inner frame first, and the unwind must notify it once. */
    seh_frame outer, inner;
    reset_state();

    if (setjmp(g_jb) == 0) {
        push_frame(&outer, h_record_unwind);
        push_frame(&inner, h_decline);
        RtlRaiseStatus(STATUS_UNSUCCESSFUL);
        pop_frame(&inner);
        pop_frame(&outer);
        FAIL_AND_RETURN("RtlRaiseStatus returned");
    }
    /* Unwound to the outer frame; the inner one is already gone. */
    irq_on();
    pop_frame(&outer);
    ASSERT_NTSTATUS(g_code, STATUS_UNSUCCESSFUL);
    ASSERT_EQ_U32(g_search_hits, 1);
    ASSERT_EQ_U32(g_unwind_seen, 1);
    return true;
}

static bool t_x87_divide_by_zero(void)
{
    bool ok = expect_exception(provoke_x87_div0, STATUS_FLOAT_DIVIDE_BY_ZERO);
    fpu_cleanup();
    return ok;
}

static bool t_sse_divide_by_zero(void)
{
    bool ok = expect_exception(provoke_sse_div0, STATUS_FLOAT_MULTIPLE_TRAPS);
    sse_cleanup();
    return ok;
}

static const test_entry_t ke_exceptions_entries[] = {
    {"raise_exception_roundtrip", t_raise_exception_roundtrip},
    {"raise_status",              t_raise_status},
    {"read_access_violation",     t_read_access_violation},
    {"write_access_violation",    t_write_access_violation},
    {"integer_divide_by_zero",    t_integer_divide_by_zero},
    {"illegal_instruction",       t_illegal_instruction},
    {"breakpoint",                t_breakpoint},
    {"continue_after_commit",     t_continue_after_commit},
    {"search_order_and_unwind",   t_search_order_and_unwind},
    /* xemu's TCG computes masked results regardless of the FPU/MXCSR
     * exception masks, so #MF/#XF never deliver under emulation (both
     * kernels agree).  Would pass on hardware; keep as TODO so they
     * light up if xemu gains FP-exception support. */
    {"x87_divide_by_zero",        t_x87_divide_by_zero,
     "xemu TCG never raises unmasked FP exceptions"},
    {"sse_divide_by_zero",        t_sse_divide_by_zero,
     "xemu TCG never raises unmasked FP exceptions"},
};

DEFINE_GROUP(ke_exceptions, "ke/exceptions");
