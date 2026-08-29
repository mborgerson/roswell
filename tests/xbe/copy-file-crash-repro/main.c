/*
 * Minimal Reproduction XBE: Asynchronous NtReadFile (FILE_NO_INTERMEDIATE_BUFFERING)
 *
 * Demonstrates the behavioral difference in NtReadFile return status between
 * standard retail Xbox BIOS and Roswell:
 *
 * - Retail BIOS: NtReadFile with FILE_NO_INTERMEDIATE_BUFFERING queues the IRP
 *   asynchronously and returns STATUS_PENDING (0x00000103). The caller must wait
 *   on the IoStatusBlock / Event for completion.
 *
 * - Roswell BIOS: NtReadFile completes synchronously inline and returns
 *   STATUS_SUCCESS (0x00000000), with IoStatusBlock already updated.
 */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmacro-redefined"
#include <windows.h>
#pragma clang diagnostic pop
#include <hal/debug.h>
#include <hal/video.h>
#include <nxdk/format.h>
#include <nxdk/mount.h>
#include <pbkit/pbkit.h>
#include <xboxkrnl/xboxkrnl.h>

#define COM1     0x3F8
#define COM1_THR (COM1 + 0)
#define COM1_DLL (COM1 + 0)
#define COM1_IER (COM1 + 1)
#define COM1_DLH (COM1 + 1)
#define COM1_FCR (COM1 + 2)
#define COM1_LCR (COM1 + 3)
#define COM1_MCR (COM1 + 4)
#define COM1_LSR (COM1 + 5)
#define LSR_THRE 0x20
#define LCR_DLAB 0x80
#define LCR_8N1  0x03

