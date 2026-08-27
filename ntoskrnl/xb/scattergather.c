/*
 * NtReadFileScatter / NtWriteFileGather -- file IO whose buffer is an array
 * of FILE_SEGMENT_ELEMENTs, one page each, rather than one flat range.
 *
 * Probed on the console: the handle must have been opened with
 * FILE_NO_INTERMEDIATE_BUFFERING (a buffered handle is rejected outright,
 * with nothing transferred); each element addresses the page it points
 * into, low bits ignored; Length need not be a whole number of pages, and a
 * short Length simply stops the transfer partway through an element; a
 * transfer that runs into end-of-file is a short success, and one that
 * starts at or past it is STATUS_END_OF_FILE.
 *
 * That is exactly a run of per-page transfers, which is how this is built:
 * one ordinary read or write per element, the caller's completion signalled
 * once at the end.  The console can leave an asynchronous request pending
 * and complete it later; this runs the elements to completion first and
 * then reports, so the caller gets its final status directly rather than
 * STATUS_PENDING.  Either is a legal answer to the same call.
 */

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* NT's FILE_SEGMENT_ELEMENT is eight bytes wide (PVOID64); the console's is
 * a bare pointer, so a title's array is half the stride the NT type would
 * step through.  These two ordinals take the console's. */
typedef union _XB_FILE_SEGMENT_ELEMENT {
    PVOID Buffer;
    ULONG Alignment;
} XB_FILE_SEGMENT_ELEMENT, *PXB_FILE_SEGMENT_ELEMENT;

/* An element addresses a page frame; the low bits carry no information. */
#define SEGMENT_PAGE(e) \
    ((PVOID)((ULONG_PTR)(e).Buffer & ~(ULONG_PTR)(PAGE_SIZE - 1)))

static VOID NTAPI
XbSgFreeApc(PKAPC Apc, PKNORMAL_ROUTINE *NormalRoutine, PVOID *NormalContext,
            PVOID *SystemArgument1, PVOID *SystemArgument2)
{
    UNREFERENCED_PARAMETER(NormalRoutine);
    UNREFERENCED_PARAMETER(NormalContext);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    /* The delivery path has already taken its own copy of the routine and
     * its arguments, so the packet is ours to release here. */
    ExFreePool(Apc);
}

/* Report a finished transfer the way the IO manager reports one: status and
 * count into the caller's block, then its event, then its APC. */
static VOID
XbSgComplete(HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
             PIO_STATUS_BLOCK IoStatusBlock, NTSTATUS Status, ULONG Transferred)
{
    PKAPC apc;
    PKEVENT event;

    IoStatusBlock->Status = Status;
    IoStatusBlock->Information = Transferred;

    if (Event != NULL &&
        NT_SUCCESS(ObReferenceObjectByHandle(Event, EVENT_MODIFY_STATE,
                                             ExEventObjectType, KernelMode,
                                             (PVOID *)&event, NULL)))
    {
        KeSetEvent(event, IO_NO_INCREMENT, FALSE);
        ObDereferenceObject(event);
    }

    if (ApcRoutine == NULL)
        return;

    apc = ExAllocatePoolWithTag(NonPagedPool, sizeof(*apc), 'gSbX');
    if (apc == NULL)
        return;

    KeInitializeApc(apc, KeGetCurrentThread(), CurrentApcEnvironment,
                    XbSgFreeApc, NULL, (PKNORMAL_ROUTINE)ApcRoutine,
                    KernelMode, ApcContext);
    KeInsertQueueApc(apc, IoStatusBlock, NULL, IO_NO_INCREMENT);
}

static NTSTATUS
XbScatterGather(BOOLEAN Write, HANDLE FileHandle, HANDLE Event,
                PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
                PIO_STATUS_BLOCK IoStatusBlock,
                PXB_FILE_SEGMENT_ELEMENT SegmentArray, ULONG Length,
                PLARGE_INTEGER ByteOffset)
{
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER offset;
    PFILE_OBJECT file;
    NTSTATUS status;
    ULONG done = 0;
    ULONG i;

    status = ObReferenceObjectByHandle(FileHandle,
                                       Write ? FILE_WRITE_DATA : FILE_READ_DATA,
                                       IoFileObjectType, KernelMode,
                                       (PVOID *)&file, NULL);
    if (!NT_SUCCESS(status))
        return status;

    /* Unbuffered IO only: a cached handle is refused before anything moves. */
    if (!(file->Flags & FO_NO_INTERMEDIATE_BUFFERING))
    {
        ObDereferenceObject(file);
        IoStatusBlock->Status = STATUS_INVALID_PARAMETER;
        IoStatusBlock->Information = 0;
        return STATUS_INVALID_PARAMETER;
    }

    if (ByteOffset != NULL)
        offset = *ByteOffset;

    status = STATUS_SUCCESS;
    for (i = 0; done < Length; i++)
    {
        ULONG chunk = Length - done;
        NTSTATUS one;

        if (chunk > PAGE_SIZE)
            chunk = PAGE_SIZE;
        if (SegmentArray[i].Buffer == NULL)
            break;

        one = Write
            ? NtWriteFile(FileHandle, NULL, NULL, NULL, &iosb,
                          SEGMENT_PAGE(SegmentArray[i]), chunk,
                          ByteOffset != NULL ? &offset : NULL, NULL)
            : NtReadFile(FileHandle, NULL, NULL, NULL, &iosb,
                         SEGMENT_PAGE(SegmentArray[i]), chunk,
                         ByteOffset != NULL ? &offset : NULL, NULL);
        if (one == STATUS_PENDING)
        {
            /* Asynchronous handle: nothing else of ours is outstanding on
             * it, so the file object's event is this element's completion. */
            KeWaitForSingleObject(&file->Event, Executive, KernelMode, FALSE,
                                  NULL);
            one = iosb.Status;
        }

        /* Running into end-of-file partway through is a short success; only
         * a transfer that moved nothing reports the error. */
        if (!NT_SUCCESS(one))
        {
            if (done == 0)
                status = one;
            break;
        }

        done += (ULONG)iosb.Information;
        offset.QuadPart += (LONGLONG)iosb.Information;
        if (iosb.Information < chunk)
            break;
    }

    ObDereferenceObject(file);
    XbSgComplete(Event, ApcRoutine, ApcContext, IoStatusBlock, status, done);
    return status;
}

NTSTATUS NTAPI
XeNtReadFileScatter(HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine,
                    PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock,
                    PXB_FILE_SEGMENT_ELEMENT SegmentArray, ULONG Length,
                    PLARGE_INTEGER ByteOffset)
{
    return XbScatterGather(FALSE, FileHandle, Event, ApcRoutine, ApcContext,
                           IoStatusBlock, SegmentArray, Length, ByteOffset);
}

NTSTATUS NTAPI
XeNtWriteFileGather(HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine,
                    PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock,
                    PXB_FILE_SEGMENT_ELEMENT SegmentArray, ULONG Length,
                    PLARGE_INTEGER ByteOffset)
{
    return XbScatterGather(TRUE, FileHandle, Event, ApcRoutine, ApcContext,
                           IoStatusBlock, SegmentArray, Length, ByteOffset);
}
