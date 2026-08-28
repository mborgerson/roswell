/*
 * PROJECT:     nxkrnl -- a free kernel for the original Xbox
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Objects a title defines and owns the storage for.
 *
 * ObCreateObject and ObInsertObject are the one place where the caller,
 * not the kernel, supplies the memory an object lives in: the caller's
 * OBJECT_TYPE carries an allocate and a free procedure and the object
 * manager calls them, so the body sits behind a four-word header of the
 * console's own shape rather than NT's.  Such an object cannot be one
 * of ntoskrnl's, so the header, the reference count and the name are
 * kept here, and the handle table and the namespace are reached through
 * a small ntoskrnl object -- the shim -- that holds nothing but a
 * pointer back.  Closing the last handle drops the shim, and the shim
 * drops the reference it took.
 */

#include <ntifs.h>
#include <ndk/obtypes.h>
#include <ndk/obfuncs.h>
#include "object-types.h"
#include "obcreate.h"

/* The console's object header: the four words in front of the body. */
typedef struct _XB_OBJECT_HEADER
{
    LONG PointerCount;
    LONG HandleCount;
    PVOID Type;
    ULONG Flags;
} XB_OBJECT_HEADER, *PXB_OBJECT_HEADER;

/* Prepended to the header when the object is named, with the name
 * characters themselves parked after the body. */
typedef struct _XB_OBJECT_NAME_INFO
{
    PVOID ChainLink;
    PVOID Directory;
    ANSI_STRING Name;
} XB_OBJECT_NAME_INFO, *PXB_OBJECT_NAME_INFO;

#define XB_OB_FLAG_NAMED_OBJECT     0x01
#define XB_OB_FLAG_PERMANENT_OBJECT 0x02
#define XB_OB_FLAG_ATTACHED_OBJECT  0x04

typedef PVOID (NTAPI *XB_ALLOCATE_PROCEDURE)(SIZE_T Size, ULONG Tag);
typedef VOID (NTAPI *XB_FREE_PROCEDURE)(PVOID Base);
typedef VOID (NTAPI *XB_DELETE_PROCEDURE)(PVOID Object);

#define XbHeader(Object) ((PXB_OBJECT_HEADER)(Object) - 1)

typedef struct _XB_TITLE_TYPE
{
    LIST_ENTRY Link;
    PVOID Type;
} XB_TITLE_TYPE, *PXB_TITLE_TYPE;

/* A zeroed spin lock is an unheld one, and the list head is built the
 * first time something goes on it. */
static LIST_ENTRY XobTitleTypes;
static KSPIN_LOCK XobTitleTypeLock;
static LONG XobTitleTypeCount;

/* The ntoskrnl object standing in for a title object in the handle
 * table and the namespace. */
typedef struct _XB_OBJECT_SHIM
{
    PVOID Object;
} XB_OBJECT_SHIM, *PXB_OBJECT_SHIM;

static POBJECT_TYPE XobShimType;
static LONG XobShimTypeState;

/*
 * Reference counting, the console's way: the count lives in the
 * caller's own header, and reaching zero runs the type's delete
 * procedure on the body and then hands the whole allocation -- name
 * info included -- back to the free procedure.
 */
VOID FASTCALL
XobReferenceObject(PVOID Object)
{
    InterlockedIncrement(&XbHeader(Object)->PointerCount);
}

VOID FASTCALL
XobDereferenceObject(PVOID Object)
{
    PXB_OBJECT_HEADER Header = XbHeader(Object);
    XBOX_OBJECT_TYPE *Type;
    PVOID Base;

    if (InterlockedDecrement(&Header->PointerCount) != 0)
        return;

    Type = Header->Type;
    if (Type->DeleteProcedure != NULL)
        ((XB_DELETE_PROCEDURE)Type->DeleteProcedure)(Object);

    Base = (Header->Flags & XB_OB_FLAG_NAMED_OBJECT)
               ? (PVOID)((PXB_OBJECT_NAME_INFO)Header - 1)
               : (PVOID)Header;
    ((XB_FREE_PROCEDURE)Type->FreeProcedure)(Base);
}

/*
 * A type becomes known the first time an object is created from it,
 * which is what lets the reference and lookup entries tell a title
 * object from one of ntoskrnl's.  Nothing removes a type: the storage
 * belongs to the image, and there are only ever a handful.
 */
BOOLEAN
XobIsTitleType(PVOID Type)
{
    PLIST_ENTRY Entry;
    KIRQL OldIrql;
    BOOLEAN Found = FALSE;

    if (Type == NULL || XobTitleTypeCount == 0)
        return FALSE;

    KeAcquireSpinLock(&XobTitleTypeLock, &OldIrql);
    for (Entry = XobTitleTypes.Flink; Entry != &XobTitleTypes;
         Entry = Entry->Flink)
    {
        if (CONTAINING_RECORD(Entry, XB_TITLE_TYPE, Link)->Type == Type)
        {
            Found = TRUE;
            break;
        }
    }
    KeReleaseSpinLock(&XobTitleTypeLock, OldIrql);
    return Found;
}

