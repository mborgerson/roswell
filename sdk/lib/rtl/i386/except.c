/*
 * COPYRIGHT:         See COPYING in the top level directory
 * PROJECT:           ReactOS Run-Time Library
 * PURPOSE:           User-mode exception support for IA-32
 * FILE:              lib/rtl/i386/except.c
 * PROGRAMERS:        Alex Ionescu (alex@relsoft.net)
 *                    Casper S. Hornstrup (chorns@users.sourceforge.net)
 */

/* INCLUDES *****************************************************************/

#include <rtl.h>
#define NDEBUG
#include <debug.h>

/* PUBLIC FUNCTIONS **********************************************************/

/*
 * @implemented
 */
VOID
NTAPI
RtlGetCallersAddress(OUT PVOID *CallersAddress,
                     OUT PVOID *CallersCaller)
{
    USHORT FrameCount;
    PVOID  BackTrace[2];
    PULONG BackTraceHash = NULL;

    /* Get the tow back trace address */
    FrameCount = RtlCaptureStackBackTrace(2, 2, &BackTrace[0],BackTraceHash);

    /* Only if user want it */
    if (CallersAddress != NULL)
    {
        /* only when first frames exist */
        if (FrameCount >= 1)
        {
            *CallersAddress = BackTrace[0];
        }
        else
        {
            *CallersAddress = NULL;
        }
    }

    /* Only if user want it */
    if (CallersCaller != NULL)
    {
        /* only when second frames exist */
        if (FrameCount >= 2)
        {
            *CallersCaller = BackTrace[1];
        }
        else
        {
            *CallersCaller = NULL;
        }
    }
}

#ifdef SARCH_XBOX
/*
 * The Xbox public CONTEXT differs from the NT layout the kernel uses
 * internally: no debug registers, no data-segment fields, and an
 * FXSAVE-shaped float area (0x208 bytes) directly after ContextFlags.
 * Title handlers and filters read and write through the Xbox layout,
 * so convert at the handler-call boundary.  Title code lives below the
 * kernel base; kernel-side handlers (PSEH) get the NT layout
 * unchanged.  The float area is not carried across -- retail leaves
 * fp state to the handler's own fxsave if it cares.
 *
 * The 0x204-byte float area is the packed Xbox FLOATING_SAVE_AREA:
 * four WORDs, six DWORDs, 128+128+224 raw bytes, Cr0NpxState.
 */
typedef struct _XBOX_CONTEXT
{
    ULONG ContextFlags;
    UCHAR FloatSave[0x204];
    ULONG Edi;
    ULONG Esi;
    ULONG Ebx;
    ULONG Edx;
    ULONG Ecx;
    ULONG Eax;
    ULONG Ebp;
    ULONG Eip;
    ULONG SegCs;
    ULONG EFlags;
    ULONG Esp;
    ULONG SegSs;
} XBOX_CONTEXT;

