
PROJECT(NTOS)

if (CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID STREQUAL "Clang")
    # Enable this again. CORE-17637
    add_compile_options(-Wunused-result)
endif()

include(ntos.cmake)

set(NTOSKRNL_SOURCE ${SOURCE})
set(NTOSKRNL_ASM_SOURCE ${ASM_SOURCE})

set(NTKRNLMP_SOURCE ${SOURCE})
set(NTKRNLMP_ASM_SOURCE ${ASM_SOURCE})

spec2def(ntoskrnl.exe ntoskrnl.spec ADD_IMPORTLIB)

# xboxkrnl.exe carries a real PE export directory of the ~371 Xbox kernel
# ordinals.  xb/gen-exports.py reads nxdk's xboxkrnl.exe.def together with
# xb/ordinals.map and emits a spec2def spec + a self-naming bugcheck
# scaffold for every unmapped ordinal.  (The vestigial NT-name ntoskrnl.spec
# above stays only for the libntoskrnl importlib target machinery --
# nothing imports ntoskrnl by name now.)
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/xb-exports.spec
           ${CMAKE_CURRENT_BINARY_DIR}/xb-exports.c
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/xb/gen-exports.py
            ${CMAKE_CURRENT_SOURCE_DIR}/xb/xboxkrnl.exe.def
            ${CMAKE_CURRENT_SOURCE_DIR}/xb/ordinals.map
            ${CMAKE_CURRENT_BINARY_DIR}/xb-exports.spec
            ${CMAKE_CURRENT_BINARY_DIR}/xb-exports.c
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/xb/gen-exports.py
            ${CMAKE_CURRENT_SOURCE_DIR}/xb/ordinals.map
            ${CMAKE_CURRENT_SOURCE_DIR}/xb/xboxkrnl.exe.def
    COMMENT "Generating the Xbox-ordinal export spec + scaffold"
    VERBATIM)
spec2def(xboxkrnl.exe ${CMAKE_CURRENT_BINARY_DIR}/xb-exports.spec)

# The in-kernel import table (xb/gen-import-table.py) is no longer
# generated: the tree-wide _NTOSKRNL_/_NTHAL_/_NTSYSTEM_ defines (top-level
# CMakeLists) make NTKERNELAPI/NTHALAPI/NTSYSAPI expand empty, so the
# folded drivers reference kernel APIs directly -- no __imp_ pointer
# slots, no indirect calls, no GC rooting.  The export-name renames that
# made the old table necessary are gone too: the Exi* fast-mutex rename
# is NT-HAL-only (gated off in exfuncs.h), and the Ke critical-region
# out-of-line bodies now carry their plain names (ke/apc.c).

# The .def is no longer passed to ld -- fix-edata.py builds the export
# directory post-link inside the reserved NxkExportDirectory array
# (gen-exports.py), which keeps bfd's NONAME name-table over-reservation
# (~6 KB of zeros) and its alignment page out of .rdata.  Without ld's
# .def-driven rooting, xb-export-roots.c is what keeps every ordinal's
# implementation alive against LTO/--gc-sections on ALL builds: it takes
# each symbol's address from a -fno-lto TU so plain COFF relocations root
# them; -u pins the (INITDATA, boot-discarded) anchor array itself, and
# the export directory is pinned the same way.
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/xb-export-roots.c
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/xb/gen-export-roots.py
            ${CMAKE_CURRENT_BINARY_DIR}/xboxkrnl.def
            ${CMAKE_CURRENT_BINARY_DIR}/xb-export-roots.c
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/xboxkrnl.def
            ${CMAKE_CURRENT_SOURCE_DIR}/xb/gen-export-roots.py
    COMMENT "Generating export-root anchors"
    VERBATIM)
set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/xb-export-roots.c
    PROPERTIES COMPILE_FLAGS "-fno-lto")
