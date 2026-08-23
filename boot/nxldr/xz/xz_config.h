/* SPDX-License-Identifier: 0BSD */

/*
 * Private includes and definitions for the freestanding nxldr build of
 * XZ Embedded.  Derived from the upstream userspace/xz_config.h; the
 * libc pieces are replaced with nxldr's own memory helpers and a static
 * bump arena (the loader runs with no allocator; the decoder state is
 * ~32 KB allocated once per boot and never freed).
 */

#ifndef XZ_CONFIG_H
#define XZ_CONFIG_H

/* The flash payload is produced by `xz --x86 --check=crc32`; only the
 * x86 BCJ filter decoder is needed. */
#define XZ_DEC_X86

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xz.h"

/* nxldr's mem helpers (loader.c / nxldr.h). */
void *memcpy(void *dst, const void *src, uint32_t n);
void *memset(void *dst, int c, uint32_t n);
void *memmove(void *dst, const void *src, uint32_t n);
int memcmp(const void *a, const void *b, uint32_t n);

/* One-shot bump allocator backed by a static arena in .bss; see
 * loader.c.  Nothing is ever freed -- the decoder lives for the single
 * decompression and the loader hands off to the kernel right after. */
void *xz_arena_alloc(unsigned long size);
#define kmalloc(size, flags) xz_arena_alloc(size)
#define kfree(ptr) ((void)(ptr))
#define vmalloc(size) xz_arena_alloc(size)
#define vfree(ptr) ((void)(ptr))

#define memeq(a, b, size) (memcmp(a, b, size) == 0)
#define memzero(buf, size) memset(buf, 0, size)

#ifndef min
#	define min(x, y) ((x) < (y) ? (x) : (y))
#endif
#define min_t(type, x, y) min(x, y)

#ifndef fallthrough
#	if (defined(__GNUC__) && __GNUC__ >= 7)
#		define fallthrough __attribute__((__fallthrough__))
#	else
#		define fallthrough do {} while (0)
#	endif
#endif

#ifndef __always_inline
#	ifdef __GNUC__
#		define __always_inline \
			inline __attribute__((__always_inline__))
#	else
#		define __always_inline inline
#	endif
#endif

/* Inline functions to access unaligned unsigned 32-bit integers */
#ifndef get_unaligned_le32
static inline uint32_t get_unaligned_le32(const uint8_t *buf)
{
	return (uint32_t)buf[0]
			| ((uint32_t)buf[1] << 8)
			| ((uint32_t)buf[2] << 16)
			| ((uint32_t)buf[3] << 24);
}
#endif

#ifndef get_unaligned_be32
static inline uint32_t get_unaligned_be32(const uint8_t *buf)
{
	return (uint32_t)((uint32_t)buf[0] << 24)
			| ((uint32_t)buf[1] << 16)
			| ((uint32_t)buf[2] << 8)
			| (uint32_t)buf[3];
}
#endif

#ifndef put_unaligned_le32
static inline void put_unaligned_le32(uint32_t val, uint8_t *buf)
{
	buf[0] = (uint8_t)val;
	buf[1] = (uint8_t)(val >> 8);
	buf[2] = (uint8_t)(val >> 16);
	buf[3] = (uint8_t)(val >> 24);
}
#endif

#ifndef put_unaligned_be32
static inline void put_unaligned_be32(uint32_t val, uint8_t *buf)
{
	buf[0] = (uint8_t)(val >> 24);
	buf[1] = (uint8_t)(val >> 16);
	buf[2] = (uint8_t)(val >> 8);
	buf[3] = (uint8_t)val;
}
#endif

/*
 * Use get_unaligned_le32() also for aligned access for simplicity. On
 * little endian systems, #define get_le32(ptr) (*(const uint32_t *)(ptr))
 * could save a few bytes in code size.
 */
#ifndef get_le32
#	define get_le32 get_unaligned_le32
#endif

#endif
