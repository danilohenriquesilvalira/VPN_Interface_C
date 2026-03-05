# Makefile - RLS Automacao VPN v1.3.0 - MinGW-w64 build (C + C++ / WebView2)
# Usage:
#   make                       - build rls_vpn.exe
#   make WEBVIEW2INC=path/to   - build with custom WebView2.h path
#   make clean                 - remove build artifacts
#   make install               - copy to Program Files (requires admin)

CC      = gcc
WINDRES = windres
TARGET  = rls_vpn.exe

SRCS    = src/main.c src/vpn.c
OBJS    = src/main.o src/vpn.o src/resource.o

CFLAGS  = -Wall -Wextra -O2 -Isrc -mwindows
LDFLAGS = -lcomctl32 -lgdi32 -lshcore -lole32 -luuid -lgdiplus -lshell32 -liphlpapi -lws2_32

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -mwindows -o $@ $^ $(LDFLAGS)
	@echo Built: $(TARGET)

src/main.o: src/main.c src/vpn.h
	$(CC) $(CFLAGS) -c -o $@ $<

src/vpn.o: src/vpn.c src/vpn.h
	$(CC) $(CFLAGS) -c -o $@ $<

src/resource.o: src/resource.rc src/app.manifest
	$(WINDRES) src/resource.rc -O coff -o $@

clean:
	-del /f src\*.o $(TARGET) 2>NUL
	@echo Clean done.

install: $(TARGET)
	copy /Y $(TARGET) "%PROGRAMFILES%\RLS Automacao\VPN\$(TARGET)"
