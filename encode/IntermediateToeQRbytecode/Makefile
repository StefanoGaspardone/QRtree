# Builds the c2_exhaustive shared library used by compression.py (via ctypes).
#
#   make          build the library for the current OS
#   make clean    remove the built library
#
# The output name depends on the platform, matching what compression.py looks for:
#   Linux   -> libc2.so
#   macOS   -> libc2.dylib
#   Windows -> libc2.dll (MinGW-w64, e.g. MSYS2)

CC      = gcc
CFLAGS  ?= -O3
SRC     := c2_exhaustive.c
LDLIBS  := -lm

ifeq ($(OS),Windows_NT)
    TARGET  := libc2.dll
    LDFLAGS := -shared -pthread -static
    RM      := del /Q
else
    UNAME_S := $(shell uname -s)
    ifeq ($(UNAME_S),Darwin)
        TARGET := libc2.dylib
    else
        TARGET := libc2.so
    endif
    CFLAGS  += -fPIC
    LDFLAGS := -shared -pthread
    RM      := rm -f
endif

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

clean:
	-$(RM) $(TARGET)