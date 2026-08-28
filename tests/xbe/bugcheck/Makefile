# One-off XBE for the KeBugCheckEx ordinal -- see main.c.  It bugchecks
# on purpose, so it cannot be part of the api-regression suite.
XBE_TITLE = nxkrnl-bugcheck
GEN_XISO = $(XBE_TITLE).iso
NXDK_DIR ?= $(CURDIR)/../../../third_party/nxdk
SRCS = $(CURDIR)/main.c $(CURDIR)/../api-regression/tap.c
include $(NXDK_DIR)/Makefile
