
add_subdirectory(nxldr)

# boot/bootdata + boot/rtl + freeldr produce the ReactOS install CD images
# (registry hives, blrtl, freeldr); none of it is reachable from the Xbox
# flash image, which boots directly into nxldr -> xboxkrnl.
if(NOT SARCH STREQUAL "xbox")
    include(boot_images.cmake)
    add_subdirectory(bootdata)
    add_subdirectory(freeldr)
    add_subdirectory(rtl)

    add_cd_file(FILE ${CMAKE_CURRENT_SOURCE_DIR}/remaster.cmd DESTINATION loader NO_CAB FOR bootcd)
    add_cd_file(FILE ${CMAKE_CURRENT_SOURCE_DIR}/remaster.sh DESTINATION loader NO_CAB FOR bootcd)
endif()
