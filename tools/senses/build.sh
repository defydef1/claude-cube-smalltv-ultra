#!/bin/sh
# Build tools/CubeSenses.app: the cube shows «LISTENING» while the Claude app records your voice.
# A real app bundle is what lets macOS ask for microphone access. Then run tools/install_mac.sh.
set -e
cd "$(dirname "$0")"
APP=../CubeSenses.app
mkdir -p "$APP/Contents/MacOS"
cat > "$APP/Contents/Info.plist" <<'PL'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleIdentifier</key><string>com.claudecube.senses</string>
  <key>CFBundleName</key><string>CubeSenses</string>
  <key>CFBundleExecutable</key><string>CubeSenses</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>LSUIElement</key><true/>
  <key>NSMicrophoneUsageDescription</key><string>Claude Cube shows a waveform while you dictate to Claude. Audio is never saved.</string>
</dict>
</plist>
PL
swiftc -O main.swift -o "$APP/Contents/MacOS/CubeSenses" -framework AVFoundation -framework CoreAudio
codesign -f -s - "$APP"
echo "built $(cd .. && pwd)/CubeSenses.app (needs: pip3 install --user numpy sounddevice)"
