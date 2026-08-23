# boot/nxldr -- minimal Xbox loader.  Output: nxldr.bin, the 256 KB flash image
# with a 0xff-filled hole that gen-flash-bin.py fills with the XZ-compressed
# kernel payload (at the KernelPayload* linker symbols from nxldr.ld).

set(NXLDR_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(NXLDR_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR})

if(DBG)
    set(NXLDR_DBG_FLAG -DDBG=1)
else()
    set(NXLDR_DBG_FLAG -DDBG=0)
endif()

set(NXLDR_CFLAGS
    -m32
    -ffreestanding
    -nostdlib
    -fno-pic
    -fno-stack-protector
    -fomit-frame-pointer
    -fno-asynchronous-unwind-tables
    # No vector instructions: CR4.OSFXSR is off in the loader, so any
    # gcc-emitted SSE/MMX (e.g. an auto-vectorized loop) would #UD into a
    # triple-fault.  The kernel enables SSE later.
    -mno-sse
    -mno-sse2
    -mno-mmx
    -mno-3dnow
    -mno-avx
    -mgeneral-regs-only
    -Wall
    -Os
    -ffunction-sections
    -fdata-sections
    ${NXLDR_DBG_FLAG}
    -I${NXLDR_SRC_DIR}
)

set(NXLDR_LDFLAGS
    -m elf_i386
    -nostdlib
    --no-dynamic-linker
    -static
    --gc-sections
    -T ${NXLDR_SRC_DIR}/nxldr.ld
)

# Listed so any header edit retriggers the whole loader build.
set(NXLDR_HEADERS
    ${NXLDR_SRC_DIR}/nxldr.h
    ${NXLDR_SRC_DIR}/log.h
    ${NXLDR_SRC_DIR}/pe.h
    ${NXLDR_SRC_DIR}/paging.h
    ${NXLDR_SRC_DIR}/boot-layout.h
)

# startup.S -- the reset path + .low_rom prologue (assembly entry).
add_custom_command(
    OUTPUT  ${NXLDR_OUT_DIR}/startup.o
    COMMAND gcc ${NXLDR_CFLAGS} -c ${NXLDR_SRC_DIR}/startup.S -o ${NXLDR_OUT_DIR}/startup.o
    DEPENDS ${NXLDR_SRC_DIR}/startup.S ${NXLDR_SRC_DIR}/xcodes.inc.S
)
set(NXLDR_OBJS ${NXLDR_OUT_DIR}/startup.o)

# C sources, one object each (the "/" in xz/ paths flattens to "_" in the .o
# name).  serial.c is DBG-only; the release loader is serial-silent.
set(NXLDR_SOURCES
    loader.c
    mem.c
    pe.c
    paging.c
    xz/xz_crc32.c
    xz/xz_dec_lzma2.c
    xz/xz_dec_stream.c
    xz/xz_dec_bcj.c
)
if(DBG)
    list(APPEND NXLDR_SOURCES serial.c)
endif()

foreach(src ${NXLDR_SOURCES})
    string(REPLACE ".c" ".o" obj ${src})
    string(REPLACE "/" "_" obj ${obj})
    add_custom_command(
        OUTPUT  ${NXLDR_OUT_DIR}/${obj}
        COMMAND gcc ${NXLDR_CFLAGS} -c ${NXLDR_SRC_DIR}/${src} -o ${NXLDR_OUT_DIR}/${obj}
        DEPENDS ${NXLDR_SRC_DIR}/${src} ${NXLDR_HEADERS}
    )
    list(APPEND NXLDR_OBJS ${NXLDR_OUT_DIR}/${obj})
endforeach()

# nanoprintf.c (DBG only) needs -Wno-conversion and tracks its own header.
if(DBG)
    add_custom_command(
        OUTPUT  ${NXLDR_OUT_DIR}/nanoprintf.o
        COMMAND gcc ${NXLDR_CFLAGS} -Wno-conversion -c
                ${NXLDR_SRC_DIR}/nanoprintf.c -o ${NXLDR_OUT_DIR}/nanoprintf.o
        DEPENDS ${NXLDR_SRC_DIR}/nanoprintf.c ${NXLDR_SRC_DIR}/nanoprintf.h
    )
    list(APPEND NXLDR_OBJS ${NXLDR_OUT_DIR}/nanoprintf.o)
endif()

add_custom_command(
    OUTPUT  ${NXLDR_OUT_DIR}/nxldr.elf
    COMMAND ld ${NXLDR_LDFLAGS} ${NXLDR_OBJS} -o ${NXLDR_OUT_DIR}/nxldr.elf
    DEPENDS ${NXLDR_OBJS} ${NXLDR_SRC_DIR}/nxldr.ld
)

add_custom_command(
    OUTPUT  ${NXLDR_OUT_DIR}/nxldr.bin
    COMMAND objcopy -O binary --remove-section=.data --gap-fill=0xff
            ${NXLDR_OUT_DIR}/nxldr.elf ${NXLDR_OUT_DIR}/nxldr.bin
    DEPENDS ${NXLDR_OUT_DIR}/nxldr.elf
)

add_custom_target(nxldr ALL DEPENDS ${NXLDR_OUT_DIR}/nxldr.bin)

if(SARCH STREQUAL "xbox")
    set(_flash_bin   ${CMAKE_BINARY_DIR}/flash.bin)
    set(_startup_bin ${NXLDR_OUT_DIR}/nxldr.bin)
    add_custom_command(
        OUTPUT ${_flash_bin}
        COMMAND ${NXLDR_SRC_DIR}/gen-flash-bin.py
                --out     ${_flash_bin}
                --nxldr   ${_startup_bin}
                --kernel  $<TARGET_FILE:ntoskrnl>
        DEPENDS ntoskrnl nxldr
                $<TARGET_FILE:ntoskrnl> ${_startup_bin}
                ${NXLDR_SRC_DIR}/gen-flash-bin.py
        COMMENT "Assembling Xbox flash image (gen-flash-bin.py)"
        VERBATIM)
    add_custom_target(flash ALL DEPENDS ${_flash_bin})
endif()
