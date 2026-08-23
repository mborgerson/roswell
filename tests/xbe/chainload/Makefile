# Chainload test -- the loader XBE + the bundled ISO.
#
# Builds two XBEs and packs them into one xiso:
#   default.xbe  the loader (calls XLaunchXBE on target.xbe)
#   target.xbe   the target (chainloaded by the loader)
#
# extract-xiso packs everything in OUTPUT_DIR/ into the iso, so we add
# target.xbe as a prerequisite to the iso rule and stage it next to
# default.xbe before extract-xiso runs.
XBE_TITLE = chainload-loader
GEN_XISO  = $(XBE_TITLE).iso
NXDK_DIR ?= $(CURDIR)/../../../third_party/nxdk
SRCS      = $(CURDIR)/loader.c

include $(NXDK_DIR)/Makefile

# Build the target XBE in its own subdir and stage it into our bin/.
TARGET_XBE := $(CURDIR)/target/bin/default.xbe
$(OUTPUT_DIR)/target.xbe: $(TARGET_XBE) | $(OUTPUT_DIR)
	@echo "[ STAGE    ] $@"
	$(VE)cp $(TARGET_XBE) $@

$(TARGET_XBE):
	@echo "[ MAKE     ] target/"
	$(VE)$(MAKE) -C $(CURDIR)/target

# Pull target.xbe into the iso's input dir before extract-xiso runs.
$(GEN_XISO): $(OUTPUT_DIR)/target.xbe
