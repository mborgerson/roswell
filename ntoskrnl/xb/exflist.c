/*
 * Xbox Exf* interlocked doubly linked list primitives.
 *
 * The retail kernel exports the head/tail insert and head remove without
 * the trailing spinlock the NT/ReactOS forms carry: the Xbox is single
 * processor, so atomicity comes from masking interrupts around the splice
 * rather than from a spinlock.  These adapters splice directly with
 * interrupts off and return the list neighbour that occupied the slot
 * before the operation (NULL when the list was empty), matching retail.
 *
 * They carry an Xb-prefixed C name (the exported ordinal is rebound to it
 * via the ordinal map) to avoid colliding with the three-argument
 * ExfInterlocked* prototypes the DDK headers declare for the NT forms.
 */

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

PLIST_ENTRY
FASTCALL
XbExfInterlockedInsertHeadList(IN OUT PLIST_ENTRY ListHead,
                               IN PLIST_ENTRY ListEntry)
{
    BOOLEAN Enable = KeDisableInterrupts();
    PLIST_ENTRY FirstEntry = ListHead->Flink;

    InsertHeadList(ListHead, ListEntry);

    KeRestoreInterrupts(Enable);
    return (FirstEntry == ListHead) ? NULL : FirstEntry;
}

PLIST_ENTRY
FASTCALL
XbExfInterlockedInsertTailList(IN OUT PLIST_ENTRY ListHead,
                               IN PLIST_ENTRY ListEntry)
{
    BOOLEAN Enable = KeDisableInterrupts();
    PLIST_ENTRY LastEntry = ListHead->Blink;

    InsertTailList(ListHead, ListEntry);

    KeRestoreInterrupts(Enable);
    return (LastEntry == ListHead) ? NULL : LastEntry;
}

PLIST_ENTRY
FASTCALL
XbExfInterlockedRemoveHeadList(IN OUT PLIST_ENTRY ListHead)
{
    BOOLEAN Enable = KeDisableInterrupts();
    PLIST_ENTRY ListEntry;

    if (IsListEmpty(ListHead))
        ListEntry = NULL;
    else
        ListEntry = RemoveHeadList(ListHead);

    KeRestoreInterrupts(Enable);
    return ListEntry;
}
