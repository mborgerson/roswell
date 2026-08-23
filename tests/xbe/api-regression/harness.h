#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <xboxkrnl/xboxkrnl.h>
#include "tap.h"

typedef bool (*test_fn_t)(void);

typedef struct {
    const char *name;
    test_fn_t fn;
    /* Non-NULL marks a known-failing test (TAP "# TODO <reason>"): it
     * runs and reports, but does not fail the suite.  Use for asserted
     * retail behavior the kernel does not implement yet. */
    const char *todo;
} test_entry_t;

typedef struct {
    const char *group;
    const test_entry_t *entries;
    size_t count;
} test_group_t;

/* Each test sets these on failure and returns false. main.c reads them
 * to emit a YAML diagnostic block after the `not ok` line. */
extern const char *g_fail_file;
extern int g_fail_line;
extern char g_fail_what[160];

void test_record_failure(const char *file, int line, const char *fmt, ...);

#define FAIL_AND_RETURN(fmt, ...) do { \
    test_record_failure(__FILE__, __LINE__, fmt, ##__VA_ARGS__); \
    return false; \
} while (0)

#define ASSERT_TRUE(cond) do { \
    if (!(cond)) FAIL_AND_RETURN("assertion failed: %s", #cond); \
} while (0)

#define ASSERT_EQ_U32(actual, expected) do { \
    uint32_t _a = (uint32_t)(actual); \
    uint32_t _e = (uint32_t)(expected); \
    if (_a != _e) FAIL_AND_RETURN("%s: got 0x%08x expected 0x%08x", \
                                  #actual, (unsigned)_a, (unsigned)_e); \
} while (0)

#define ASSERT_EQ_PTR(actual, expected) do { \
    const void *_a = (const void *)(actual); \
    const void *_e = (const void *)(expected); \
    if (_a != _e) FAIL_AND_RETURN("%s: got %p expected %p", \
                                  #actual, _a, _e); \
} while (0)

#define ASSERT_NOT_NULL(p) do { \
    if ((p) == NULL) FAIL_AND_RETURN("%s is NULL", #p); \
} while (0)

#define ASSERT_NTSTATUS(actual, expected) do { \
    NTSTATUS _a = (actual); \
    NTSTATUS _e = (expected); \
    if (_a != _e) FAIL_AND_RETURN("%s: got 0x%08x expected 0x%08x", \
                                  #actual, (unsigned)_a, (unsigned)_e); \
} while (0)

/* Each group's .c file defines `g_group_<name>` of type test_group_t. */
#define DECLARE_GROUP(name) extern const test_group_t g_group_##name
#define DEFINE_GROUP(name, label) \
    const test_group_t g_group_##name = { \
        .group = (label), \
        .entries = name##_entries, \
        .count = sizeof(name##_entries)/sizeof(name##_entries[0]), \
    }