set(NXK_EXPORT_ROOTS_SOURCE ${CMAKE_CURRENT_BINARY_DIR}/xb-export-roots.c)

# Embed the Unicode case-mapping table into the kernel image.  The two
# 66 KB codepage tables (CP-1252, CP-437) are dropped; the Rtl* MultiByte
# routines route through ASCII fast-paths in ntoskrnl/xb/nls-stubs.c.
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/xb-nls.c
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/xb/gen-nls.py
            ${REACTOS_SOURCE_DIR}/media/nls/l_intl.nls
            ${CMAKE_CURRENT_BINARY_DIR}/xb-nls.c
    DEPENDS ${REACTOS_SOURCE_DIR}/media/nls/l_intl.nls
            ${CMAKE_CURRENT_SOURCE_DIR}/xb/gen-nls.py
    COMMENT "Embedding the Unicode case table into the kernel"
    VERBATIM)

# Bake the release version (root VERSION) plus a git-describe build id into
# an INITDATA banner that NxkPrintVersionBanner() prints on the debug serial
# at boot.  Regenerates when VERSION or the git commit / worktree state
# changes; the string and printer are both INIT-discarded (no resident cost).
set(NXK_VERSION_DEPS
    ${CMAKE_CURRENT_SOURCE_DIR}/xb/gen-version.py
    ${REACTOS_SOURCE_DIR}/VERSION)
if(EXISTS ${REACTOS_SOURCE_DIR}/.git/HEAD)
    list(APPEND NXK_VERSION_DEPS
        ${REACTOS_SOURCE_DIR}/.git/HEAD
        ${REACTOS_SOURCE_DIR}/.git/index)
endif()
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/xb-version.c
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/xb/gen-version.py
            ${REACTOS_SOURCE_DIR}
            ${REACTOS_SOURCE_DIR}/VERSION
            ${CMAKE_CURRENT_BINARY_DIR}/xb-version.c
    DEPENDS ${NXK_VERSION_DEPS}
    COMMENT "Baking the nxkrnl version banner"
    VERBATIM)

# Link GCC stack protector lib
if (STACK_PROTECTOR)
    target_link_libraries(libntoskrnl INTERFACE gcc_ssp_nt)
endif()

add_asm_files(ntoskrnl_asm ${NTOSKRNL_ASM_SOURCE})

if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
    # Clang optimises strcmp calls to memcmp.
    target_link_libraries(libntoskrnl INTERFACE memcmp)
endif()

if((CMAKE_C_COMPILER_ID STREQUAL "Clang") OR (CMAKE_C_COMPILER_ID STREQUAL "GNU"))
    # Enable xsave feature
    add_compile_options(-mxsave)
endif()

list(APPEND PCH_SKIP_SOURCE
    guid.c
    # Generated C arrays + scaffolds -- no PCH.
    ${CMAKE_CURRENT_BINARY_DIR}/xb-nls.c
    ${CMAKE_CURRENT_BINARY_DIR}/xb-exports.c
    ${CMAKE_CURRENT_BINARY_DIR}/xb-version.c
    # Xbox-side kernel additions.  xbe.c is the loader proper; the rest
    # implement Xbox-specific ordinals.  xb/mm/*.c form the Xbox MM layer
    # (see xb/mm/mm.h for the layout).
    xb/mm/init.c
    xb/mm/contig.c
    xb/mm/sysva.c
    xb/mm/iospace.c
    xb/mm/api.c
    xb/mm/pagesupply.c
    xb/mm/arm3stubs.c
    xb/crypto.c
    ${REACTOS_SOURCE_DIR}/sdk/lib/cryptlib/sha1.c
    ${REACTOS_SOURCE_DIR}/sdk/lib/cryptlib/rc4.c
    xb/strace.c
    xb/rwlock.c
    xb/scattergather.c
    xb/exflist.c
    xb/rtlprintf.c
    xb/rtlbyteswap.c
    xb/xbe.c
    xb/object-types.c
    xb/obcreate.c
    xb/psstats.c
    xb/devnode.c
    xb/nxldr-entry.c)