static VOID
XobRememberType(PVOID Type)
{
    PXB_TITLE_TYPE Known;
    PLIST_ENTRY Entry;
    KIRQL OldIrql;

    Known = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Known), 'TObX');
    if (Known == NULL)
        return;
    Known->Type = Type;

    KeAcquireSpinLock(&XobTitleTypeLock, &OldIrql);
    if (XobTitleTypeCount == 0)
        InitializeListHead(&XobTitleTypes);
    for (Entry = XobTitleTypes.Flink; Entry != &XobTitleTypes;
         Entry = Entry->Flink)
    {
        if (CONTAINING_RECORD(Entry, XB_TITLE_TYPE, Link)->Type == Type)
        {
            KeReleaseSpinLock(&XobTitleTypeLock, OldIrql);
            ExFreePool(Known);
            return;
        }
    }
    InsertTailList(&XobTitleTypes, &Known->Link);
    XobTitleTypeCount++;
    KeReleaseSpinLock(&XobTitleTypeLock, OldIrql);
}

BOOLEAN
XobHasTitleObjects(VOID)
{
    return XobTitleTypeCount != 0;
}

BOOLEAN
XobIsTitleObject(PVOID Object)
{
    return XobTitleTypeCount != 0 && XobIsTitleType(XbHeader(Object)->Type);
}

PVOID
XobObjectType(PVOID Object)
{
    return XbHeader(Object)->Type;
}

/* The last handle on the shim takes the shim's reference with it. */
static VOID NTAPI
XobShimDelete(PVOID Object)
{
    XobDereferenceObject(((PXB_OBJECT_SHIM)Object)->Object);
}

static VOID NTAPI
XobShimClose(PEPROCESS Process, PVOID Object, ACCESS_MASK GrantedAccess,
             ULONG ProcessHandleCount, ULONG SystemHandleCount)
{
    UNREFERENCED_PARAMETER(Process);
    UNREFERENCED_PARAMETER(GrantedAccess);
    UNREFERENCED_PARAMETER(ProcessHandleCount);
    UNREFERENCED_PARAMETER(SystemHandleCount);

    InterlockedDecrement(
        &XbHeader(((PXB_OBJECT_SHIM)Object)->Object)->HandleCount);
}

static GENERIC_MAPPING XobShimMapping = {
    STANDARD_RIGHTS_READ,
    STANDARD_RIGHTS_WRITE,
    STANDARD_RIGHTS_EXECUTE,
    STANDARD_RIGHTS_REQUIRED | SYNCHRONIZE,
};

/*
 * The shim type is built on first use rather than at boot: nothing
 * reaches it until a title creates an object of its own, and most
 * never do.
 */
static POBJECT_TYPE
XobGetShimType(VOID)
{
    extern NTSTATUS NTAPI NtYieldExecution(VOID);
    OBJECT_TYPE_INITIALIZER Initializer;
    UNICODE_STRING Name = RTL_CONSTANT_STRING(L"XboxObject");
    POBJECT_TYPE Type = NULL;

    if (InterlockedCompareExchange(&XobShimTypeState, 1, 0) == 0)
    {
        RtlZeroMemory(&Initializer, sizeof(Initializer));
        Initializer.Length = sizeof(Initializer);
        Initializer.PoolType = NonPagedPool;
        Initializer.ValidAccessMask = STANDARD_RIGHTS_ALL | SYNCHRONIZE;
        Initializer.GenericMapping = XobShimMapping;
        Initializer.DefaultNonPagedPoolCharge = sizeof(XB_OBJECT_SHIM);
        Initializer.CaseInsensitive = TRUE;
        Initializer.DeleteProcedure = XobShimDelete;
        Initializer.CloseProcedure = XobShimClose;

        if (NT_SUCCESS(ObCreateObjectType(&Name, &Initializer, NULL, &Type)))
            XobShimType = Type;
        InterlockedExchange(&XobShimTypeState, XobShimType != NULL ? 2 : 0);
    }

    /* Another thread got there first; it is a pool allocation and a
     * directory insert, so the wait is short. */
    while (XobShimTypeState == 1)
        ZwYieldExecution();

    return XobShimType;
}

/*
 * Split a path the way the console does: one leading separator is
 * consumed, the element runs to the next one, and everything past that
 * separator is what remains.
 */
