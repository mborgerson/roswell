/*
 * Handle-close notification for the NtPulseEvent handle cache in xb/xbe.c.
 *
 * The cache maps a handle value to the KEVENT behind it so NtPulseEvent can
 * run at DISPATCH_LEVEL without Ob.  Handle values are recycled, so every
 * close has to drop the entry -- hooking one API (NtClose) misses the rest,
 * so the hook lives at the handle-table choke point instead.
 */

#ifndef _XB_EVENT_CACHE_H_
#define _XB_EVENT_CACHE_H_

VOID NTAPI XeUntrackEvent(HANDLE Handle);

#endif /* _XB_EVENT_CACHE_H_ */
