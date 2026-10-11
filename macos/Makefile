# Copyright (c) 2026 Andy Hendrickson
# SPDX-License-Identifier: MIT
#
# Build the Code Annotator macOS port. Requires Xcode command line tools
# (xcode-select --install). Produces both a plain binary (also the --check
# CLI) and a double-clickable CodeAnnotator.app bundle.

APP      := CodeAnnotator
BIN      := build/code_annotator
CORE     := ../cpp/core.cpp ../cpp/highlight.cpp
HEADERS  := ../cpp/core.hpp ../cpp/highlight.hpp ../cpp/json.hpp
CXXFLAGS := -std=c++17 -O2 -Wall -I../cpp
MINVER   := -mmacosx-version-min=10.15

all: $(BIN) app

$(BIN): main.mm $(CORE) $(HEADERS)
	mkdir -p build
	clang++ $(CXXFLAGS) $(MINVER) -fobjc-arc main.mm $(CORE) \
	    -framework Cocoa -o $@

app: $(BIN) Info.plist
	rm -rf build/$(APP).app
	mkdir -p build/$(APP).app/Contents/MacOS
	cp Info.plist build/$(APP).app/Contents/Info.plist
	cp $(BIN) build/$(APP).app/Contents/MacOS/$(APP)

clean:
	rm -rf build

.PHONY: all app clean