static VOID
XobDissectName(ANSI_STRING Path, PANSI_STRING First, PANSI_STRING Remaining)
{
    USHORT Start, i = 0;

    RtlZeroMemory(First, sizeof(*First));
    RtlZeroMemory(Remaining, sizeof(*Remaining));
    if (Path.Length == 0)
        return;

    if (Path.Buffer[0] == '\\')
        i = 1;
    for (Start = i; i < Path.Length && Path.Buffer[i] != '\\'; i++)
        ;

    First->Length = First->MaximumLength = i - Start;
    First->Buffer = &Path.Buffer[Start];

    if (i < Path.Length)
    {
        Remaining->Length = Remaining->MaximumLength =
            Path.Length - (USHORT)(i + 1);
        Remaining->Buffer = &Path.Buffer[i + 1];
    }
}

/*
 * The object's own name is the last element of the path in front of it.
 * An empty element between two separators is not one -- the name is
 * refused -- but a separator at either end is absorbed by the split.
 */
static NTSTATUS
XobLeafName(PANSI_STRING Path, PANSI_STRING Leaf)
{
    ANSI_STRING Remaining = *Path;

    RtlZeroMemory(Leaf, sizeof(*Leaf));
    while (Remaining.Length != 0)
    {
        ANSI_STRING Rest;

        XobDissectName(Remaining, Leaf, &Rest);
        if (Rest.Length != 0 && Rest.Buffer[0] == '\\')
            return STATUS_OBJECT_NAME_INVALID;
        Remaining = Rest;
    }
    return Leaf->Length != 0 ? STATUS_SUCCESS : STATUS_OBJECT_NAME_INVALID;
}

/*
 * ObCreateObject: allocate through the type, lay the header down in
 * front of the body, and hand the body back with one reference on it.
 * The object is not in the handle table or the namespace yet -- that is
 * ObInsertObject's half -- so a caller that gets this far and no
 * further releases it with ObfDereferenceObject.
 */
