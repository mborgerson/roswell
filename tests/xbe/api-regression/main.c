/*
 * Kernel API regression suite driver. Each test_*.c file exports a
 * test_group_t named g_group_<name>. This file enumerates them, emits
 * a TAP 14 stream, and reboots when done.
 */

#include "harness.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

const char *g_fail_file;
int          g_fail_line;
char         g_fail_what[160];

void test_record_failure(const char *file, int line, const char *fmt, ...)
{
    g_fail_file = file;
    g_fail_line = line;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_fail_what, sizeof(g_fail_what), fmt, ap);
    va_end(ap);
}

DECLARE_GROUP(rtl_status);
DECLARE_GROUP(rtl_string);
DECLARE_GROUP(rtl_time);
DECLARE_GROUP(rtl_memory);
DECLARE_GROUP(rtl_strconv);
DECLARE_GROUP(rtl_intconv);
DECLARE_GROUP(rtl_nlscase);
DECLARE_GROUP(rtl_printf);
DECLARE_GROUP(rtl_critsec);
DECLARE_GROUP(rtl_byteswap);
DECLARE_GROUP(rtl_framewalk);
DECLARE_GROUP(ke_event);
DECLARE_GROUP(ke_semaphore);
DECLARE_GROUP(ke_time);
DECLARE_GROUP(ke_timebase);
DECLARE_GROUP(ex_pool);
DECLARE_GROUP(ex_raise);
DECLARE_GROUP(ex_rwlock);
DECLARE_GROUP(ex_interlocked);
DECLARE_GROUP(ex_list);
DECLARE_GROUP(mm_contig);
DECLARE_GROUP(io_file);
DECLARE_GROUP(io_dvd);
DECLARE_GROUP(hal_data);
DECLARE_GROUP(hal_portio);
DECLARE_GROUP(ob_types);
DECLARE_GROUP(ps_thread);
DECLARE_GROUP(ke_wait_apc);
DECLARE_GROUP(mm_virtual);
DECLARE_GROUP(io_dirmask);
DECLARE_GROUP(ob_named);
DECLARE_GROUP(io_bad_ptr);
DECLARE_GROUP(mm_bigpool);
DECLARE_GROUP(io_cc_write);
DECLARE_GROUP(io_storage);
DECLARE_GROUP(io_complete);
DECLARE_GROUP(xe_sections);
DECLARE_GROUP(mm_vmcontract);
DECLARE_GROUP(mm_cache);
DECLARE_GROUP(io_mdl);
DECLARE_GROUP(io_fileobj);
DECLARE_GROUP(io_shareaccess);
DECLARE_GROUP(io_irplayout);
DECLARE_GROUP(io_fsdreq);
DECLARE_GROUP(io_syncreq);
DECLARE_GROUP(mm_pressure);
DECLARE_GROUP(ke_waitmulti);
DECLARE_GROUP(ob_symlink);
DECLARE_GROUP(io_finfo);
DECLARE_GROUP(io_rawfs);
DECLARE_GROUP(hal_av);
DECLARE_GROUP(kd_flags);
DECLARE_GROUP(ke_exceptions);
DECLARE_GROUP(io_fatx16);
DECLARE_GROUP(ob_handles);
DECLARE_GROUP(xc_parity);
DECLARE_GROUP(xc_crypto);
DECLARE_GROUP(xc_rsa);
DECLARE_GROUP(xc_cipher);
DECLARE_GROUP(xc_vector);
DECLARE_GROUP(io_delete);
DECLARE_GROUP(ob_refname);
DECLARE_GROUP(ke_devqueue);
DECLARE_GROUP(ke_queue);
DECLARE_GROUP(io_irpalloc);
DECLARE_GROUP(io_devlife);
DECLARE_GROUP(io_asyncreq);
DECLARE_GROUP(io_scatter);
DECLARE_GROUP(io_iocomp);
DECLARE_GROUP(mm_protect);
DECLARE_GROUP(ob_query);
DECLARE_GROUP(ke_apc);
DECLARE_GROUP(io_dismount);
DECLARE_GROUP(ke_threadstate);
DECLARE_GROUP(ke_apcinit);
DECLARE_GROUP(ke_apcqueue);
DECLARE_GROUP(io_ioquery);
DECLARE_GROUP(io_volquery);
DECLARE_GROUP(io_startio);
DECLARE_GROUP(io_createfile);
DECLARE_GROUP(io_threadirp);
DECLARE_GROUP(xc_align);
DECLARE_GROUP(ex_timer);
DECLARE_GROUP(ob_bypointer);
DECLARE_GROUP(ke_pulse);

