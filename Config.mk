# Convenient variables
comma := ,

# Compare $(1) and $(2) and replace $(2) with $(1) if they differ
#
# Typically $(1) is a newly generated file and $(2) is the target file
# being regenerated. This prevents changing the timestamp of $(2) only
# due to being auto regenereated with the same contents.
define move-if-changed
        if ! cmp -s $(1) $(2); then mv -f $(1) $(2); else rm -f $(1); fi
endef

# cc-option: Check if compiler supports first option, else fall back to second.
#
# This is complicated by the fact that unrecognised -Wno-* options:
#   (a) are ignored unless the compilation emits a warning; and
#   (b) even then produce a warning rather than an error
# To handle this we do a test compile, passing the option-under-test, on a code
# fragment that will always produce a warning (integer assigned to pointer).
# We then grep for the option-under-test in the compiler's output, the presence
# of which would indicate an "unrecognized command-line option" warning/error.
#
# Usage: cflags-y += $(call cc-option,$(CC),-march=winchip-c6,-march=i586)
cc-option = $(shell if test -z "`echo 'void*p=1;' | \
              $(1) $(2) -c -o /dev/null -x c - 2>&1 | grep -- $(2) -`"; \
              then echo "$(2)"; else echo "$(3)"; fi ;)

ifneq ($(MINIOS_CONFIG),)
EXTRA_DEPS += $(MINIOS_CONFIG)
include $(MINIOS_CONFIG)
endif

# Compatibility with Xen's stubdom build environment.  If we are building
# stubdom, some XEN_ variables are set, set MINIOS_ variables accordingly.
#
ifneq ($(XEN_ROOT),)
MINIOS_ROOT=$(XEN_ROOT)/extras/mini-os

-include $(XEN_ROOT)/stubdom/mini-os.mk

XENSTORE_CPPFLAGS ?= -isystem $(XEN_ROOT)/tools/xenstore/include
TOOLCORE_PATH ?= $(XEN_ROOT)/stubdom/libs-$(MINIOS_TARGET_ARCH)/toolcore
TOOLLOG_PATH ?= $(XEN_ROOT)/stubdom/libs-$(MINIOS_TARGET_ARCH)/toollog
EVTCHN_PATH ?= $(XEN_ROOT)/stubdom/libs-$(MINIOS_TARGET_ARCH)/evtchn
GNTTAB_PATH ?= $(XEN_ROOT)/stubdom/libs-$(MINIOS_TARGET_ARCH)/gnttab
CALL_PATH ?= $(XEN_ROOT)/stubdom/libs-$(MINIOS_TARGET_ARCH)/call
FOREIGNMEMORY_PATH ?= $(XEN_ROOT)/stubdom/libs-$(MINIOS_TARGET_ARCH)/foreignmemory
DEVICEMODEL_PATH ?= $(XEN_ROOT)/stubdom/libs-$(MINIOS_TARGET_ARCH)/devicemodel
MANAGE_PATH ?= $(XEN_ROOT)/stubdom/libs-$(MINIOS_TARGET_ARCH)/manage
CTRL_PATH ?= $(XEN_ROOT)/stubdom/libxc-$(MINIOS_TARGET_ARCH)
GUEST_PATH ?= $(XEN_ROOT)/stubdom/libxc-$(MINIOS_TARGET_ARCH)
else
MINIOS_ROOT=$(TOPLEVEL_DIR)
endif
export MINIOS_ROOT

ifneq ($(XEN_TARGET_ARCH),)
MINIOS_TARGET_ARCH = $(XEN_TARGET_ARCH)
else
MINIOS_COMPILE_ARCH    ?= $(shell uname -m | sed -e s/i.86/x86_32/ \
                            -e s/i86pc/x86_32/ -e s/amd64/x86_64/ \
                            -e s/armv7.*/arm32/ -e s/armv8.*/arm64/ \
                            -e s/aarch64/arm64/)

MINIOS_TARGET_ARCH     ?= $(MINIOS_COMPILE_ARCH)
endif

stubdom ?= n
libc = $(stubdom)

XEN_INTERFACE_VERSION ?= 0x00030205
export XEN_INTERFACE_VERSION

# Try to find out the architecture family TARGET_ARCH_FAM.
# First check whether x86_... is contained (for x86_32, x86_32y, x86_64).
# If not x86 then use $(MINIOS_TARGET_ARCH)
ifeq ($(findstring x86_,$(MINIOS_TARGET_ARCH)),x86_)
TARGET_ARCH_FAM = x86
else
TARGET_ARCH_FAM = $(MINIOS_TARGET_ARCH)
endif

# The architecture family directory below mini-os.
TARGET_ARCH_DIR := arch/$(TARGET_ARCH_FAM)

# Export these variables for possible use in architecture dependent makefiles.
export TARGET_ARCH_DIR
export TARGET_ARCH_FAM

# Set tools
AS         = $(CROSS_COMPILE)as
LD         = $(CROSS_COMPILE)ld
ifeq ($(clang),y)
CC         = $(CROSS_COMPILE)clang
LD_LTO     = $(CROSS_COMPILE)llvm-ld
else
CC         = $(CROSS_COMPILE)gcc
LD_LTO     = $(CROSS_COMPILE)ld
endif
CPP        = $(CC) -E
AR         = $(CROSS_COMPILE)ar
RANLIB     = $(CROSS_COMPILE)ranlib
NM         = $(CROSS_COMPILE)nm
STRIP      = $(CROSS_COMPILE)strip
OBJCOPY    = $(CROSS_COMPILE)objcopy
OBJDUMP    = $(CROSS_COMPILE)objdump
SIZEUTIL   = $(CROSS_COMPILE)size

