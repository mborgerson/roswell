
# the WMI helper library is linked into the kernel image as a static
# library -- atapi (and other drivers) call its Wmi* helpers directly.
add_library(wmilib wmilib.c)
add_dependencies(wmilib psdk xdk bugcodes)
