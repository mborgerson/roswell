/*
 * Xbox-shape OBJECT_TYPE decoy structs (storage in object-types.c).
 *
 * Titles import these ordinals as DATA and read fields directly; kernel
 * adapters translate a title-passed decoy pointer back to the ReactOS
 * internal POBJECT_TYPE (see XeObjectTypeToInternal in xbe.c).
 */

#pragma once

typedef struct _XBOX_OBJECT_TYPE
{
    PVOID AllocateProcedure;
    PVOID FreeProcedure;
    PVOID CloseProcedure;
    PVOID DeleteProcedure;
    PVOID ParseProcedure;
    PVOID DefaultObject;
    ULONG PoolTag;
} XBOX_OBJECT_TYPE;

extern XBOX_OBJECT_TYPE XePsThreadObjectType;
extern XBOX_OBJECT_TYPE XeExEventObjectType;
extern XBOX_OBJECT_TYPE XeExSemaphoreObjectType;
extern XBOX_OBJECT_TYPE XeExMutantObjectType;
extern XBOX_OBJECT_TYPE XeExTimerObjectType;
extern XBOX_OBJECT_TYPE XeIoFileObjectType;
extern XBOX_OBJECT_TYPE XeIoDeviceObjectType;
extern XBOX_OBJECT_TYPE XeIoCompletionObjectType;
extern XBOX_OBJECT_TYPE XeObDirectoryObjectType;
