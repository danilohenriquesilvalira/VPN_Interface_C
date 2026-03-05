# Makefile - RLS Automacao VPN v1.3.0 - MinGW-w64 build (C + C++ / WebView2)
# Usage:
#   make                       - build rls_vpn.exe
#   make WEBVIEW2INC=path/to   - build with custom WebView2.h path
#   make clean                 - remove build artifacts
#   make install               - copy to Program Files (requires admin)

CC      = gcc
CXX     = g++
WINDRES = windres
TARGET  = rls_vpn.exe

# WebView2.h location (overrideable by CI: make WEBVIEW2INC=src)
WEBVIEW2INC ?= src

SRCS_C   = src/main.c src/vpn.c
SRCS_CPP = src/webview_bridge.cpp
OBJS     = src/main.o src/vpn.o src/webview_bridge.o src/resource.o

CFLAGS   = -Wall -Wextra -O2 -Isrc -I$(WEBVIEW2INC) -mwindows
CXXFLAGS = -Wall -Wextra -O2 -Isrc -I$(WEBVIEW2INC) -mwindows -std=c++17
LDFLAGS  = -lcomctl32 -lgdi32 -lshcore -lole32 -lgdiplus -lstdc++ -lshell32

.PHONY: all clean install

all: $(TARGET)

# Link with g++ to handle C++ symbols from webview_bridge.o
$(TARGET): $(OBJS)
	$(CXX) -mwindows -o $@ $^ $(LDFLAGS)
	@echo Built: $(TARGET)

src/main.o: src/main.c src/vpn.h src/webview_bridge.h
	$(CC) $(CFLAGS) -c -o $@ $<

src/vpn.o: src/vpn.c src/vpn.h
	$(CC) $(CFLAGS) -c -o $@ $<

src/webview_bridge.o: src/webview_bridge.cpp src/webview_bridge.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

src/resource.o: src/resource.rc src/app.manifest src/ui.html
	$(WINDRES) src/resource.rc -O coff -o $@

clean:
	-del /f src\*.o $(TARGET) 2>NUL
	@echo Clean done.

install: $(TARGET)
	copy /Y $(TARGET) "%PROGRAMFILES%\RLS Automacao\VPN\$(TARGET)"