# Allow git to be wrappered in the environment
GIT        ?= git

INSTALL      = install
INSTALL_DIR  = $(INSTALL) -d -m0755 -p
INSTALL_DATA = $(INSTALL) -m0644 -p
INSTALL_PROG = $(INSTALL) -m0755 -p

# This is used for architecture specific links.
# This can be overwritten from arch specific rules.
ARCH_LINKS =

# The path pointing to the architecture specific header files.
ARCH_INC := $(TARGET_ARCH_FAM)

# For possible special header directories.
# This can be overwritten from arch specific rules.
EXTRA_INC = $(ARCH_INC)	

# Include the architecture family's special makerules.
# This must be before include minios.mk!
include $(MINIOS_ROOT)/$(TARGET_ARCH_DIR)/arch.mk

extra_incl := $(foreach dir,$(EXTRA_INC),-isystem $(MINIOS_ROOT)/include/$(dir))

DEF_CPPFLAGS += -isystem $(MINIOS_ROOT)/include
DEF_CPPFLAGS += -D__MINIOS__

ifeq ($(libc),y)
DEF_CPPFLAGS += -DHAVE_LIBC
DEF_CPPFLAGS += -isystem $(MINIOS_ROOT)/include/posix
DEF_CPPFLAGS += $(XENSTORE_CPPFLAGS)
endif

ifneq ($(LWIPDIR),)
lwip=y
DEF_CPPFLAGS += -DHAVE_LWIP
DEF_CPPFLAGS += -isystem $(LWIPDIR)/src/include
DEF_CPPFLAGS += -isystem $(LWIPDIR)/src/include/ipv4
endif

BOOT_DIR ?= /boot

SOCKET_LIBS =
UTIL_LIBS = -lutil
DLOPEN_LIBS = -ldl

SONAME_LDFLAG = -soname
SHLIB_LDFLAGS = -shared

ifneq ($(debug),y)
CFLAGS += -O2 -fomit-frame-pointer
else
# Less than -O1 produces bad code and large stack frames
CFLAGS += -O1 -fno-omit-frame-pointer
CFLAGS-$(gcc) += -fno-optimize-sibling-calls
endif

ifeq ($(lto),y)
CFLAGS += -flto
LDFLAGS-$(clang) += -plugin LLVMgold.so
endif

# When adding a new CONFIG_ option please make sure the test configurations
# under arch/*/testbuild/ are updated accordingly. Especially
# arch/*/testbuild/*-yes and arch/*/testbuild/*-no should set ALL possible
# CONFIG_ variables.

# Configuration defaults:
# CONFIG-y contains all items defaulting to "y"
# CONFIG-n contains all items defaulting to "n"
# CONFIG-x contains all items being calculated if not set explicitly
CONFIG-y += CONFIG_START_NETWORK
CONFIG-y += CONFIG_SPARSE_BSS
CONFIG-y += CONFIG_BLKFRONT
CONFIG-y += CONFIG_NETFRONT
CONFIG-y += CONFIG_FBFRONT
CONFIG-y += CONFIG_KBDFRONT
CONFIG-y += CONFIG_CONSFRONT
CONFIG-y += CONFIG_XENBUS
CONFIG-n += CONFIG_QEMU_XS_ARGS
CONFIG-n += CONFIG_TEST
CONFIG-n += CONFIG_PCIFRONT
CONFIG-n += CONFIG_TPMFRONT
CONFIG-n += CONFIG_9PFRONT
CONFIG-n += CONFIG_TPM_TIS
CONFIG-n += CONFIG_TPMBACK
CONFIG-n += CONFIG_BALLOON
CONFIG-n += CONFIG_LIBXENCALL
CONFIG-n += CONFIG_LIBXENCTRL
CONFIG-n += CONFIG_LIBXENDEVICEMODEL
CONFIG-n += CONFIG_LIBXENEVTCHN
CONFIG-n += CONFIG_LIBXENFOREIGNMEMORY
CONFIG-n += CONFIG_LIBXENGNTTAB
CONFIG-n += CONFIG_LIBXENGUEST
CONFIG-n += CONFIG_LIBXENTOOLCORE
CONFIG-n += CONFIG_LIBXENTOOLLOG
CONFIG-n += CONFIG_LIBXENMANAGE
CONFIG-n += CONFIG_KEXEC
# Setting CONFIG_USE_XEN_CONSOLE copies all print output to the Xen emergency
# console apart of standard dom0 handled console.
CONFIG-n += CONFIG_USE_XEN_CONSOLE
ifeq ($(TARGET_ARCH_FAM),x86)
CONFIG-y += CONFIG_PARAVIRT
else
CONFIG-n += CONFIG_PARAVIRT
endif

CONFIG-$(lwip) += CONFIG_LWIP

$(foreach i,$(CONFIG-y),$(eval $(i) ?= y))
$(foreach i,$(CONFIG-n),$(eval $(i) ?= n))

CONFIG-x += CONFIG_LIBXS
CONFIG_LIBXS ?= $(CONFIG_XENBUS)

CONFIG-all := $(CONFIG-y) $(CONFIG-n) $(CONFIG-x)

# Export config items as compiler directives
$(foreach i,$(CONFIG-all),$(eval DEFINES-$($(i)) += -D$(i)))

DEFINES-y += -D__XEN_INTERFACE_VERSION__=$(XEN_INTERFACE_VERSION)

# Override settings for this OS
PTHREAD_LIBS =
nosharedlibs=y
