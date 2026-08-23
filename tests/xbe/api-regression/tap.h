#pragma once

#include <stdarg.h>
#include <stdbool.h>

void tap_version(unsigned int v);
void tap_plan(unsigned int n);
void tap_ok(unsigned int n, const char *desc);
void tap_not_ok(unsigned int n, const char *desc);

/* Open / close a YAML diagnostic block underneath a `not ok` line. */
void tap_diag_begin(void);
void tap_diag_end(void);
void tap_diag_kv(const char *key, const char *fmt, ...);

void tap_comment(const char *fmt, ...);

/* Drop a raw string verbatim (no formatting / framing).  Used for the
 * "== api-regression begin ==" / "== ... end ==" markers in main.c so
 * the runner can slice the TAP block out of the surrounding kernel
 * debug stream. */
void tap_puts(const char *s);

/* Block until the UART transmitter is fully drained.  Call before
 * issuing an SMC shutdown so the final bytes aren't lost. */
void tap_drain(void);