static EXCEPTION_DISPOSITION
RtlpCallHandler(IN PEXCEPTION_RECORD ExceptionRecord,
                IN PEXCEPTION_REGISTRATION_RECORD RegistrationFrame,
                IN OUT PCONTEXT Context,
                IN PVOID DispatcherContext,
                IN BOOLEAN ForUnwind)
{
    PEXCEPTION_ROUTINE Handler = RegistrationFrame->Handler;
    EXCEPTION_DISPOSITION Disposition;
    XBOX_CONTEXT XboxContext;

    if ((ULONG_PTR)Handler >= 0x80000000)
    {
        return ForUnwind
            ? RtlpExecuteHandlerForUnwind(ExceptionRecord, RegistrationFrame,
                                          Context, DispatcherContext, Handler)
            : RtlpExecuteHandlerForException(ExceptionRecord,
                                             RegistrationFrame, Context,
                                             DispatcherContext, Handler);
    }

    RtlZeroMemory(XboxContext.FloatSave, sizeof(XboxContext.FloatSave));
    XboxContext.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    XboxContext.Edi = Context->Edi;
    XboxContext.Esi = Context->Esi;
    XboxContext.Ebx = Context->Ebx;
    XboxContext.Edx = Context->Edx;
    XboxContext.Ecx = Context->Ecx;
    XboxContext.Eax = Context->Eax;
    XboxContext.Ebp = Context->Ebp;
    XboxContext.Eip = Context->Eip;
    XboxContext.SegCs = Context->SegCs;
    XboxContext.EFlags = Context->EFlags;
    XboxContext.Esp = Context->Esp;
    XboxContext.SegSs = Context->SegSs;

    Disposition = ForUnwind
        ? RtlpExecuteHandlerForUnwind(ExceptionRecord, RegistrationFrame,
                                      (PCONTEXT)&XboxContext,
                                      DispatcherContext, Handler)
        : RtlpExecuteHandlerForException(ExceptionRecord, RegistrationFrame,
                                         (PCONTEXT)&XboxContext,
                                         DispatcherContext, Handler);

    /* Carry handler modifications (Eip surgery, register fixes) back. */
    Context->Edi = XboxContext.Edi;
    Context->Esi = XboxContext.Esi;
    Context->Ebx = XboxContext.Ebx;
    Context->Edx = XboxContext.Edx;
    Context->Ecx = XboxContext.Ecx;
    Context->Eax = XboxContext.Eax;
    Context->Ebp = XboxContext.Ebp;
    Context->Eip = XboxContext.Eip;
    Context->EFlags = XboxContext.EFlags;
    Context->Esp = XboxContext.Esp;

    return Disposition;
}
#else
#define RtlpCallHandler(Record, Frame, Ctx, DispCtx, ForUnwind)         \
    ((ForUnwind)                                                        \
     ? RtlpExecuteHandlerForUnwind((Record), (Frame), (Ctx), (DispCtx), \
                                   (Frame)->Handler)                    \
     : RtlpExecuteHandlerForException((Record), (Frame), (Ctx),         \
                                      (DispCtx), (Frame)->Handler))
#endif

/*
 * @implemented
 */
BOOLEAN
NTAPI
RtlDispatchException(IN PEXCEPTION_RECORD ExceptionRecord,
                     IN PCONTEXT Context)
{
    PEXCEPTION_REGISTRATION_RECORD RegistrationFrame, NestedFrame = NULL;
    DISPATCHER_CONTEXT DispatcherContext;
    EXCEPTION_RECORD ExceptionRecord2;
    EXCEPTION_DISPOSITION Disposition;
    ULONG_PTR StackLow, StackHigh;
    ULONG_PTR RegistrationFrameEnd;

    /* Perform vectored exception handling for user mode */
    if (RtlCallVectoredExceptionHandlers(ExceptionRecord, Context))
    {
        /* Exception handled, now call vectored continue handlers */
        RtlCallVectoredContinueHandlers(ExceptionRecord, Context);

        /* Continue execution */
        return TRUE;
    }

    /* Get the current stack limits and registration frame */
    RtlpGetStackLimits(&StackLow, &StackHigh);
    RegistrationFrame = RtlpGetExceptionList();

    /* Now loop every frame */
    while (RegistrationFrame != EXCEPTION_CHAIN_END)
    {
        /* Registration chain entries are never NULL */
        ASSERT(RegistrationFrame != NULL);

        /* Find out where it ends */
        RegistrationFrameEnd = (ULONG_PTR)RegistrationFrame +
                                sizeof(EXCEPTION_REGISTRATION_RECORD);

        /* Make sure the registration frame is located within the stack */
        if ((RegistrationFrameEnd > StackHigh) ||
            ((ULONG_PTR)RegistrationFrame < StackLow) ||
            ((ULONG_PTR)RegistrationFrame & 0x3))
        {
            /* Check if this happened in the DPC Stack */
            if (RtlpHandleDpcStackException(RegistrationFrame,
                                            RegistrationFrameEnd,
                                            &StackLow,
                                            &StackHigh))
            {
                /* Use DPC Stack Limits and restart */
                continue;
            }

            /* Set invalid stack and bail out */
            ExceptionRecord->ExceptionFlags |= EXCEPTION_STACK_INVALID;
            return FALSE;
        }

        //
        // TODO: Implement and call here RtlIsValidHandler(RegistrationFrame->Handler)
        // for supporting SafeSEH functionality, see the following articles:
        // https://www.optiv.com/blog/old-meets-new-microsoft-windows-safeseh-incompatibility
        // https://msrc-blog.microsoft.com/2012/01/10/more-information-on-the-impact-of-ms12-001/
        //

        /* Check if logging is enabled */
        RtlpCheckLogException(ExceptionRecord,
                              Context,
                              RegistrationFrame,
                              sizeof(*RegistrationFrame));

        /* Call the handler */
        Disposition = RtlpCallHandler(ExceptionRecord,
                                      RegistrationFrame,
                                      Context,
                                      &DispatcherContext,
                                      FALSE);

        /* Check if this is a nested frame */
        if (RegistrationFrame == NestedFrame)
        {
            /* Mask out the flag and the nested frame */
            ExceptionRecord->ExceptionFlags &= ~EXCEPTION_NESTED_CALL;
            NestedFrame = NULL;
        }

        /* Handle the dispositions */
        switch (Disposition)
        {
            /* Continue execution */
            case ExceptionContinueExecution:
            {
                /* Check if it was non-continuable */
                if (ExceptionRecord->ExceptionFlags & EXCEPTION_NONCONTINUABLE)
                {
                    /* Set up the exception record */
                    ExceptionRecord2.ExceptionRecord = ExceptionRecord;
                    ExceptionRecord2.ExceptionCode =
                        STATUS_NONCONTINUABLE_EXCEPTION;
                    ExceptionRecord2.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
                    ExceptionRecord2.NumberParameters = 0;

                    /* Raise the exception */
                    RtlRaiseException(&ExceptionRecord2);
                }
                else
                {
                    /* In user mode, call any registered vectored continue handlers */
                    RtlCallVectoredContinueHandlers(ExceptionRecord, Context);

                    /* Execution continues */
                    return TRUE;
                }
            }

            /* Continue searching */
            case ExceptionContinueSearch:
                if (ExceptionRecord->ExceptionFlags & EXCEPTION_STACK_INVALID)
                {
                    /* We have an invalid stack, bail out */
                    return FALSE;
                }
                break;

            /* Nested exception */
            case ExceptionNestedException:
            {
                /* Turn the nested flag on */
                ExceptionRecord->ExceptionFlags |= EXCEPTION_NESTED_CALL;

                /* Update the current nested frame */
                if (DispatcherContext.RegistrationPointer > NestedFrame)
                {
                    /* Get the frame from the dispatcher context */
                    NestedFrame = DispatcherContext.RegistrationPointer;
                }
                break;
            }

            /* Anything else */
            default:
            {
                /* Set up the exception record */
                ExceptionRecord2.ExceptionRecord = ExceptionRecord;
                ExceptionRecord2.ExceptionCode = STATUS_INVALID_DISPOSITION;
                ExceptionRecord2.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
                ExceptionRecord2.NumberParameters = 0;

                /* Raise the exception */
                RtlRaiseException(&ExceptionRecord2);
                break;
            }
        }

        /* Go to the next frame */
        RegistrationFrame = RegistrationFrame->Next;
    }

    /* Unhandled, bail out */
    return FALSE;
}

