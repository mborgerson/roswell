/* Single-TU build of nanoprintf for the loader.  Disable everything we
 * don't need so the impl shrinks: no floats, no large/binary specifiers,
 * no writeback.  Strip down to int/hex/string/char + field width. */

#define NANOPRINTF_USE_FIELD_WIDTH_FORMAT_SPECIFIERS 1
#define NANOPRINTF_USE_PRECISION_FORMAT_SPECIFIERS   0
#define NANOPRINTF_USE_FLOAT_FORMAT_SPECIFIERS       0
#define NANOPRINTF_USE_LARGE_FORMAT_SPECIFIERS       0
#define NANOPRINTF_USE_SMALL_FORMAT_SPECIFIERS       0
#define NANOPRINTF_USE_BINARY_FORMAT_SPECIFIERS      0
#define NANOPRINTF_USE_WRITEBACK_FORMAT_SPECIFIERS   0
#define NANOPRINTF_USE_ALT_FORM_FLAG                 1

#define NANOPRINTF_IMPLEMENTATION
#include "nanoprintf.h"
