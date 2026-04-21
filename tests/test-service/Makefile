CC      := $(CROSS_COMPILE)gcc

ifdef STAGING_DIR
    INC  := -I$(STAGING_DIR)/usr/include
    LDIR := -L$(STAGING_DIR)/usr/lib
else
    INC  :=
    LDIR :=
endif

CFLAGS  += -Wall -Wextra $(INC) -D_GNU_SOURCE
LDFLAGS += $(LDIR) -lubus -lubox -lpthread

BIN_DIR := ../../build/bin
TARGET  := $(BIN_DIR)/test-service

.PHONY: all clean

all: $(TARGET)

$(BIN_DIR):
	mkdir -p $@

$(TARGET): src/main.c | $(BIN_DIR)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

clean:
	rm -f $(TARGET)
