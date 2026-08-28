/*
 * PROJECT:     nxkrnl -- a free kernel for the original Xbox
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The process object a title reaches through its own thread.
 *
 * The console publishes KPROCESS to titles: the running thread's APC
 * state names it, and it is the only handle a title has on its own
 * process -- so it is what KeSetPriorityProcess is called with.  One
 * process object serves the whole title.
 *
 * Our threads are NT's, so what a title sees is the Xbox-shaped shadow
 * appended to each KTHREAD (xb/xbe.c).  This fills the fields of that
 * shadow which name and describe the process, at the offsets the
 * console puts them.
 *
 * The offsets below were measured on the console rather than read off a
 * header: a probe case in ke/procprio printed them with offsetof under
 * the toolchain that builds the test image, booted against the retail
 * kernel.  A field written at the wrong offset is worse than one left
 * zero, so they are pinned here and asserted where they can be.
 *
 * NOT DONE, DELIBERATELY -- the thread list.  The console links every
 * thread into KPROCESS.ThreadListHead through KTHREAD.ThreadListEntry,
 * which sits at 0x104, past the end of the 0x80 shadow.  Doing it for
 * real means growing the shadow to the console's full 0x110 (KTHREAD
 * 0x240 -> ~0x2C4, +144 bytes a thread) and unlinking on *both* ways out
 * of a thread -- the trampoline's return in xb/xbe.c and ordinal 258
 * PsTerminateSystemThread, which today maps straight to ReactOS's with
 * no wrapper of ours.  Miss either and a title walking the list reads a
 * freed thread.  So the list is published empty, which is a true
 * statement about a list we do not keep, and the ke/procprio case that
 * expects it non-empty stays a documented divergence.  Nothing is known
 * to walk it: the thread count a title can actually observe comes from
 * PsQueryStatistics, which xb/psstats.c already answers correctly from
 * NT's own ActiveThreads.
 */

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* Xbox KPROCESS, measured: 0x1C bytes. */
#include <pshpack4.h>
typedef struct _XBE_KPROCESS
{
    LIST_ENTRY ReadListHead;        /* 0x00 */
    LIST_ENTRY ThreadListHead;      /* 0x08 */
    ULONG      StackCount;          /* 0x10 */
    LONG       ThreadQuantum;       /* 0x14 */
    CHAR       BasePriority;        /* 0x18 */
    UCHAR      DisableBoost;        /* 0x19 */
    UCHAR      DisableQuantum;      /* 0x1A */
    UCHAR      Padding;             /* 0x1B */
} XBE_KPROCESS, *PXBE_KPROCESS;
#include <poppack.h>

C_ASSERT(sizeof(XBE_KPROCESS) == 0x1C);
C_ASSERT(FIELD_OFFSET(XBE_KPROCESS, ThreadListHead) == 0x08);
C_ASSERT(FIELD_OFFSET(XBE_KPROCESS, StackCount) == 0x10);
C_ASSERT(FIELD_OFFSET(XBE_KPROCESS, ThreadQuantum) == 0x14);
C_ASSERT(FIELD_OFFSET(XBE_KPROCESS, BasePriority) == 0x18);

/* Xbox KTHREAD field offsets, within the shadow. */
#define XBE_KTHREAD_PRIORITY      0x32   /* SCHAR */
#define XBE_KTHREAD_APCLIST0      0x34   /* LIST_ENTRY */
#define XBE_KTHREAD_APCLIST1      0x3C   /* LIST_ENTRY */
#define XBE_KTHREAD_APCPROCESS    0x44   /* PKPROCESS */
#define XBE_KTHREAD_APCQUEUEABLE  0x4B   /* UCHAR */
#define XBE_KTHREAD_QUANTUM       0x6C   /* LONG */
#define XBE_KTHREAD_BASEPRIORITY  0x70   /* SCHAR */

/* Everything written lands inside the 0x80 shadow. */
C_ASSERT(XBE_KTHREAD_BASEPRIORITY < 0x80);

/* The quantum a thread starts with, as the console reports it. */
#define XBE_THREAD_QUANTUM        60

static XBE_KPROCESS NxkTitleProcess;
static BOOLEAN      NxkTitleProcessReady = FALSE;

