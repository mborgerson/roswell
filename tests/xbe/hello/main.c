/*
 * nxkrnl test XBE -- minimal real-nxdk title.
 *
 * Built with the real nxdk C runtime, so it exercises the full nxdk
 * startup path: TLS setup, __security_init_cookie, and thrd_create
 * spawning the main thread (the XBE entry point returns immediately;
 * main() runs on the spawned thread).
 *
 * The body is deliberately trivial -- a kernel DbgPrint, visible on the LPC
 * serial port -- so what is under test is the loader (drivers/nxbe): TLS/TEB
 * setup, thread-spawn handling, and the xboxkrnl ordinal table.
 */
#include <xboxkrnl/xboxkrnl.h>

int main(void)
{
    DbgPrint("hello from a real nxdk XBE\n");
    return 0;
}