# nxpool: replace the ARM3 small-block pool backend (mm/ARM3/expool.c)
# with xb/mm/pool.c.  Revert = -DNXK_MM_POOL=OFF + reconfigure.
option(NXK_MM_POOL "Use the nxmm pool backend instead of ARM3 expool" ON)
if(NXK_MM_POOL)
    list(REMOVE_ITEM NTOSKRNL_SOURCE
        ${REACTOS_SOURCE_DIR}/ntoskrnl/mm/ARM3/expool.c)
    list(APPEND PCH_SKIP_SOURCE xb/mm/pool.c)
    add_definitions(-DNXK_MM_POOL)
endif()

# nxpoolpages: whole pages for nxpool come from a PTE-mapped VA window
# backed by the nxmm supply (xb/mm/poolpages.c) instead of the
# ARM3 pool page layer.  Revert = -DNXK_MM_POOLPAGES=OFF.
option(NXK_MM_POOLPAGES "Use the nxmm pool page window instead of ARM3" ON)
if(NXK_MM_POOLPAGES)
    add_definitions(-DNXK_MM_POOLPAGES)
    list(REMOVE_ITEM NTOSKRNL_SOURCE
        ${REACTOS_SOURCE_DIR}/ntoskrnl/mm/ARM3/pool.c)
    list(APPEND PCH_SKIP_SOURCE xb/mm/poolpages.c)
endif()

# nxphys: the per-page array allocator in xb/mm/pagesupply.c
# becomes the real page supply; the ARM3 colored free lists are
# bypassed (their three primitives forward into the array).
# Revert = -DNXK_MM_PHYS=OFF.
option(NXK_MM_PHYS "Use the nxmm array page supply instead of ARM3 lists" ON)
if(NXK_MM_PHYS)
    if(NOT NXK_MM_POOLPAGES)
        message(FATAL_ERROR "NXK_MM_PHYS requires NXK_MM_POOLPAGES (the \
ARM3 pool layer needs the PFN database the PHYS config no longer builds)")
    endif()
    add_definitions(-DNXK_MM_PHYS)
endif()

# nxvm: route the title VM ordinals (XeNt*VirtualMemory, address
# protect, statistics) to the eager-commit manager in xb/mm/vm.c.
# The ARM3 VAD path stays linked but unreachable from titles; it is
# deleted with the PFN retirement.  Revert = -DNXK_MM_VM=OFF.
option(NXK_MM_VM "Use the nxmm title VM manager instead of ARM3 VADs" ON)
if(NXK_MM_VM)
    add_definitions(-DNXK_MM_VM)
    list(REMOVE_ITEM NTOSKRNL_SOURCE
        ${REACTOS_SOURCE_DIR}/ntoskrnl/mm/ARM3/virtual.c
        ${REACTOS_SOURCE_DIR}/ntoskrnl/mm/ARM3/pagfault.c
        ${REACTOS_SOURCE_DIR}/ntoskrnl/mm/ARM3/vadnode.c
        ${REACTOS_SOURCE_DIR}/ntoskrnl/mm/ARM3/wslist.cpp
        ${REACTOS_SOURCE_DIR}/ntoskrnl/mm/mmfault.c)
    list(APPEND PCH_SKIP_SOURCE xb/mm/vm.c xb/mm/fault.c)
endif()

