
CC      := $(CROSS_COMPILE)gcc
AR      := $(CROSS_COMPILE)ar

ifdef STAGING_DIR
    AMX_INC  := $(STAGING_DIR)/usr/include
    AMX_LDIR := $(STAGING_DIR)/usr/lib
else
    AMX_INC  := /usr/include
    AMX_LDIR :=
endif

EXTRA_CFLAGS  := -Wall -Wextra -Werror \
                 -I./include \
                 -I$(AMX_INC) \
                 -D_GNU_SOURCE

EXTRA_LDFLAGS := $(if $(AMX_LDIR),-L$(AMX_LDIR),) \
                 -lamxd -lamxo -lamxc -lamxp -lamxb \
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

# datamodel.o must be compiled with -fPIC because it is linked into hgw_doctor.so
$(OBJ_DIR)/datamodel.o: $(SRC_DIR)/datamodel.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -fPIC -c $< -o $@

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -c $< -o $@

# Main daemon: all objects except datamodel.o, linked against the .so
$(DAEMON): $(filter-out $(OBJ_DIR)/datamodel.o, $(OBJS)) $(SHARED) | $(BIN_DIR)
	$(CC) $(filter %.o, $^) $(SHARED) -o $@ $(LDFLAGS) $(EXTRA_LDFLAGS) -rdynamic

# Shared library: only datamodel.o (Ambiorix callbacks)
$(SHARED): $(OBJ_DIR)/datamodel.o | $(SO_DIR)
	$(CC) -shared -fPIC -Wl,-soname,hgw_doctor.so $< -o $@ $(LDFLAGS) $(EXTRA_LDFLAGS)

$(OBJ_DIR) $(BIN_DIR) $(SO_DIR):
	mkdir -p $@

clean:
	rm -rf build/

install: all
	install -D -m 755 $(DAEMON)                            /usr/sbin/hgw-doctor
	install -D -m 644 $(SHARED)                            /usr/lib/hgw_doctor.so
	mkdir -p /etc/amx/hgw_doctor
	ln -sf /usr/lib/hgw_doctor.so                          /etc/amx/hgw_doctor/hgw_doctor.so
	install -D -m 644 conf/hgw_doctor.conf                 /etc/hgw_doctor/hgw_doctor.conf
	install -D -m 644 odl/hgw_doctor.odl                   /etc/amx/hgw_doctor/hgw_doctor.odl
	install -D -m 644 odl/hgw_doctor_defaults.odl          /etc/amx/hgw_doctor/hgw_doctor_defaults.odl
	install -D -m 644 odl/hgw_doctor_caps.odl              /etc/amx/hgw_doctor/hgw_doctor_caps.odl
	install -D -m 644 odl/hgw_doctor_tr181.odl             /etc/amx/hgw_doctor/hgw_doctor_tr181.odl
	install -D -m 755 actions/restart_process.sh           /usr/lib/hgw_doctor/actions/restart_process.sh
	install -D -m 755 actions/clear_cache.sh               /usr/lib/hgw_doctor/actions/clear_cache.sh

test:
	$(MAKE) -C tests/