/*
 * @implemented
 */
VOID
NTAPI
RtlUnwind(IN PVOID TargetFrame OPTIONAL,
          IN PVOID TargetIp OPTIONAL,
          IN PEXCEPTION_RECORD ExceptionRecord OPTIONAL,
          IN PVOID ReturnValue)
{
    PEXCEPTION_REGISTRATION_RECORD RegistrationFrame, OldFrame;
    DISPATCHER_CONTEXT DispatcherContext;
    EXCEPTION_RECORD ExceptionRecord2, ExceptionRecord3;
    EXCEPTION_DISPOSITION Disposition;
    ULONG_PTR StackLow, StackHigh;
    ULONG_PTR RegistrationFrameEnd;
    CONTEXT LocalContext;
    PCONTEXT Context;

    /* Get the current stack limits */
    RtlpGetStackLimits(&StackLow, &StackHigh);

    /* Check if we don't have an exception record */
    if (!ExceptionRecord)
    {
        /* Overwrite the argument */
        ExceptionRecord = &ExceptionRecord3;

        /* Setup a local one */
        ExceptionRecord3.ExceptionFlags = 0;
        ExceptionRecord3.ExceptionCode = STATUS_UNWIND;
        ExceptionRecord3.ExceptionRecord = NULL;
        ExceptionRecord3.ExceptionAddress = _ReturnAddress();
        ExceptionRecord3.NumberParameters = 0;
    }

    /* Check if we have a frame */
    if (TargetFrame)
    {
        /* Set it as unwinding */
        ExceptionRecord->ExceptionFlags |= EXCEPTION_UNWINDING;
    }
    else
    {
        /* Set the Exit Unwind flag as well */
        ExceptionRecord->ExceptionFlags |= (EXCEPTION_UNWINDING |
                                            EXCEPTION_EXIT_UNWIND);
    }

    /* Now capture the context */
    Context = &LocalContext;
    LocalContext.ContextFlags = CONTEXT_INTEGER |
                                CONTEXT_CONTROL |
                                CONTEXT_SEGMENTS;
    RtlpCaptureContext(Context);

    /* Pop the current arguments off */
    Context->Esp += sizeof(TargetFrame) +
                    sizeof(TargetIp) +
                    sizeof(ExceptionRecord) +
                    sizeof(ReturnValue);

    /* Set the new value for EAX */
    Context->Eax = (ULONG)ReturnValue;

    /* Get the current frame */
    RegistrationFrame = RtlpGetExceptionList();

    /* Now loop every frame */
    while (RegistrationFrame != EXCEPTION_CHAIN_END)
    {
        /* Registration chain entries are never NULL */
        ASSERT(RegistrationFrame != NULL);

        /* If this is the target */
        if (RegistrationFrame == TargetFrame) ZwContinue(Context, FALSE);

        /* Check if the frame is too low */
        if ((TargetFrame) &&
            ((ULONG_PTR)TargetFrame < (ULONG_PTR)RegistrationFrame))
        {
            /* Create an invalid unwind exception */
            ExceptionRecord2.ExceptionCode = STATUS_INVALID_UNWIND_TARGET;
            ExceptionRecord2.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
            ExceptionRecord2.ExceptionRecord = ExceptionRecord;
            ExceptionRecord2.NumberParameters = 0;

            /* Raise the exception */
            RtlRaiseException(&ExceptionRecord2);
        }

        /* Find out where it ends */
        RegistrationFrameEnd = (ULONG_PTR)RegistrationFrame +
                               sizeof(EXCEPTION_REGISTRATION_RECORD);

        /* Make sure the registration frame is located within the stack */
        if ((RegistrationFrameEnd > StackHigh) ||
            ((ULONG_PTR)RegistrationFrame < StackLow) ||
            ((ULONG_PTR)RegistrationFrame & 0x3))
        {
            /* Check if this happened in the DPC Stack */
            if (RtlpHandleDpcStackException(RegistrationFrame,
                                            RegistrationFrameEnd,
                                            &StackLow,
                                            &StackHigh))
            {
                /* Use DPC Stack Limits and restart */
                continue;
            }

            /* Create an invalid stack exception */
            ExceptionRecord2.ExceptionCode = STATUS_BAD_STACK;
            ExceptionRecord2.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
            ExceptionRecord2.ExceptionRecord = ExceptionRecord;
            ExceptionRecord2.NumberParameters = 0;

            /* Raise the exception */
            RtlRaiseException(&ExceptionRecord2);
        }
        else
        {
            /* Call the handler */
            Disposition = RtlpCallHandler(ExceptionRecord,
                                          RegistrationFrame,
                                          Context,
                                          &DispatcherContext,
                                          TRUE);

            switch(Disposition)
            {
                /* Continue searching */
                case ExceptionContinueSearch:
                    break;

                /* Collision */
                case ExceptionCollidedUnwind:
                {
                    /* Get the original frame */
                    RegistrationFrame = DispatcherContext.RegistrationPointer;
                    break;
                }

                /* Anything else */
                default:
                {
                    /* Set up the exception record */
                    ExceptionRecord2.ExceptionRecord = ExceptionRecord;
                    ExceptionRecord2.ExceptionCode = STATUS_INVALID_DISPOSITION;
                    ExceptionRecord2.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
                    ExceptionRecord2.NumberParameters = 0;

                    /* Raise the exception */
                    RtlRaiseException(&ExceptionRecord2);
                    break;
                }
            }

            /* Go to the next frame */
            OldFrame = RegistrationFrame;
            RegistrationFrame = RegistrationFrame->Next;

            /* Remove this handler */
            RtlpSetExceptionList(OldFrame);
        }
    }

    /* Check if we reached the end */
    if (TargetFrame == EXCEPTION_CHAIN_END)
    {
        /* Unwind completed, so we don't exit */
        ZwContinue(Context, FALSE);
    }
    else
    {
        /* This is an exit_unwind or the frame wasn't present in the list */
        ZwRaiseException(ExceptionRecord, Context, FALSE);
    }
}

/* EOF */