# nxcache: replace the section/VACB file cache (ntoskrnl/cc/) with the
# fixed-pool buffer cache in xb/mm/cache.c.
# Revert = -DNXK_MM_CACHE=OFF + reconfigure.
option(NXK_MM_CACHE "Use the nxmm file cache instead of the ROS Cc" ON)
if(NXK_MM_CACHE)
    list(REMOVE_ITEM NTOSKRNL_SOURCE
        ${REACTOS_SOURCE_DIR}/ntoskrnl/cc/cacheman.c
        ${REACTOS_SOURCE_DIR}/ntoskrnl/cc/copy.c
        ${REACTOS_SOURCE_DIR}/ntoskrnl/cc/fs.c
        ${REACTOS_SOURCE_DIR}/ntoskrnl/cc/lazywrite.c
        ${REACTOS_SOURCE_DIR}/ntoskrnl/cc/mdl.c
        ${REACTOS_SOURCE_DIR}/ntoskrnl/cc/pin.c
        ${REACTOS_SOURCE_DIR}/ntoskrnl/cc/view.c
        # The section/marea layer existed to back Cc and user mappings;
        # nxcache + nxvm leave it unreachable.
        ${REACTOS_SOURCE_DIR}/ntoskrnl/mm/ARM3/section.c
        ${REACTOS_SOURCE_DIR}/ntoskrnl/mm/section.c
        ${REACTOS_SOURCE_DIR}/ntoskrnl/mm/marea.c
        ${REACTOS_SOURCE_DIR}/ntoskrnl/mm/region.c
        ${REACTOS_SOURCE_DIR}/ntoskrnl/mm/rmap.c
        ${REACTOS_SOURCE_DIR}/ntoskrnl/mm/pagefile.c
        ${REACTOS_SOURCE_DIR}/ntoskrnl/mm/balance.c)
    list(APPEND PCH_SKIP_SOURCE xb/mm/cache.c)
    add_definitions(-DNXK_MM_CACHE)
endif()

add_executable(ntoskrnl
    ${ntoskrnl_asm}
    ${NTOSKRNL_SOURCE}
    ${PCH_SKIP_SOURCE}
    ntoskrnl.rc
    ${NXK_EXPORT_ROOTS_SOURCE})
target_link_options(ntoskrnl PRIVATE
    -Wl,-u,_NxkExportRoots -Wl,-u,_NxkExportDirectory)

# Auto-INIT placement (two-stage link).  -ffunction-sections on the LINK makes
# per-function .text$<fn> sections survive LTO codegen; tools/lto-init-relink.py
# (wired as RULE_LAUNCH_LINK) then relinks against a generated linker script
# that names each boot-only function's section inside the discardable INIT
# section -- no source CODE_SEG tags.  init-ref-check (below) gates it.
if(SARCH STREQUAL "xbox" AND NOT MSVC)
    target_link_options(ntoskrnl PRIVATE -ffunction-sections)
    set_target_properties(ntoskrnl PROPERTIES RULE_LAUNCH_LINK
        "${CMAKE_COMMAND} -E env LTO_INIT_FINDTOOL=${REACTOS_SOURCE_DIR}/tools/find-init-candidates.py DEF=${CMAKE_CURRENT_BINARY_DIR}/xboxkrnl.def NM=${CMAKE_NM} OBJDUMP=${CMAKE_OBJDUMP} AR=${CMAKE_AR} python3 ${REACTOS_SOURCE_DIR}/tools/lto-init-relink.py")
    # RULE_LAUNCH_LINK is only a command prefix -- CMake does not treat the
    # scripts/data it names as inputs, so editing them would not relink the
    # kernel (and flash.bin would stay stale).  Declare them as LINK_DEPENDS so
    # a change to the INIT-placement logic or the force-resident list forces a
    # relink like any other build input.
    set_target_properties(ntoskrnl PROPERTIES LINK_DEPENDS
        "${REACTOS_SOURCE_DIR}/tools/lto-init-relink.py;${REACTOS_SOURCE_DIR}/tools/find-init-candidates.py;${REACTOS_SOURCE_DIR}/tools/init-force-resident.list")
endif()

