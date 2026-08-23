
include_directories(include)

add_definitions(-D_CRTBLD)

# The CRT is the libcall provider: GCC materializes calls to memcmp/
# strstr/sprintf/towlower/... during LTO ltrans, after archive symbol
# resolution.  Those late references must extract real machine code with
# real internal dependencies (sprintf -> _streamout), not bytecode whose
# COFF shadow the plugin already discarded.  Keep the whole CRT out of
# LTO; it is leaf string/mem/printf code with nothing to gain from
# cross-TU inlining anyway.
if(LTCG)
    add_compile_options(-fno-lto)
endif()

# Each of these populates per-subsystem source lists (LIBCNTPR_*, MSVCRTEX_*,
# ...) -- libcntpr below pulls *all* of them.  Kept unconditional even on
# SARCH=xbox because removing any breaks libcntpr's string/mem/etc. routines.
include(except/except.cmake)
include(float/float.cmake)
include(math/math.cmake)
include(mem/mem.cmake)
include(misc/misc.cmake)
include(printf/printf.cmake)
include(search/search.cmake)
include(startup/startup.cmake)
include(stdlib/stdlib.cmake)
include(string/string.cmake)
include(wstring/wstring.cmake)

include(libcntpr.cmake)  # used by the kernel

# Kernel-only build skips user-mode CRT bits: crtmath / msvcrtex / oldnames,
# the wsprintf shim, the dbgrpt assertion lib.
if(NOT SARCH STREQUAL "xbox")
    include(crtmath.cmake)
    include(msvcrtex.cmake)
    include(oldnames.cmake)

    add_library(user32_wsprintf
        printf/streamout.c
        printf/wstreamout.c
        printf/wsprintfA.c
        printf/wsprintfW.c
        printf/wvsprintfA.c
        printf/wvsprintfW.c
        printf/wvsnprintfA.c
        printf/wvsnprintfW.c
        string/strnlen.c
        string/wcsnlen.c
        string/mbstowcs_nt.c
        string/wcstombs_nt.c)

    add_dependencies(user32_wsprintf psdk)
    target_compile_definitions(user32_wsprintf PRIVATE _USER32_WSPRINTF)

    if(MSVC AND ARCH STREQUAL "i386")
        add_asm_files(ftol2_asm
            math/i386/ftol2_asm.s
            math/i386/ftoul2_legacy_asm.s
        )
        add_library(ftol2 ${ftol2_asm})
        set_target_properties(ftol2 PROPERTIES LINKER_LANGUAGE "C")
        add_dependencies(ftol2 asm)
    endif()

    add_library(dbgrpt misc/dbgrpt.cpp)
    target_link_libraries(dbgrpt chkstk ${PSEH_LIB})
    target_compile_definitions(dbgrpt PRIVATE
        _MT
        _DEBUG)
    add_dependencies(dbgrpt psdk asm)
endif()
