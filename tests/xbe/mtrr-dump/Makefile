# mtrr-dump test XBE -- dumps the MTRRs over the debug UART.  Builds an XISO
# too, so it can boot on the official kernel under xemu (run-xemu --official
# --dvd).  See main.c.
XBE_TITLE = nxkrnl-mtrr-dump
GEN_XISO = $(XBE_TITLE).iso
NXDK_DIR ?= $(CURDIR)/../../../third_party/nxdk
SRCS = $(CURDIR)/main.c
include $(NXDK_DIR)/Makefile
