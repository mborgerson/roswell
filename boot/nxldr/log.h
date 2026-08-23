/*
 * DBG-gated debug logger.  Release builds compile every call out to a
 * statement-expression no-op so the loader runs silent.  DBG builds route
 * formatted output through serial.c -> nanoprintf -> COM1.
 */

#ifndef NXLDR_LOG_H
#define NXLDR_LOG_H

#include "nxldr.h"

#if DBG

/* Implemented in serial.c.  Linked only in DBG builds. */
__attribute__((format(printf, 1, 2))) void
Log(const char *Format, ...);
void
LogInit(void);
void
LogDrain(void);

#define dprintf(...) Log(__VA_ARGS__)
#define DBG_SERIAL_INIT() LogInit()
#define DBG_SERIAL_DRAIN() LogDrain()

#else /* !DBG */

#define dprintf(...) ((void)0)
#define DBG_SERIAL_INIT() ((void)0)
#define DBG_SERIAL_DRAIN() ((void)0)

#endif /* DBG */

#endif /* NXLDR_LOG_H */
