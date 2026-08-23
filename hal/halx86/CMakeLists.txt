
add_definitions(
    -D_NTHALDLL_
    -D_NTHAL_)

# for the Xbox build the HAL is linked into the kernel image (halxbox
# is a static library, not a separate hal.dll), so its references to the
# kernel's own API (NTKERNELAPI / NTSYSAPI) must be direct symbols, not _imp_
# thunks.
if(SARCH STREQUAL "xbox")
    add_definitions(-D_NTSYSTEM_)
endif()

include_directories(
    include
    ${REACTOS_SOURCE_DIR}/ntoskrnl/include
    ${REACTOS_SOURCE_DIR}/sdk/include/reactos/libs/fast486)

function(add_hal _halname)
    cmake_parse_arguments(_haldata "STATICLIB" "" "SOURCES;COMPONENTS" ${ARGN})

    if(_haldata_STATICLIB)
        # build the HAL as a static library of its component objects,
        # linked directly into ntoskrnl -- the Xbox kernel is one image, with
        # no separate hal.dll.  No spec2def / importlib / module-type / cd-file.
        foreach(_component ${_haldata_COMPONENTS})
            list(APPEND _hal_objects "$<TARGET_OBJECTS:lib_hal_${_component}>")
        endforeach()
        add_library(${_halname} ${_hal_objects})
        set_target_properties(${_halname} PROPERTIES LINKER_LANGUAGE C)
        add_dependencies(${_halname} psdk asm)
        return()
    endif()

    # Handle the spec file for the dll name
    spec2def(${_halname}.dll ../hal.spec ADD_IMPORTLIB)

    # Create the actual target
    foreach(_component ${_haldata_COMPONENTS})
        list(APPEND _haldata_SOURCES "$<TARGET_OBJECTS:lib_hal_${_component}>")
    endforeach()
    add_library(${_halname} MODULE
        ${_haldata_SOURCES}
        ${CMAKE_CURRENT_BINARY_DIR}/hal.def)

    if(${_halname} STREQUAL "hal")
        target_link_libraries(${_halname} libcntpr setjmp arbiter fast486 ${PSEH_LIB})
    else()
        target_link_libraries(${_halname} libcntpr setjmp fast486 ${PSEH_LIB})
    endif()

    add_importlibs(${_halname} ntoskrnl)
    #add_pch(${_halname} include/hal.h)
    add_dependencies(${_halname} psdk asm)
    set_module_type(${_halname} kerneldll ENTRYPOINT HalInitSystem 8)
    add_cd_file(TARGET ${_halname} DESTINATION reactos/system32 NO_CAB FOR all)
    if(MSVC)
        target_link_options(${_halname} PRIVATE /ignore:4216 /SECTION:INIT,ERWD)
    else()
        target_link_libraries(${_halname} -lgcc)
    endif()
endfunction()

if(ARCH STREQUAL "i386" AND SARCH STREQUAL "xbox")
    # halxbox COMPONENTS = xbox + up; neither references lib_hal_generic,
    # and the rest of the HAL variants (hal, halacpi, halaacpi, halapic,
    # halmacpi, halpc98) aren't reachable from the flash image, so we
    # also skip their component objects (acpi, apic, smp, legacy, pc98,
    # pic, generic).
    include(up.cmake)
    include(pcidata.cmake)
    include(xbox.cmake)
    remove_definitions(-DSARCH_XBOX)
    add_hal(halxbox COMPONENTS xbox up STATICLIB)
    return()
endif()

# The components
include(generic.cmake)
include(acpi.cmake)
include(apic.cmake)
include(up.cmake)
include(smp.cmake)

if(ARCH STREQUAL "i386")
    include(pcidata.cmake)
    include(legacy.cmake)
    include(pic.cmake)
    include(xbox.cmake)
    include(pc98.cmake)
    add_subdirectory(minihal)

    remove_definitions(-DSARCH_XBOX)
    remove_definitions(-DSARCH_PC98)

    # hal
    add_hal(hal SOURCES pic/halpic.rc COMPONENTS generic legacy up pic)
    add_hal(halacpi SOURCES acpi/halacpi.rc COMPONENTS generic acpi up pic)
    add_hal(halaacpi SOURCES apic/halaacpi.rc COMPONENTS generic acpi up apic)
    add_hal(halapic SOURCES apic/halapic.rc COMPONENTS generic legacy up apic)
    add_hal(halxbox COMPONENTS xbox up STATICLIB)
    add_hal(halpc98 SOURCES pc98/halpc98.rc COMPONENTS pc98 up)

    add_hal(halmacpi SOURCES smp/halmacpi.rc COMPONENTS generic acpi smp apic)
    #add_hal(halmp SOURCES mp/halmp.rc COMPONENTS generic legacy smp apic)

elseif(ARCH STREQUAL "amd64")

    add_hal(hal SOURCES ${HAL_SOURCE} COMPONENTS generic acpi up apic)
    add_hal(halmp SOURCES ${HAL_SOURCE} COMPONENTS generic acpi smp apic)

endif()
