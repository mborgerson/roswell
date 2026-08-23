
# kdcom -- the kernel debugger transport.  Built as a static library and
# linked into the kernel rather than imported as a separate kerneldll.
# _NTSYSTEM_: ntoskrnl APIs resolve as direct symbols, not DLL imports.
# kdcom's own functions (kddll.h) carry no import decoration, so ntoskrnl
# already references them directly.
add_library(kdcom kdrosdbg.c)
target_compile_definitions(kdcom PRIVATE _NTSYSTEM_)
target_link_libraries(kdcom cportlib)
add_dependencies(kdcom psdk xdk bugcodes)
