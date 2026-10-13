CC ?= gcc
CFLAGS = -Wall -Wextra -std=c11

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    CFLAGS += -I/opt/homebrew/include
    LDFLAGS += -L/opt/homebrew/lib -lX11
else
    CFLAGS += -I/usr/include/X11
    LDFLAGS += -lX11
endif

TARGET = terminal
SRC = terminal.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) $(LDFLAGS) -o $(TARGET)

clean:
	rm -f $(TARGET)
