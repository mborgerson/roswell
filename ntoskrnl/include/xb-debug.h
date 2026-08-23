/*
 * DbgPrint wrapper that vanishes in release (DBG=0) builds, used for
 * Xbox-side status/trace prints.  Raw DbgPrint compiles in unconditionally,
 * so the XbDbg(...) wrapper is the way to get a quiet release without
 * sprinkling #if DBG around every call site.
 *
 * Use raw DbgPrint only for messages that must print even in release --
 * currently none in our tree.
 */

#ifndef _XB_DEBUG_H_
#define _XB_DEBUG_H_

#if DBG
#define XbDbg(...) DbgPrint(__VA_ARGS__)
#else
#define XbDbg(...) ((void)0)
#endif

#endif /* _XB_DEBUG_H_ */
