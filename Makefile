# Makefile for HGW-Doctor
#
# Builds a single shared library (hgw_doctor.so) loaded by amxrt.
#
# Usage:
#   make                          -> native host build
#   make DEBUG=1                  -> native debug build (-g -O0)
#   make CROSS_COMPILE=aarch64-openwrt-linux-musl- \
#        STAGING_DIR=/prplos-build/prplos/staging_dir/target-aarch64_cortex-a72_musl
#                                 -> cross-compile for RPi4 / PrplOS
#
#   amxrt odl/hgw-doctor.odl     -> run the plugin
#
# Design note:
#   CFLAGS / LDFLAGS are kept as pass-through variables so the OpenWrt build
#   system can inject hardening flags via the command line.
#   Project-specific flags live in EXTRA_CFLAGS / EXTRA_LDFLAGS.

CC := $(CROSS_COMPILE)gcc

# If STAGING_DIR is set (cross-compile) use it; otherwise fall back to system.
ifdef STAGING_DIR
    AMX_INC  := $(STAGING_DIR)/usr/include
    AMX_LDIR := $(STAGING_DIR)/usr/lib
else
    AMX_INC  := /usr/include
    AMX_LDIR :=
endif

DEBUG_FLAGS := $(if $(DEBUG),-g -O0,-O2)

# All objects go into the .so → every TU needs -fPIC.
EXTRA_CFLAGS := -Wall -Wextra -Werror -fPIC \
                $(DEBUG_FLAGS) \
                -I./include_priv \
                -I$(AMX_INC) \
                -D_GNU_SOURCE

EXTRA_LDFLAGS := $(if $(AMX_LDIR),-L$(AMX_LDIR),) \
                 -lamxd -lamxo -lamxc -lamxp -lamxb \
                 -shared -fPIC

# ── Sources: only the five new plugin files ──────────────────────────────────
SRCS := src/hgwdoctor_main.c \
        src/hgwd_metrics.c \
        src/hgwd_events.c \
        src/hgwd_recovery.c \
        src/hgwd_dm_methods.c

OBJ_DIR := build/obj
LIB_DIR := build/lib
OBJS    := $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(SRCS))
SHARED  := $(LIB_DIR)/hgw_doctor.so

.PHONY: all clean install

all: $(SHARED)

$(OBJ_DIR)/%.o: src/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -c $< -o $@

$(SHARED): $(OBJS) | $(LIB_DIR)
	$(CC) $(LDFLAGS) $(EXTRA_LDFLAGS) \
	      -Wl,-soname,hgw_doctor.so \
	      $(OBJS) -o $@

$(OBJ_DIR) $(LIB_DIR):
	mkdir -p $@

clean:
	rm -rf build/

install: all
	install -D -m 755 $(SHARED)                         /usr/lib/hgw_doctor.so
	install -D -m 644 odl/hgw-doctor.odl               /etc/amx/hgw-doctor/hgw-doctor.odl
	install -D -m 644 odl/hgw-doctor_definition.odl    /etc/amx/hgw-doctor/hgw-doctor_definition.odl
