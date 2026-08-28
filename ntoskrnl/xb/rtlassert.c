/*
 * PROJECT:     nxkrnl -- a free kernel for the original Xbox
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The two failure reporters a title can call.
 *
 * Both print what they were handed and then break, which is all a
 * caller can observe: with nothing attached the break comes back as an
 * exception, and whoever handles it decides what happens next.
 *
 * ntoskrnl's own RtlAssert is not the shape wanted here -- it prompts
 * the debugger for what to do and loops until it gets an answer, so
 * with no debugger listening it never returns.
 */

#include <ntddk.h>

VOID NTAPI
XeRtlAssert(PVOID FailedAssertion, PVOID FileName, ULONG LineNumber,
            PCHAR Message)
{
    DbgPrint("\n*** Assertion failed: %s%s\n"
             "***   Source File: %s, line %lu\n\n",
             Message != NULL ? Message : "",
             FailedAssertion != NULL ? (PSTR)FailedAssertion : "",
             FileName != NULL ? (PSTR)FileName : "",
             LineNumber);
    DbgBreakPoint();
}

VOID NTAPI
XeRtlRip(PVOID ApiName, PVOID Expression, PVOID Message)
{
    DbgPrint("\n*** RIP in %s: %s\n***   %s\n\n",
             ApiName != NULL ? (PSTR)ApiName : "",
             Expression != NULL ? (PSTR)Expression : "",
             Message != NULL ? (PSTR)Message : "");
    DbgBreakPoint();
}