# The kernel ships as xboxkrnl.exe.  Target stays "ntoskrnl" so reactos's
# set_module_type / set_image_base / spec2def_to_def consumers keep working
# unchanged.
set_target_properties(ntoskrnl PROPERTIES OUTPUT_NAME xboxkrnl)
# One contiguous image at the devkit base (PA 0x4000000+) for both DBG
# and release.  The split-image retail-base layout -- header + .text in
# the retail slot, other sections in an 8 MB high zone at PA 0x3400000 --
# was abandoned: those high-zone .data/.bss sections landed inside the
# loader's high-zone free descriptor (md_hz1), so under cache/pool
# pressure the page supply handed their physical frames out as IO
# buffers, aliasing kernel memory and corrupting FATX.  Retail isn't a
# split image either.  The kernel runs above the low 64 MiB (needs the
# 128 MiB devkit map) and paints a stub PE header at the retail VA
# 0x80010000 for title ordinal resolution (ntoskrnl/xb/xbe.c).  Once the
# total resident footprint fits below the title pool floor (PA 0x61000,
# ~324 KB), move back to a single low image at 0x80010000 -- still not
# split -- to run on bare 64 MB.
# Release fits below the title pool floor (resident == 0x51000) and *can* run as
# a single low image at the retail base 0x80010000 on bare 64 MB -- but the
# resident footprint is still ~18 pages over the budget Halo's intro needs there
# (the retail map), so titles don't boot on real 64 MB
# yet.  FOR NOW both DBG and release run high at the devkit base 0x84000000 on
# the 128 MB map (DBG never fit low anyway) so titles boot.  Move release back to
# 0x80010000 once the footprint closes.  boot/nxldr picks its layout from this
# ImageBase.
set_module_type(ntoskrnl kernel IMAGEBASE 0x84000000)

# The kernel is loaded at a fixed image base by boot/nxldr; nothing ever
# relocates it.  Strip .reloc post-link to shave ~119 KB off xboxkrnl.exe.
# (-Wl,--disable-reloc-section doesn't take effect because the linker
# script's .reloc rule keeps the section reference alive.)
if(SARCH STREQUAL "xbox" AND NOT MSVC)
    # fix-edata must run pre-strip: it reads the COFF symbol table.  It
    # builds the PE export directory inside the reserved NxkExportDirectory
    # array and fills every ordinal's RVA from the final symbol values.
    add_custom_command(TARGET ntoskrnl POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} --remove-section=.reloc $<TARGET_FILE:ntoskrnl>
        COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/xb/fix-edata.py
                $<TARGET_FILE:ntoskrnl> ${CMAKE_CURRENT_BINARY_DIR}/xboxkrnl.def)
endif()

