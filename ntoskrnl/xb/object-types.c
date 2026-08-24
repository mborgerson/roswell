/*
 * Xbox-shape OBJECT_TYPE export storage.
 *
 * The Xbox public OBJECT_TYPE is a 28-byte struct ending in a 4-byte
 * PoolTag at +0x18 (see xboxkrnl.h).  ReactOS's internal OBJECT_TYPE is
 * much larger and reached via a POBJECT_TYPE pointer (e.g. PsThreadType,
 * ExEventObjectType).  Titles import the ordinal as a DATA symbol and
 * read offsets through it directly, so we must expose a real 28-byte
 * struct -- the pointer-shaped scaffold leaks adjacent .data bytes at
 * +0x18.  PoolTag is the only field titles actually read; the
 * procedures stay NULL (retail leaves several of them NULL too).
 */

#include <ntdef.h>
#include "object-types.h"

#define XB_OT(tag) { NULL, NULL, NULL, NULL, NULL, NULL, (tag) }

/* Tags chosen to match what `!poolused` style debuggers expect on NT --
 * 4-char little-endian-printed mnemonic of the type name. */
XBOX_OBJECT_TYPE XePsThreadObjectType   = XB_OT('erhT');
XBOX_OBJECT_TYPE XeExEventObjectType    = XB_OT('tnvE');
XBOX_OBJECT_TYPE XeExSemaphoreObjectType = XB_OT('ameS');
XBOX_OBJECT_TYPE XeExMutantObjectType   = XB_OT('atuM');
XBOX_OBJECT_TYPE XeIoFileObjectType     = XB_OT('eliF');
XBOX_OBJECT_TYPE XeIoDeviceObjectType   = XB_OT('iveD');
XBOX_OBJECT_TYPE XeObDirectoryObjectType = XB_OT('eriD');
