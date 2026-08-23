/*
 * shim-nt.c -- single TU that exposes ReactOS NT struct layouts to libclang.
 * Same headers xbe.c sees (see CMakeLists for xbe).  No code is emitted;
 * tools/struct-audit/audit.py only walks the AST.
 *
 * mmintrin.h's __m64 intrinsics confuse libclang in -m32 mode; the
 * headers themselves only forward-declare __m64, so we suppress the
 * include by pre-defining its guard.  None of the audited types depend
 * on MMX intrinsics.
 */
#define _MMINTRIN_H_INCLUDED
#define _INCLUDED_MM2
#define _INCLUDED_EMM
#include <ntifs.h>
#include <ntimage.h>