static const test_group_t *const GROUPS[] = {
    &g_group_rtl_status,
    &g_group_rtl_string,
    &g_group_rtl_time,
    &g_group_rtl_memory,
    &g_group_rtl_strconv,
    &g_group_rtl_intconv,
    &g_group_rtl_nlscase,
    &g_group_rtl_printf,
    &g_group_rtl_critsec,
    &g_group_rtl_byteswap,
    &g_group_rtl_framewalk,
    &g_group_ke_event,
    &g_group_ke_semaphore,
    &g_group_ke_time,
    &g_group_ke_timebase,
    &g_group_ex_pool,
    &g_group_ex_raise,
    &g_group_ex_rwlock,
    &g_group_ex_interlocked,
    &g_group_ex_list,
    &g_group_mm_contig,
    &g_group_io_file,
    &g_group_io_dvd,
    &g_group_hal_data,
    &g_group_hal_portio,
    &g_group_ob_types,
    &g_group_ps_thread,
    &g_group_ke_wait_apc,
    &g_group_mm_virtual,
    &g_group_io_dirmask,
    &g_group_ob_named,
    &g_group_io_bad_ptr,
    &g_group_mm_bigpool,
    &g_group_io_cc_write,
    &g_group_io_storage,
    &g_group_io_complete,
    &g_group_xe_sections,
    &g_group_mm_vmcontract,
    &g_group_mm_cache,
    &g_group_io_mdl,
    &g_group_io_fileobj,
    &g_group_io_shareaccess,
    &g_group_io_irplayout,
    &g_group_io_fsdreq,
    &g_group_io_syncreq,
    &g_group_ke_waitmulti,
    &g_group_ob_symlink,
    &g_group_io_finfo,
    &g_group_io_rawfs,
    &g_group_ke_exceptions,
    &g_group_io_fatx16,
    &g_group_ob_handles,
    &g_group_xc_parity,
    &g_group_xc_crypto,
    &g_group_xc_rsa,
    &g_group_xc_cipher,
    &g_group_xc_vector,
    &g_group_io_delete,
    &g_group_ob_refname,
    &g_group_ke_devqueue,
    &g_group_ke_queue,
    &g_group_io_irpalloc,
    &g_group_io_devlife,
    &g_group_io_asyncreq,
    &g_group_io_scatter,
    &g_group_io_iocomp,
    &g_group_mm_protect,
    &g_group_ob_query,
    &g_group_ke_apc,
    &g_group_ke_threadstate,
    &g_group_ke_apcinit,
    &g_group_ke_apcqueue,
    &g_group_io_ioquery,
    &g_group_io_volquery,
    &g_group_io_startio,
    &g_group_io_createfile,
    &g_group_io_threadirp,
    &g_group_ob_bypointer,
    &g_group_ke_pulse,
    &g_group_ex_timer,
    /* Allocates a permanent framebuffer, so keep it after the FS/IO
     * groups but before the memory-exhaustion finale. */
    &g_group_kd_flags,
    &g_group_hal_av,
    /* Last: exhausts and recovers all of memory. */
    &g_group_mm_pressure,
    /* Last of all: takes the cache volume down and back up. */
    &g_group_io_dismount,
    /* After that: a call the console can be made to fault on. */
    &g_group_xc_align,
};

#define NGROUPS (sizeof(GROUPS)/sizeof(GROUPS[0]))

static unsigned int total_tests(void)
{
    unsigned int n = 0;
    for (unsigned int i = 0; i < NGROUPS; i++) n += (unsigned int)GROUPS[i]->count;
    return n;
}

int main(void)
{
    /* Bracket the TAP stream so the runner can find start/end inside
     * any kernel-side serial noise. */
    tap_puts("== api-regression begin ==\n");

    tap_version(14);
    tap_plan(total_tests());

    unsigned int idx = 0;
    unsigned int passed = 0;
    unsigned int failed = 0;

    for (unsigned int g = 0; g < NGROUPS; g++) {
        const test_group_t *grp = GROUPS[g];
        for (unsigned int t = 0; t < grp->count; t++) {
            idx++;
            char desc[128];
            snprintf(desc, sizeof(desc), "%s/%s",
                     grp->group, grp->entries[t].name);

            g_fail_file = NULL;
            g_fail_line = 0;
            g_fail_what[0] = 0;

            bool ok = grp->entries[t].fn();
            const char *todo = grp->entries[t].todo;
            if (todo != NULL) {
                /* Known gap: report, never fail the suite.  An
                 * unexpected pass is worth seeing too. */
                char tdesc[192];
                snprintf(tdesc, sizeof(tdesc), "%s # TODO %s%s", desc,
                         ok ? "unexpectedly passing: " : "", todo);
                if (ok) tap_ok(idx, tdesc);
                else {
                    tap_not_ok(idx, tdesc);
                    tap_diag_begin();
                    if (g_fail_file) tap_diag_kv("at", "%s:%d", g_fail_file, g_fail_line);
                    if (g_fail_what[0]) tap_diag_kv("message", "%s", g_fail_what);
                    tap_diag_end();
                }
                passed++;  /* counted as not-failing either way */
            } else if (ok) {
                tap_ok(idx, desc);
                passed++;
            } else {
                tap_not_ok(idx, desc);
                tap_diag_begin();
                if (g_fail_file) tap_diag_kv("at", "%s:%d", g_fail_file, g_fail_line);
                if (g_fail_what[0]) tap_diag_kv("message", "%s", g_fail_what);
                tap_diag_end();
                failed++;
            }
        }
    }

    tap_comment("tests=%u passed=%u failed=%u", idx, passed, failed);
    tap_puts(failed == 0 ? "== api-regression end PASS ==\n"
                         : "== api-regression end FAIL ==\n");
    tap_drain();

    /* Tell the SMC to power off: register 0x02, bit 0x80 (SHUTDOWN).
     * xemu maps this to qemu_system_shutdown_request, which exits the
     * VM cleanly under both retail and nxkrnl. (HalReturnToFirmware
     * with HalHaltRoutine differs across the two -- retail goes to SMC
     * RESET when given the "halt" code, so the box reboots and the
     * DVD title boots all over again.) */
    LARGE_INTEGER d = { .QuadPart = -((LONGLONG)500 * 10000) };
    KeDelayExecutionThread(KernelMode, FALSE, &d);
    HalWriteSMBusValue(0x20, 0x02, FALSE, 0x80);
    /* If the SMC write somehow didn't take, fall back to a busy loop so
     * the runner's timeout still kills xemu. */
    for (;;) { __asm__ __volatile__("hlt"); }
    return 0;
}