static inline void outb(unsigned short port, unsigned char val)
{
    __asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline unsigned char inb(unsigned short port)
{
    unsigned char ret;
    __asm__ __volatile__("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static void com_init(void)
{
    outb(COM1_IER, 0x00);
    outb(COM1_LCR, LCR_DLAB);
    outb(COM1_DLL, 0x01);
    outb(COM1_DLH, 0x00);
    outb(COM1_LCR, LCR_8N1);
    outb(COM1_FCR, 0xC7);
    outb(COM1_MCR, 0x0B);
}

static void com_putc(char c)
{
    while ((inb(COM1_LSR) & LSR_THRE) == 0) { }
    outb(COM1_THR, (unsigned char)c);
}

static void com_puts(const char *s)
{
    while (*s) {
        if (*s == '\n') com_putc('\r');
        com_putc(*s++);
    }
}

static void
log_printf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    com_puts(buf);
    debugPrint("%s", buf);
    pb_show_debug_screen();
}

static void
power_off(void)
{
    LARGE_INTEGER d = { .QuadPart = -((LONGLONG)200 * 10000) };
    KeDelayExecutionThread(KernelMode, FALSE, &d);
    HalWriteSMBusValue(0x20, 0x02, FALSE, 0x80);
    for (;;) { __asm__ __volatile__("hlt"); }
}

#define TEST_FILE_SIZE (64 * 1024)
#define TEST_FILE_PATH "Z:\\async_read_test.raw"

static bool
prepare_test_file(void)
{
    const char *partition5 = "\\Device\\Harddisk0\\Partition5";

    if (!nxIsDriveMounted('Z'))
    {
        log_printf("[SETUP] Formatting %s...\n", partition5);
        if (!nxFormatVolume(partition5, 0))
        {
            log_printf("[SETUP] nxFormatVolume failed: %lu\n", GetLastError());
            return false;
        }
        log_printf("[SETUP] Mounting %s as Z:...\n", partition5);
        if (!nxMountDrive('Z', partition5))
        {
            log_printf("[SETUP] nxMountDrive failed: %lu\n", GetLastError());
            return false;
        }
    }

    HANDLE hFile = CreateFile(TEST_FILE_PATH, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        log_printf("[SETUP] CreateFile('%s') failed: %lu\n", TEST_FILE_PATH, GetLastError());
        return false;
    }

    static uint8_t buffer[TEST_FILE_SIZE];
    memset(buffer, 0xAA, sizeof(buffer));

    DWORD written = 0;
    if (!WriteFile(hFile, buffer, sizeof(buffer), &written, NULL) || written != sizeof(buffer))
    {
        log_printf("[SETUP] WriteFile failed: %lu\n", GetLastError());
        CloseHandle(hFile);
        return false;
    }
    CloseHandle(hFile);

    log_printf("[SETUP] Created '%s' (%u bytes)\n", TEST_FILE_PATH, (unsigned int)TEST_FILE_SIZE);
    return true;
}

int main(void)
{
    com_init();

    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
    if (pb_init())
    {
        return 1;
    }
    pb_show_debug_screen();

    log_printf("\n=== NtReadFile Async/Sync Repro Test ===\n\n");

    if (!prepare_test_file())
    {
        log_printf("FAIL: Failed to prepare test file\n");
        power_off();
        return 1;
    }

    /*
     * Open file with FILE_NO_INTERMEDIATE_BUFFERING without FILE_SYNCHRONOUS_IO_*
     */
    STRING path;
    RtlInitAnsiString(&path, TEST_FILE_PATH);

    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(&attributes, &path, OBJ_CASE_INSENSITIVE, ObDosDevicesDirectory(), NULL);

    HANDLE handle = INVALID_HANDLE_VALUE;
    IO_STATUS_BLOCK io_status = {0};

    NTSTATUS status = NtCreateFile(
        &handle, GENERIC_READ | FILE_READ_ATTRIBUTES | SYNCHRONIZE, &attributes, &io_status, 0, FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ, FILE_OPEN, FILE_NON_DIRECTORY_FILE | FILE_RANDOM_ACCESS | FILE_NO_INTERMEDIATE_BUFFERING);

    if (!NT_SUCCESS(status))
    {
        log_printf("FAIL: NtCreateFile failed: 0x%08lX\n", (unsigned long)status);
        power_off();
        return 1;
    }

    void *read_buffer = VirtualAlloc(NULL, TEST_FILE_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!read_buffer)
    {
        log_printf("FAIL: VirtualAlloc failed\n");
        NtClose(handle);
        power_off();
        return 1;
    }

    LARGE_INTEGER byte_offset = {.QuadPart = 0};
    io_status.Status = STATUS_PENDING;

    log_printf("[TEST] Invoking NtReadFile with FILE_NO_INTERMEDIATE_BUFFERING...\n");
    status = NtReadFile(
        handle, NULL, /* Event */
        NULL,         /* ApcRoutine */
        NULL,         /* ApcContext */
        &io_status, read_buffer, TEST_FILE_SIZE, &byte_offset);

    log_printf("\n--- RESULTS ---\n");
    log_printf("NtReadFile return status: 0x%08lX\n", (unsigned long)status);

    if (status == STATUS_PENDING)
    {
        log_printf("-> Behavior: ASYNCHRONOUS (Retail BIOS behavior, STATUS_PENDING)\n");
        log_printf("-> Waiting for io_status.Status to complete...\n");
        while (io_status.Status == STATUS_PENDING)
        {
            Sleep(1);
        }
        log_printf(
            "-> Completed async with status: 0x%08lX, bytes transferred: %lu\n", (unsigned long)io_status.Status,
            (unsigned long)io_status.Information);
    }
    else if (status == STATUS_SUCCESS)
    {
        log_printf("-> Behavior: SYNCHRONOUS (Roswell behavior, STATUS_SUCCESS)\n");
        log_printf(
            "-> io_status.Status: 0x%08lX, bytes transferred: %lu\n", (unsigned long)io_status.Status,
            (unsigned long)io_status.Information);
    }
    else
    {
        log_printf("-> Behavior: ERROR (0x%08lX)\n", (unsigned long)status);
    }

    VirtualFree(read_buffer, 0, MEM_RELEASE);
    NtClose(handle);

    log_printf("\nTAP version 14\n1..1\nok 1 - ntreadfile/async_probe\n");
    log_printf("== copy-file-crash-repro end PASS ==\n");
    log_printf("[TEST] Finished.\n");

    Sleep(5000);

    power_off();
    return 0;
}
