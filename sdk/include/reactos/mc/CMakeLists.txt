
list(APPEND ANSI_SOURCE
     bugcodes.mc)

if(SARCH STREQUAL "xbox")
    # Kernel-only build pulls just the two .mc headers that our linked
    # sources include: ntiologc + ntstatus.
    list(APPEND UNICODE_SOURCE
         ntiologc.mc
         ntstatus.mc)
else()
    list(APPEND UNICODE_SOURCE
         errcodes.mc
         net_msg.mc
         neteventmsg.mc
         netmsgmsg.mc
         ntiologc.mc
         ntstatus.mc
         pciclass.mc)

    list(APPEND UNICODE_SOURCE_REALLY
         bootmsg.mc
         sacmsg.mc)
endif()

add_message_headers(ANSI ${ANSI_SOURCE})
# FIXME: this needs testing before switching to unicode
add_message_headers(UNICODE ${UNICODE_SOURCE})
if(UNICODE_SOURCE_REALLY)
    add_message_headers(UNICODE ${UNICODE_SOURCE_REALLY})
endif()
