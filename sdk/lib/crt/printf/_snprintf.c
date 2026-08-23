/*
 * COPYRIGHT:       GNU GPL, see COPYING in the top level directory
 * PROJECT:         ReactOS crt library
 * FILE:            lib/sdk/crt/printf/_snprintf.c
 * PURPOSE:         Implementation of _snprintf
 * PROGRAMMER:      Timo Kreuzer
 */

#define _sxprintf _snprintf
#define USE_COUNT 1

#ifdef SARCH_XBOX
/* Referenced only by boot-time device-tree setup. */
#include <stddef.h>
#include <section_attribs.h>
int __cdecl _snprintf(char *buffer, size_t count, const char *format, ...);
#endif

#include "_sxprintf.c"
