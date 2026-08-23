
add_definitions(
    -D_NTOSKRNL_
    -DNO_RTL_INLINES
    -D_NTSYSTEM_
    -D_NTDLLBUILD_)

if (CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID STREQUAL "Clang")
    # Enable this again. CORE-17637
    add_compile_options(-Wunused-result)

    add_compile_options(-Wno-incompatible-pointer-types)
    add_compile_options(-Wno-missing-braces)
endif()

list(APPEND RTL_WINE_SOURCE
    timerqueue.c
)
# actctx.c (activation contexts) + threadpool.c are user-mode features
# with no kernel consumers; unlink on Xbox.  Other arches keep them.
if(NOT SARCH STREQUAL "xbox")
    list(APPEND RTL_WINE_SOURCE actctx.c threadpool.c)
endif()
list(APPEND RTL_WINE_SOURCE_VISTA
    product.c
    threadpool.c
)
set_source_files_properties(threadpool.c PROPERTIES COMPILE_DEFINITIONS __WINESRC__)

if(MSVC)
    # Silence warning C4267: 'initializing': conversion from 'size_t' to 'const int', possible loss of data
    set_source_files_properties(${RTL_WINE_SOURCE} PROPERTIES COMPILE_OPTIONS "/wd4267")
endif()

list(APPEND SOURCE
    ${RTL_WINE_SOURCE}
    wine_debug.c
    access.c
    acl.c
    appverifier.c
    assert.c
    atom.c
    avltable.c
    bitmap.c
    bootdata.c
    compress.c
    crc32.c
    critical.c
    dbgbuffer.c
    debug.c
    dos8dot3.c
    encode.c
    env.c
    error.c
    exception.c
    generictable.c
    handle.c
    heap.c
    heapdbg.c
    heappage.c
    heapuser.c
    image.c
    interlck.c
    largeint.c
    locale.c
    luid.c
    memstream.c
    message.c
    nls.c
    path.c
    ppb.c
    prefix.c
    priv.c
    process.c
    propvar.c
    random.c
    rangelist.c
    registry.c
    res.c
    resource.c
    rxact.c
    sd.c
    security.c
    sid.c
    slist.c
    splaytree.c
    sysvol.c
    thread.c
    time.c
    timezone.c
    trace.c
    unicode.c
    unicodeprefix.c
    vectoreh.c
    version.c
    wait.c
    workitem.c
    rtl.h)

if(SARCH STREQUAL "xbox")
    # Xbox kernel doesn't ship a TCP/IP stack; RtlIp[v4|v6]* address parsers
    # have zero consumers in the kept image.  Other arches keep network.c.
    list(REMOVE_ITEM SOURCE network.c)
    # Codepage routines (RtlMultiByteToUnicodeN etc.) come from ASCII fast-paths
    # in ntoskrnl/xb/nls-stubs.c; the 132 KB CP-1252/CP-437 tables go with them.
    # Case-table lookup (RtlpUpcase/Downcase) stays driven by NxkrnlNlsCase.
    list(REMOVE_ITEM SOURCE nls.c)
    # User-mode surface no kept Xbox code or folded driver consumes.  Titles
    # run ring 0 with no PEB/TEB/environment/atom-table/user-heap/vectored-EH.
    # registry.c is dropped entirely: with no configuration manager the Rtl
    # registry surface has no implementation to call, and we want the linker
    # to surface every caller (drivers/* registry-touching paths) as an
    # undefined reference rather than silently returning STATUS_NOT_FOUND.
    list(REMOVE_ITEM SOURCE
        env.c
        process.c
        ppb.c
        timerqueue.c
        wait.c
        vectoreh.c
        propvar.c
        rxact.c
        atom.c
        appverifier.c
        heapdbg.c
        heappage.c
        heapuser.c
        message.c
        path.c
        registry.c
        # Additional unlinkable rtl files identified via per-obj
        # all-symbol audit (zero live symbols in the final xboxkrnl.exe
        # for release AND DBG builds).  Saves ~50 KB of compile time.
        heap.c             # RtlAllocateHeap -- titles use Mm
        locale.c           # RtlLocale*
        critical.c         # RtlEnterCriticalSection -- usermode
        workitem.c         # RtlQueueWorkItem -- usermode worker pool
        security.c         # Rtl token/SID helpers
        rangelist.c        # RtlInitializeRangeList -- no IO arbitration
        priv.c             # Rtl*Privilege
        trace.c            # RtlGetStackTrace
        dbgbuffer.c        # RtlpDebugBuffer
        unicodeprefix.c    # RtlInsertUnicodePrefix
        dos8dot3.c         # 8.3 short names -- FATX has long names
        memstream.c        # RtlpInitMemoryStream
        thread.c           # RtlCreateUserThread
        compress.c         # RtlCompressBuffer LZNT1
        resource.c         # RtlInitializeResource
        wine_debug.c       # Wine TRACE/WARN plumbing
        version.c          # RtlGetVersion etc
        utf8.c             # RtlUTF8ToUnicodeN
        bootdata.c         # boot-data registry helpers
        handle.c           # RtlAllocateHandle
        encode.c           # RtlEncodePointer
        slist.c            # RtlInitializeSListHead -- portable C
        luid.c             # RtlInitializeLuid
        sysvol.c           # RtlSetSystemVolumeInformationFolder
        interlck.c         # portable interlocked fallback
        prefix.c)          # RtlInsertPrefix ANSI
else()
    list(APPEND SOURCE network.c)
endif()

if(ARCH STREQUAL "i386")
    list(APPEND ASM_SOURCE
        i386/debug_asm.S
        i386/except_asm.s
        i386/interlck.S
        i386/rtlmem.s
        i386/rtlswap.S
        i386/res_asm.s)
    list(APPEND SOURCE i386/except.c)
    if(NOT SARCH STREQUAL "xbox")
        # i386/thread.c is the RtlCreateUserThread x86 helper -- dead on
        # Xbox (no usermode).
        list(APPEND SOURCE i386/thread.c)
    endif()
elseif(ARCH STREQUAL "amd64")
    list(APPEND ASM_SOURCE
        amd64/debug_asm.S
        amd64/except_asm.S
        amd64/slist.S
        amd64/unwind-asm.s)
    list(APPEND SOURCE
        bitmap64.c
        byteswap.c
        amd64/dynfntbl.c
        amd64/except.c
        amd64/unwind.c
        amd64/stubs.c
        mem.c)
elseif(ARCH STREQUAL "arm")
    list(APPEND ASM_SOURCE arm/debug_asm.S)
    list(APPEND SOURCE
        arm/except.c
        byteswap.c
        mem.c)
endif()

add_asm_files(rtl_asm ${ASM_SOURCE})
add_library(rtl ${SOURCE} ${rtl_asm})
target_link_libraries(rtl PRIVATE pseh)
add_pch(rtl rtl.h SOURCE)
add_dependencies(rtl psdk asm)

list(APPEND SOURCE_VISTA
    ${RTL_WINE_SOURCE_VISTA}
    condvar.c
    locale.c
    runonce.c
    srw.c
    threadpool.c
    unicode_vista.c
    utf8.c)

if(NOT SARCH STREQUAL "xbox")
    add_library(rtl_vista ${SOURCE_VISTA})
    add_pch(rtl_vista rtl_vista.h SOURCE_VISTA)
    add_dependencies(rtl_vista psdk)
    target_link_libraries(rtl_vista PRIVATE pseh)
endif()
