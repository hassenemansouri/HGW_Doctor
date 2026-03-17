# Makefile for HGW-Doctor
#
# Usage:
#   make                          -> native host build
#   make CROSS_COMPILE=aarch64-openwrt-linux-musl- \
#        STAGING_DIR=/prplos-build/prplos/staging_dir/target-aarch64_cortex-a72_musl
#                               -> cross-compile for RPi4 using PrplOS toolchain
#
# Design note:
#   CFLAGS / LDFLAGS are intentionally kept as pass-through variables so that
#   the OpenWrt build system can inject its hardening flags via the command line.
#   Project-specific flags (include paths, libraries) live in EXTRA_CFLAGS /
#   EXTRA_LDFLAGS so they are always appended regardless of how CFLAGS is set.

CC      := $(CROSS_COMPILE)gcc
AR      := $(CROSS_COMPILE)ar

# If STAGING_DIR is set (cross-compile) use it; fall back to system paths.
ifdef STAGING_DIR
    AMX_INC  := $(STAGING_DIR)/usr/include
    AMX_LDIR := $(STAGING_DIR)/usr/lib
else
    AMX_INC  := /usr/include
    AMX_LDIR :=
endif

# Always-added flags (not overridable by command-line CFLAGS)
# Note: headers are included as <amxd/amxd_dm.h>, <amxc/amxc_var.h> etc.
# so the -I path must point to the *parent* directory (usr/include), not
# the subdirectory (usr/include/amxd).
EXTRA_CFLAGS  := -Wall -Wextra -Werror \
                 -I./include \
                 -I$(AMX_INC) \
                 -D_GNU_SOURCE

EXTRA_LDFLAGS := $(if $(AMX_LDIR),-L$(AMX_LDIR),) \
                 -lamxd -lamxo -lamxc -lamxp -lamxs \
                 -lcurl -lpthread -lm

SRC_DIR  = src
OBJ_DIR  = build/obj
BIN_DIR  = build/bin
SO_DIR   = build/lib

SRCS     = $(wildcard $(SRC_DIR)/*.c)
OBJS     = $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o, $(SRCS))
DAEMON   = $(BIN_DIR)/hgw-doctor
SHARED   = $(SO_DIR)/hgw_doctor.so

.PHONY: all clean install test

all: $(DAEMON) $(SHARED)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -c $< -o $@

# Main daemon binary (all objects except datamodel which goes in .so)
# $(SHARED) is built first (prerequisite), then passed directly on the link
# line so the linker resolves datamodel_* symbols without needing lib prefix.
$(DAEMON): $(filter-out $(OBJ_DIR)/datamodel.o, $(OBJS)) $(SHARED) | $(BIN_DIR)
	$(CC) $(filter %.o, $^) $(SHARED) -o $@ $(LDFLAGS) $(EXTRA_LDFLAGS) -rdynamic

# Shared library containing Ambiorix entry-point callbacks
$(SHARED): $(OBJ_DIR)/datamodel.o | $(SO_DIR)
	$(CC) -shared -fPIC -Wl,-soname,hgw_doctor.so $< -o $@ $(LDFLAGS) $(EXTRA_LDFLAGS)

$(OBJ_DIR) $(BIN_DIR) $(SO_DIR):
	mkdir -p $@

clean:
	rm -rf build/

install: all
	install -D -m 755 $(DAEMON)  /usr/sbin/hgw-doctor
	install -D -m 644 $(SHARED)  /usr/lib/hgw_doctor.so
	install -D -m 644 conf/hgw_doctor.conf         /etc/hgw_doctor/hgw_doctor.conf
	install -D -m 644 odl/hgw_doctor.odl           /etc/amx/hgw_doctor/hgw_doctor.odl
	install -D -m 644 odl/hgw_doctor_defaults.odl  /etc/amx/hgw_doctor/hgw_doctor_defaults.odl
	install -D -m 644 odl/hgw_doctor_caps.odl      /etc/amx/hgw_doctor/hgw_doctor_caps.odl
	install -D -m 755 actions/restart_process.sh   /usr/lib/hgw_doctor/actions/restart_process.sh
	install -D -m 755 actions/clear_cache.sh       /usr/lib/hgw_doctor/actions/clear_cache.sh

test:
	$(MAKE) -C tests/
