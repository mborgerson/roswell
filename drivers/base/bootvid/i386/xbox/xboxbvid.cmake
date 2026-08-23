
set(MODULE_HEADER ${CMAKE_CURRENT_LIST_DIR}/xbox.h) # For precomp.h
list(APPEND SOURCE
    ${MODULE_HEADER}
    ${CMAKE_CURRENT_LIST_DIR}/bootvid.c)

# Linked into the kernel image; lets boot-only entry points join the
# kernel's discardable INIT section.
if(SARCH STREQUAL "xbox")
    list(APPEND COMPILE_DEFINITIONS SARCH_XBOX)
endif()

set(REACTOS_STR_FILE_DESCRIPTION "Original Xbox Boot Video Driver")
