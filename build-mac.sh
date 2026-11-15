#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
VERSION="1.1.0"
mkdir -p build dist/macOS
(cd assets && xxd -i ui.html && xxd -i commands.json) > build/embedded.h
FLAGS="-O2 -Wall -Wextra -arch arm64 -arch x86_64 -mmacosx-version-min=10.15"
clang++ $FLAGS -std=c++17 -Ibuild -c src/main.cpp -o build/main.o
clang++ $FLAGS -std=c++17 -fobjc-arc -c src/mac_window.mm -o build/mac_window.o
clang++ $FLAGS build/main.o build/mac_window.o -framework Cocoa -framework WebKit -o build/arya
strip -x build/arya

APP="dist/macOS/Arya Terminal Assistant.app"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp build/arya "$APP/Contents/MacOS/Arya Terminal Assistant"
cp assets/AppIcon.icns "$APP/Contents/Resources/AppIcon.icns"
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>Arya Terminal Assistant</string>
  <key>CFBundleDisplayName</key><string>Arya Terminal Assistant</string>
  <key>CFBundleIdentifier</key><string>com.ruzbahani.arya-terminal-assistant</string>
  <key>CFBundleVersion</key><string>${VERSION}</string>
  <key>CFBundleShortVersionString</key><string>${VERSION}</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleExecutable</key><string>Arya Terminal Assistant</string>
  <key>CFBundleIconFile</key><string>AppIcon</string>
  <key>NSHumanReadableCopyright</key><string>© 2026 Ali Mohammadi Ruzbahani · ruzbahani.com</string>
  <key>LSApplicationCategoryType</key><string>public.app-category.developer-tools</string>
  <key>LSMinimumSystemVersion</key><string>10.15</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>NSAppTransportSecurity</key><dict><key>NSAllowsLocalNetworking</key><true/></dict>
</dict>
</plist>
PLIST
codesign --force --sign - "$APP"
echo "Built: $APP"
lipo -info "$APP/Contents/MacOS/Arya Terminal Assistant"
