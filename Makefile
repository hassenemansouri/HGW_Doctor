# Makefile for HGW-Doctor
# Supports two targets:
#   make           -> native x86_64 host build (for unit tests)
#   make TARGET=arm -> cross-compile for Raspberry Pi 4 (ARM Cortex-A72)

CC      ?= gcc
AR      ?= ar
TARGET  ?= host

ifeq ($(TARGET), arm)
    CC       = aarch64-linux-gnu-gcc
    SYSROOT ?= /opt/raspberry-pi-sysroot
    CFLAGS  += --sysroot=$(SYSROOT)
    LDFLAGS += --sysroot=$(SYSROOT)
endif

CFLAGS  += -Wall -Wextra -Werror -g -O2 \
           -I./include \
           -I/usr/include/amxd \
           -I/usr/include/amxo \
           -I/usr/include/amxc \
           -D_GNU_SOURCE

LDFLAGS += -lamxd -lamxo -lamxc -lamxp -lamxs \
           -lcurl -lpthread -lm

SRC_DIR  = src
OBJ_DIR  = build/obj
BIN_DIR  = build/bin
SO_DIR   = build/lib

SRCS     = $(wildcard $(SRC_DIR)/*.c)
OBJS     = $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o, $(SRCS))
DAEMON   = $(BIN_DIR)/hgw-doctor
SHARED   = $(SO_DIR)/hgw_doctor.so   # loaded by libamxo at ODL parse time

.PHONY: all clean install test

all: $(DAEMON) $(SHARED)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Main daemon binary (links everything except datamodel which goes in .so)
$(DAEMON): $(filter-out $(OBJ_DIR)/datamodel.o, $(OBJS)) | $(BIN_DIR)
	$(CC) $^ -o $@ $(LDFLAGS)

# Shared library containing Ambiorix entry-point callbacks
$(SHARED): $(OBJ_DIR)/datamodel.o | $(SO_DIR)
	$(CC) -shared -fPIC $< -o $@ $(LDFLAGS)

$(OBJ_DIR) $(BIN_DIR) $(SO_DIR):
	mkdir -p $@

clean:
	rm -rf build/

install: all
	install -D -m 755 $(DAEMON) /usr/sbin/hgw-doctor
	install -D -m 644 $(SHARED) /usr/lib/hgw_doctor.so
	install -D -m 644 conf/hgw_doctor.conf /etc/hgw_doctor/hgw_doctor.conf
	install -D -m 644 odl/hgw_doctor.odl   /etc/amx/hgw_doctor/hgw_doctor.odl
	install -D -m 644 odl/hgw_doctor_defaults.odl /etc/amx/hgw_doctor/hgw_doctor_defaults.odl
	install -D -m 644 odl/hgw_doctor_caps.odl     /etc/amx/hgw_doctor/hgw_doctor_caps.odl
	install -D -m 644 init/hgw-doctor.service /lib/systemd/system/hgw-doctor.service
	install -D -m 755 actions/restart_process.sh /usr/lib/hgw_doctor/actions/restart_process.sh
	install -D -m 755 actions/clear_cache.sh     /usr/lib/hgw_doctor/actions/clear_cache.sh
	systemctl daemon-reload

test:
	$(MAKE) -C tests/