static VOID
NxkInitializeTitleProcess(_In_ CHAR BasePriority)
{
    InitializeListHead(&NxkTitleProcess.ReadListHead);
    InitializeListHead(&NxkTitleProcess.ThreadListHead);
    NxkTitleProcess.ThreadQuantum = XBE_THREAD_QUANTUM;
    NxkTitleProcess.BasePriority = BasePriority;
    NxkTitleProcess.DisableBoost = 0;
    NxkTitleProcess.DisableQuantum = 0;
    NxkTitleProcessReady = TRUE;
}

/*
 * Fill the process-facing part of a thread's shadow.  Called as each
 * title thread registers, which is also when the thread count is
 * refreshed -- it is a snapshot rather than a running total, since
 * there is no hook on the way out to decrement one.
 */
VOID
NxkPublishThreadProcess(_Out_writes_bytes_(0x80) PUCHAR Shadow)
{
    PKTHREAD Thread = KeGetCurrentThread();
    CHAR Base = (CHAR)Thread->BasePriority;
    ULONG Threads = PsGetCurrentProcess()->ActiveThreads;

    if (!NxkTitleProcessReady)
        NxkInitializeTitleProcess(Base);

    NxkTitleProcess.StackCount = (Threads != 0) ? Threads : 1;

    /* Both APC queues start empty and point at themselves. */
    InitializeListHead((PLIST_ENTRY)(Shadow + XBE_KTHREAD_APCLIST0));
    InitializeListHead((PLIST_ENTRY)(Shadow + XBE_KTHREAD_APCLIST1));

    *(PVOID *)(Shadow + XBE_KTHREAD_APCPROCESS) = &NxkTitleProcess;
    Shadow[XBE_KTHREAD_APCQUEUEABLE] = TRUE;

    Shadow[XBE_KTHREAD_PRIORITY] = (UCHAR)Thread->Priority;
    Shadow[XBE_KTHREAD_BASEPRIORITY] = (UCHAR)Base;
    *(PLONG)(Shadow + XBE_KTHREAD_QUANTUM) = XBE_THREAD_QUANTUM;
}

/*
 * KeSetPriorityProcess -- a partial implementation, deliberately.
 *
 * Probed on the console: it returns the process' previous base
 * priority, writes the new one into the field with no validation at all
 * (0, 16, 31 and -1 all land verbatim), and moves every thread of the
 * title to the new priority.  Out of range the threads are clamped and
 * the clamp does not undo, so a caller that goes outside 1..15 and comes
 * back leaves its threads somewhere else entirely.
 *
 * What is done here: the return value, the field, and the calling
 * thread.  What is not: the title's *other* threads, which stay where
 * they were.  Moving them means walking a list of the title's threads,
 * and there isn't one -- the console's lives in KPROCESS.ThreadListHead,
 * which is empty here for the reason at the top of this file.  NT's own
 * EPROCESS thread list is not a substitute without care: there is one
 * process on this console and it carries the kernel's system threads
 * alongside the title's, so an unfiltered walk would re-prioritise the
 * kernel.  KTHREAD.XeXboxFs4 marks a registered title thread and is the
 * filter that walk would need.
 *
 * A title that sets its process priority therefore sees the right answer
 * back and gets the calling thread moved; the rest of its threads keep
 * running where they were.  That is a real divergence, and the
 * ke/procprio case that spawns a thread names it.
 */
LONG NTAPI
KeSetPriorityProcess(PVOID Process, LONG BasePriority)
{
    PXBE_KPROCESS Target = (PXBE_KPROCESS)Process;
    LONG Old;

    if (Target == NULL)
        return 0;

    /* The console stores whatever it is handed, so this does too. */
    Old = Target->BasePriority;
    Target->BasePriority = (CHAR)BasePriority;

    /* Only move a thread for a value the console would not have had to
     * clamp -- how it clamps was never pinned down, and guessing would
     * be worse than leaving the thread alone. */
    if (BasePriority >= 1 && BasePriority <= 15)
    {
        PKTHREAD Thread = KeGetCurrentThread();

        KeSetPriorityThread(Thread, BasePriority);
        Thread->XeXboxShadow[XBE_KTHREAD_PRIORITY] = (UCHAR)BasePriority;
        Thread->XeXboxShadow[XBE_KTHREAD_BASEPRIORITY] = (UCHAR)BasePriority;
    }

    return Old;
}

/* EOF */