NTSTATUS NTAPI
XeObCreateObject(PVOID Type, PVOID Attributes, ULONG BodySize, PVOID *Object)
{
    /* The Xbox OBJECT_ATTRIBUTES; xbe.c carries the same shape. */
    struct
    {
        HANDLE RootDirectory;
        PANSI_STRING ObjectName;
        ULONG Attributes;
    } *Oa = Attributes;
    XBOX_OBJECT_TYPE *ObjectType = Type;
    PXB_OBJECT_HEADER Header;
    PXB_OBJECT_NAME_INFO NameInfo;
    ANSI_STRING Leaf;
    ULONG Aligned;
    NTSTATUS Status;
    PVOID Base;

    *Object = NULL;
    if (ObjectType == NULL || ObjectType->AllocateProcedure == NULL ||
        ObjectType->FreeProcedure == NULL)
        return STATUS_INVALID_PARAMETER;

    if (Oa == NULL || Oa->ObjectName == NULL)
    {
        Base = ((XB_ALLOCATE_PROCEDURE)ObjectType->AllocateProcedure)(
            sizeof(XB_OBJECT_HEADER) + BodySize, ObjectType->PoolTag);
        if (Base == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;

        Header = Base;
        Header->Flags = 0;
    }
    else
    {
        Status = XobLeafName(Oa->ObjectName, &Leaf);
        if (!NT_SUCCESS(Status))
            return Status;

        Aligned = ALIGN_UP_BY(BodySize, sizeof(ULONG));
        Base = ((XB_ALLOCATE_PROCEDURE)ObjectType->AllocateProcedure)(
            sizeof(XB_OBJECT_NAME_INFO) + sizeof(XB_OBJECT_HEADER) +
                Aligned + Leaf.Length,
            ObjectType->PoolTag);
        if (Base == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;

        NameInfo = Base;
        Header = (PXB_OBJECT_HEADER)(NameInfo + 1);
        NameInfo->ChainLink = NULL;
        NameInfo->Directory = NULL;
        NameInfo->Name.Length = NameInfo->Name.MaximumLength = Leaf.Length;
        NameInfo->Name.Buffer = (PCHAR)(Header + 1) + Aligned;
        RtlCopyMemory(NameInfo->Name.Buffer, Leaf.Buffer, Leaf.Length);
        Header->Flags = XB_OB_FLAG_NAMED_OBJECT;
    }

    Header->PointerCount = 1;
    Header->HandleCount = 0;
    Header->Type = ObjectType;

    XobRememberType(ObjectType);
    *Object = Header + 1;
    return STATUS_SUCCESS;
}

/*
 * ObInsertObject: give the object a handle, and a place in the
 * namespace when the attributes name one.  The reference the create
 * left behind is this call's to give up, so a refused insert is what
 * destroys the object.
 */
NTSTATUS NTAPI
XeObInsertObject(PVOID Object, PVOID Attributes, ULONG PointerBias,
                 PHANDLE Handle)
{
    extern POBJECT_ATTRIBUTES XeTranslateXboxOa(PVOID Xbox,
                                                POBJECT_ATTRIBUTES Nt,
                                                PUNICODE_STRING Name);
    OBJECT_ATTRIBUTES ntoa;
    UNICODE_STRING name;
    POBJECT_ATTRIBUTES oa;
    POBJECT_TYPE ShimType;
    PXB_OBJECT_SHIM Shim;
    PVOID Existing, Target = Object;
    BOOLEAN Named, Permanent;
    NTSTATUS Status;

    *Handle = NULL;

    ShimType = XobGetShimType();
    if (ShimType == NULL)
    {
        XobDereferenceObject(Object);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    oa = XeTranslateXboxOa(Attributes, &ntoa, &name);
    Named = oa != NULL && oa->ObjectName != NULL;
    Permanent = oa != NULL && (oa->Attributes & OBJ_PERMANENT) != 0;

    /* ntoskrnl captures the name into the object, so the translated one
     * goes back as soon as the shim is built. */
    Status = ObCreateObject(KernelMode, ShimType, oa, KernelMode, NULL,
                            sizeof(XB_OBJECT_SHIM), 0, 0, (PVOID *)&Shim);
    if (name.Buffer != NULL)
        RtlFreeUnicodeString(&name);
    if (!NT_SUCCESS(Status))
    {
        XobDereferenceObject(Object);
        return Status;
    }

    /* The shim's own reference on the object it stands for; its delete
     * procedure gives it back, including down every failure path from
     * here on. */
    Shim->Object = Object;
    XobReferenceObject(Object);

    Status = ObInsertObject(Shim, NULL, GENERIC_ALL, 0, NULL, Handle);
    if (!NT_SUCCESS(Status))
    {
        *Handle = NULL;
        XobDereferenceObject(Object);
        return Status;
    }

    /* A name already taken and opened rather than collided with: the
     * handle belongs to the object that was there, so the bias and the
     * handle count are its.  Our own went down with the shim. */
    if (Status == STATUS_OBJECT_NAME_EXISTS &&
        NT_SUCCESS(ObReferenceObjectByHandle(*Handle, 0, ShimType, KernelMode,
                                             &Existing, NULL)))
    {
        Target = ((PXB_OBJECT_SHIM)Existing)->Object;
        ObfDereferenceObject(Existing);
    }

    InterlockedIncrement(&XbHeader(Target)->HandleCount);
    InterlockedExchangeAdd(&XbHeader(Target)->PointerCount, (LONG)PointerBias);
    if (Named)
        XbHeader(Target)->Flags |= XB_OB_FLAG_ATTACHED_OBJECT;
    if (Permanent)
        XbHeader(Target)->Flags |= XB_OB_FLAG_PERMANENT_OBJECT;

    XobDereferenceObject(Object);
    return Status;
}

/*
 * The lookup entries in xbe.c hand a shim here to be unwrapped: the
 * caller asked for its own object, and its own type is what the
 * reference has to be checked against.
 */
BOOLEAN
XobIsShim(PVOID Object)
{
    return XobShimType != NULL &&
           OBJECT_TO_OBJECT_HEADER(Object)->Type == XobShimType;
}

POBJECT_TYPE
XobShimObjectType(VOID)
{
    return XobShimType;
}

NTSTATUS
XobUnwrapShim(PVOID Shim, PVOID Type, PVOID *Object)
{
    PVOID Body = ((PXB_OBJECT_SHIM)Shim)->Object;

    if (Type != NULL && XbHeader(Body)->Type != Type)
    {
        ObfDereferenceObject(Shim);
        *Object = NULL;
        return STATUS_OBJECT_TYPE_MISMATCH;
    }

    XobReferenceObject(Body);
    ObfDereferenceObject(Shim);
    *Object = Body;
    return STATUS_SUCCESS;
}

VOID
XobMakeTemporary(PVOID Object)
{
    XbHeader(Object)->Flags &= ~XB_OB_FLAG_PERMANENT_OBJECT;
}

/*
 * A handle for an object that already has one.  The insert gives up a
 * reference, so one is added first and the caller keeps its own.
 */
NTSTATUS
XobOpenTitleObject(PVOID Object, PVOID Type, PHANDLE Handle)
{
    if (Type != NULL && XbHeader(Object)->Type != Type)
    {
        *Handle = NULL;
        return STATUS_OBJECT_TYPE_MISMATCH;
    }

    XobReferenceObject(Object);
    return XeObInsertObject(Object, NULL, 0, Handle);
}
