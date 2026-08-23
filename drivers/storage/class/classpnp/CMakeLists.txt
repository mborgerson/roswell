
remove_definitions(-D_WIN32_WINNT=0x502)

list(APPEND SOURCE
    autorun.c
    class.c
    clntirp.c
    create.c
    data.c
    debug.c
    dictlib.c
    dispatch.c
    guid.c
    history.c
    lock.c
    obsolete.c
    retry.c
    srblib.c
    utils.c
    xferpkt.c)

if(NOT SARCH STREQUAL "xbox")
    # power.c: ClassDispatchPower stubbed; no PoCallDriver / Po settings ever fire.
    # classwmi.c: no WMI/IRP_MJ_SYSTEM_CONTROL surface on Xbox.
    list(APPEND SOURCE power.c classwmi.c)
endif()

# the storage class library is linked into the kernel image as a
# static library -- disk (and the deferred cdrom) call its Class* ABI
# directly.
add_library(classpnp ${SOURCE})

# PRIVATE (not PUBLIC): these must not leak into ntoskrnl's own compilation
# now that classpnp is linked into the kernel image.
target_compile_definitions(classpnp PRIVATE
    DEBUG_USE_KDPRINT
    _WIN32_WINNT=0x602
    NTDDI_VERSION=0x06020000 # NTDDI_WIN8
    DriverEntry=ClasspnpDriverEntry)

target_compile_definitions(classpnp PRIVATE
    CLASS_GLOBAL_BREAK_ON_LOST_IRPS=0
    CLASS_GLOBAL_SECONDS_TO_WAIT_FOR_SYNCHRONOUS_SRB=100
    CLASS_GLOBAL_USE_DELAYED_RETRY=1
    CLASS_GLOBAL_BUFFERED_DEBUG_PRINT=0
    CLASS_GLOBAL_BUFFERED_DEBUG_PRINT_BUFFER_SIZE=512
    CLASS_GLOBAL_BUFFERED_DEBUG_PRINT_BUFFERS=512)

if(NOT MSVC)
    target_compile_options(classpnp PRIVATE -Wno-pointer-to-int-cast -Wno-switch)
endif()

if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    target_compile_options(classpnp PRIVATE -Wno-unused-but-set-variable)
endif()

target_link_libraries(classpnp ntoskrnl_vista libcntpr ${PSEH_LIB})
add_dependencies(classpnp psdk xdk bugcodes)
