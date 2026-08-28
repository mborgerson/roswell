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

/* #GP: load a garbage selector.  A faulting segment load leaves the
 * register unchanged, and gs is unused in ring 0. */
static void provoke_gp(void)
{
    __asm__ volatile("movw $0x9b, %%ax; movw %%ax, %%gs" ::: "eax");
}

/* #DB: single-step trap on the instruction after TF is set.  The trap
 * gate clears TF for the handler, and unwind recovery abandons the
 * TF-set context, so stepping does not resume. */
static void provoke_single_step(void)
{
    __asm__ volatile(
        "pushfl\n\t"
        "orl $0x100, (%%esp)\n\t"
        "popfl\n\t"
        "nop"
        ::: "memory", "cc");
}

/* #OF: signed overflow + into. */
static void provoke_into(void)
{
    __asm__ volatile("movb $0x7f, %%al; addb $1, %%al; into" ::: "eax", "cc");
}

/* #BR: bound check against a [0,1] range with index 5.  clang's
 * assembler dropped the mnemonic, so encode bound %eax,(%edx) raw. */
static void provoke_bound(void)
{
    static const int range[2] = { 0, 1 };
    __asm__ volatile(
        "movl $5, %%eax\n\t"
        "movl %0, %%edx\n\t"
        ".byte 0x62, 0x02"
        :: "r"(range) : "eax", "edx");
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

static bool t_general_protection(void)
{
    return expect_exception(provoke_gp, STATUS_ACCESS_VIOLATION);
}

static bool t_single_step(void)
{
    return expect_exception(provoke_single_step,
                            (NTSTATUS)0x80000004L /* STATUS_SINGLE_STEP */);
}

static bool t_integer_overflow(void)
{
    return expect_exception(provoke_into,
                            (NTSTATUS)0xC0000095L /* INTEGER_OVERFLOW */);
}

static bool t_bound_range(void)
{
    return expect_exception(provoke_bound,
                            (NTSTATUS)0xC000008CL /* ARRAY_BOUNDS_EXCEEDED */);
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

/* The exported breakpoints are the same int3 with the same recovery;
 * with nothing attached they are only reachable through SEH.  The
 * status DbgBreakPointWithStatus carries is handed to a debugger in
 * EAX and is invisible from here -- what the test pins is that the
 * argument leaves the stack balanced. */
static void provoke_dbg_break(void)
{
    DbgBreakPoint();
}

static void provoke_dbg_break_with_status(void)
{
    DbgBreakPointWithStatus(0x80000007 /* DBG_STATUS_CONTROL_C */);
}

static bool t_dbg_break_point(void)
{
    return expect_exception(provoke_dbg_break, STATUS_BREAKPOINT);
}

static bool t_dbg_break_point_with_status(void)
{
    return expect_exception(provoke_dbg_break_with_status, STATUS_BREAKPOINT);
}

/* The two failure reporters end in the same breakpoint.  Both print
 * what they were given first, so with nothing attached the only thing
 * left of them is the break -- and the fact that neither eats the
 * caller's stack on the way there. */
static void provoke_rtl_assert(void)
{
    RtlAssert((PVOID)"apireg == 0", (PVOID)"apireg.c", 1,
              (PCHAR)"deliberate");
}

static void provoke_rtl_rip(void)
{
    RtlRip((PVOID)"ApiRegression", (PVOID)"apireg == 0",
           (PVOID)"deliberate");
}

static bool t_rtl_assert(void)
{
    return expect_exception(provoke_rtl_assert, STATUS_BREAKPOINT);
}

static bool t_rtl_rip(void)
{
    return expect_exception(provoke_rtl_rip, STATUS_BREAKPOINT);
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

/* ---- CONTEXT layout ----------------------------------------------------- */

/* These pin the public CONTEXT layout as handlers see it (this file
 * compiles against the Xbox definition): the kernel must present and
 * honor these offsets even though it uses a different layout
 * internally. */

static ULONG g_ctx_eip, g_ctx_esp, g_ctx_cs;

static int __cdecl h_read_ctx_unwind(EXCEPTION_RECORD *rec, void *frame,
                                     CONTEXT *ctx, void *disp)
{
    (void)disp;
    if (rec->ExceptionFlags & EXCEPTION_UNWIND)
        return XD_CONTINUE_SEARCH;
    record(rec);
    g_ctx_eip = ctx->Eip;
    g_ctx_esp = ctx->Esp;
    g_ctx_cs = ctx->SegCs;
    RtlUnwind(frame, NULL, rec, 0);
    longjmp(g_jb, 1);
}

static bool t_context_read_layout(void)
{
    seh_frame f;
    reset_state();
    g_ctx_eip = g_ctx_esp = g_ctx_cs = 0;

    if (setjmp(g_jb) == 0) {
        push_frame(&f, h_read_ctx_unwind);
        provoke_read_av();
        pop_frame(&f);
        FAIL_AND_RETURN("no exception was raised");
    }
    irq_on();
    pop_frame(&f);
    ASSERT_NTSTATUS(g_code, STATUS_ACCESS_VIOLATION);

    /* Eip must point into the provoker. */
    ULONG lo = (ULONG)(ULONG_PTR)provoke_read_av;
    if (g_ctx_eip < lo || g_ctx_eip > lo + 64)
        FAIL_AND_RETURN("ctx Eip %08x not near provoker %08x",
                        (unsigned)g_ctx_eip, (unsigned)lo);
    /* Ring-0 code selector, stack pointer near this frame. */
    ASSERT_TRUE(g_ctx_cs != 0 && (g_ctx_cs & 3) == 0);
    ULONG here = (ULONG)(ULONG_PTR)&f;
    ASSERT_TRUE(g_ctx_esp > here - 0x10000 && g_ctx_esp < here + 0x1000);
    return true;
}

/* Skip the faulting instruction by advancing ctx->Eip -- the classic
 * filter fixup.  ud2 is two bytes, so Eip += 2 resumes at the nop. */
static int __cdecl h_skip2_continue(EXCEPTION_RECORD *rec, void *frame,
                                    CONTEXT *ctx, void *disp)
{
    (void)disp;
    if (rec->ExceptionFlags & EXCEPTION_UNWIND)
        return XD_CONTINUE_SEARCH;
    record(rec);
    if (++g_fault_count > 3) {
        RtlUnwind(frame, NULL, rec, 0);
        longjmp(g_jb, 3);
    }
    ctx->Eip += 2;
    return XD_CONTINUE_EXECUTION;
}

static bool t_skip_instruction_continue(void)
{
    seh_frame f;
    volatile int after = 0;
    reset_state();
    g_fault_count = 0;

    if (setjmp(g_jb) == 0) {
        push_frame(&f, h_skip2_continue);
        __asm__ volatile("ud2; nop");
        after = 1;
        pop_frame(&f);
    } else {
        irq_on();
        pop_frame(&f);
        FAIL_AND_RETURN("Eip advance not honored (%d faults)",
                        g_fault_count);
    }

    ASSERT_NTSTATUS(g_code, STATUS_ILLEGAL_INSTRUCTION);
    ASSERT_TRUE(after == 1);
    return true;
}

/* RtlCaptureContext requires an EBP frame in its caller (NT x86
 * contract), so call it through an asm shim that guarantees one.  The
 * captured context is the shim-caller's state as of the shim's
 * return: Eip lands back in this translation unit, Ebp is this
 * function's frame. */
__asm__(
    ".text\n"
    "_capture_with_frame:\n\t"
    "pushl %ebp\n\t"
    "movl %esp, %ebp\n\t"
    "pushl 8(%ebp)\n\t"
    "call _RtlCaptureContext@4\n\t"
    "popl %ebp\n\t"
    "ret\n");
extern void __cdecl capture_with_frame(CONTEXT *ctx);

static bool t_capture_context(void)
{
    /* Force a frame so %ebp is meaningful at the call site. */
    volatile char *pad = __builtin_alloca(16);
    pad[0] = 0;

    CONTEXT ctx;
    memset(&ctx, 0xEE, sizeof(ctx));
    capture_with_frame(&ctx);

    /* Retail leaves ContextFlags untouched -- the fill must survive. */
    ASSERT_EQ_U32(ctx.ContextFlags, 0xEEEEEEEE);

    /* Eip: back into this function's body. */
    ULONG fn = (ULONG)(ULONG_PTR)t_capture_context;
    if (ctx.Eip < fn || ctx.Eip > fn + 0x200)
        FAIL_AND_RETURN("Eip %08x not in test fn %08x",
                        (unsigned)ctx.Eip, (unsigned)fn);

    /* Ebp: this frame.  Esp: within it. */
    ULONG frame = (ULONG)(ULONG_PTR)__builtin_frame_address(0);
    ASSERT_EQ_U32(ctx.Ebp, frame);
    ASSERT_TRUE(ctx.Esp > frame - 0x1000 && ctx.Esp < frame + 0x100);

    /* Ring-0 flat segments, interrupts on. */
    ASSERT_TRUE(ctx.SegCs != 0 && (ctx.SegCs & 3) == 0);
    ASSERT_TRUE(ctx.SegSs != 0 && (ctx.SegSs & 3) == 0);
    ASSERT_TRUE(ctx.EFlags & 0x200);
    return true;
}

static const test_entry_t ke_exceptions_entries[] = {
    {"raise_exception_roundtrip", t_raise_exception_roundtrip},
    {"raise_status",              t_raise_status},
    {"read_access_violation",     t_read_access_violation},
    {"write_access_violation",    t_write_access_violation},
    {"integer_divide_by_zero",    t_integer_divide_by_zero},
    {"illegal_instruction",       t_illegal_instruction},
    {"general_protection",        t_general_protection},
    {"single_step",               t_single_step},
    {"integer_overflow",          t_integer_overflow},
    {"bound_range",               t_bound_range},
    {"dbg_break_point",             t_dbg_break_point},
    {"dbg_break_point_with_status", t_dbg_break_point_with_status},
    {"rtl_assert",                  t_rtl_assert},
    {"rtl_rip",                     t_rtl_rip},
    {"breakpoint",                t_breakpoint},
    {"continue_after_commit",     t_continue_after_commit},
    {"search_order_and_unwind",   t_search_order_and_unwind},
    {"context_read_layout",       t_context_read_layout},
    {"skip_instruction_continue", t_skip_instruction_continue},
    {"capture_context",           t_capture_context},
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