# Strip debug symbols from xboxkrnl.exe before it's appended to freeldr and
# packed into the flash image.  The image sits in a 7 MB flash slot, and
# RelWithDebInfo's -g symbols easily blow that.  freeldr_pe gets the same
# treatment in boot/freeldr/freeldr/pcat.cmake.  Only on Xbox so other arches
# keep upstream behaviour.
#
# Re-run pefixup after strip -- strip clobbers IMAGE_SCN_MEM_DISCARDABLE on
# INIT and IMAGE_SCN_MEM_NOT_PAGED on .text (it warns "ignoring section
# flag" for each).  Without DISCARDABLE the Mm doesn't reclaim INIT pages
# after boot, leaking ~96 KB.
if(SARCH STREQUAL "xbox" AND NOT MSVC)
    # --strip-all removes the COFF symbol table that the linker writes at
    # the end of the PE -- about 950 KB on a debug build, 700 KB on
    # release.  It is never loaded at runtime (kernel exports go through
    # .edata, drivers through .idata), so safe to drop.  --strip-debug
    # alone would only nuke DWARF, missing the COFF table.
    #
    # We save the pre-strip binary as xboxkrnl.unstripped.exe so
    # tools/kernel-size.py and similar nm-based analysis tools have a
    # symbol table to walk.
    # .rsrc here is only the boot-animation bitmaps and the version block;
    # there is no bugcheck message table, and nothing walks resources after
    # Phase 1 (LdrFindResource_U's only callers run during init).  Marking
    # it DISCARDABLE lets MiFreeInitializationCode reclaim its pages with
    # INIT.  NT keeps the kernel's .rsrc resident for bugcheck text -- that
    # is why --kernel mode alone doesn't discard it.
    # init-ref-check: fail the build if any resident section still
    # references INIT/INITDATA/.rsrc (use-after-free once
    # MiFreeInitializationCode runs).  LTO can create such an edge
    # silently when it clones/places an INIT-tagged function; this is the
    # post-link guard that makes CODE_SEG("INIT") migrations safe to
    # land.  Allowlist: tools/init-ref.allow.  Needs the unstripped
    # image (symbol table), so it runs right after the copy.
    # COVERAGE (non-LTO) layouts keep boot-only callers resident instead of
    # migrating them into INIT, so the resident->discardable audit reports
    # false positives there; it polices the shipped LTO layout only.
    if(NOT COVERAGE)
        set(_init_ref_check
            COMMAND ${CMAKE_COMMAND} -E env NM=${CMAKE_NM} OBJDUMP=${CMAKE_OBJDUMP}
                    python3 ${REACTOS_SOURCE_DIR}/tools/init-audit.py --check
                    $<TARGET_FILE_DIR:ntoskrnl>/xboxkrnl.unstripped.exe)
    else()
        set(_init_ref_check)
    endif()
    add_custom_command(TARGET ntoskrnl POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:ntoskrnl>
                $<TARGET_FILE_DIR:ntoskrnl>/xboxkrnl.unstripped.exe
        ${_init_ref_check}
        COMMAND ${CMAKE_STRIP} --strip-all $<TARGET_FILE:ntoskrnl>
        COMMAND native-pefixup --kernel --section:.rsrc,D $<TARGET_FILE:ntoskrnl>)
endif()

source_group(TREE ${REACTOS_SOURCE_DIR}/ntoskrnl PREFIX "Source Files" FILES ${NTOSKRNL_SOURCE})

# halxbox (the Xbox HAL) is linked into the kernel image (alongside kdcom /
# xboxbvid).  fast486 (the x86 emulator for the HAL's INT 10h video-BIOS
# thunks) was removed -- Xbox has no PC BIOS; x86bios.c is stubbed in
# hal/halx86/legacy/.  vfatfs handles both FAT32 and FATX (the Xbox HDD
# format), so it stays.
target_link_libraries(ntoskrnl ntoskrnl_vista cportlib csq ${PSEH_LIB} rtl ${ROSSYM_LIB} libcntpr setjmp kdcom xboxbvid halxbox pci pciidex pciide atapi classpnp disk nxcdrom partmgr mountmgr xdvdfs vfatfs)

if(STACK_PROTECTOR)
    target_link_libraries(ntoskrnl gcc_ssp_nt)
endif()

# The HAL is linked into the kernel image (halxbox static library), so the
# kernel imports nothing -- no add_importlibs.  HAL ABI re-exported from
# ntoskrnl.spec.
add_pch(ntoskrnl ${REACTOS_SOURCE_DIR}/ntoskrnl/include/ntoskrnl.h "${PCH_SKIP_SOURCE}")
add_dependencies(ntoskrnl psdk asm)
add_cd_file(TARGET ntoskrnl DESTINATION reactos/system32 NO_CAB FOR all)

if(NOT SARCH STREQUAL "xbox")
    add_subdirectory(ntkrnlmp)

    # ntdllsys is the kernel-side ntdll syscall stub library; nothing in
    # the Xbox kernel links it (no user-mode subsystem).
    add_asm_files(ntdllsys_asm ntdll.S)
    add_library(ntdllsys ${ntdllsys_asm})
    set_target_properties(ntdllsys PROPERTIES LINKER_LANGUAGE "C")
    add_dependencies(ntdllsys asm)
endif()
