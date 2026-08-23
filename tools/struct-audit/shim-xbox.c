/*
 * shim-xbox.c -- single TU that exposes Xbox struct layouts to libclang.
 * No code is emitted; tools/struct-audit/audit.py only walks the AST.
 */
#include <xboxkrnl/xboxkrnl.h>
