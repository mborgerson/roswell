/*
 * Objects a title defines and owns the storage for (obcreate.c).
 *
 * The object manager entries in xbe.c call in here to tell a title
 * object from one of ntoskrnl's, and to unwrap the shim object that
 * carries one through the handle table and the namespace.
 */

#pragma once

/* TRUE once a title has created an object of its own; every check
 * below is behind this one so nothing else pays for the feature. */
BOOLEAN XobHasTitleObjects(VOID);

BOOLEAN XobIsTitleObject(PVOID Object);
PVOID XobObjectType(PVOID Object);
BOOLEAN XobIsTitleType(PVOID Type);
BOOLEAN XobIsShim(PVOID Object);
POBJECT_TYPE XobShimObjectType(VOID);

/* Consumes the caller's reference on the shim and hands back the title
 * object it stands for, referenced, once the type matches. */
NTSTATUS XobUnwrapShim(PVOID Shim, PVOID Type, PVOID *Object);

VOID FASTCALL XobReferenceObject(PVOID Object);
VOID FASTCALL XobDereferenceObject(PVOID Object);
VOID XobMakeTemporary(PVOID Object);
NTSTATUS XobOpenTitleObject(PVOID Object, PVOID Type, PHANDLE Handle);

NTSTATUS NTAPI XeObCreateObject(PVOID Type, PVOID Attributes,
                                ULONG BodySize, PVOID *Object);
NTSTATUS NTAPI XeObInsertObject(PVOID Object, PVOID Attributes,
                                ULONG PointerBias, PHANDLE Handle);
