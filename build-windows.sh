#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
ZIG="${ZIG:-zig}"
mkdir -p build dist/Windows
(cd assets && xxd -i ui.html && xxd -i commands.json) > build/embedded.h
"$ZIG" c++ -target x86_64-windows-gnu -std=c++17 -O2 -s -Ibuild \
  src/main.cpp assets/app.rc \
  -lws2_32 -lshell32 -ladvapi32 -lbcrypt \
  -Wl,--subsystem,windows \
  -o "dist/Windows/Arya Terminal Assistant.exe"
echo "Built: dist/Windows/Arya Terminal Assistant.exe"
