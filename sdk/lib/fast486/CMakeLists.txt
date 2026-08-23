
include_directories(${REACTOS_SOURCE_DIR}/sdk/include/reactos/libs/fast486)

list(APPEND SOURCE
    debug.c
    fast486.c
    opcodes.c
    opgroups.c
    extraops.c
    common.c
    fpu.c)

add_library(fast486 ${SOURCE})
add_dependencies(fast486 xdk)

# for the Xbox build fast486 is linked into the kernel image (the
# HAL's x86bios support uses it), so its references to the kernel's own
# NTSYSAPI routines (RtlAssert, ...) must be direct symbols, not _imp_
# thunks.
if(SARCH STREQUAL "xbox")
    target_compile_definitions(fast486 PRIVATE _NTSYSTEM_)
endif()
