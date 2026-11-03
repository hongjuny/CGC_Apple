#!/bin/bash
# Builds CGCApple.app (release). ROMs and disk images are never part of the
# repository. With --with-roms, the local roms/ folder (and disks/call3327.dsk,
# if present) are copied into the bundle for personal use; otherwise the app
# finds ROMs in $CGCAPPLE_ROMS, ~/Library/Application Support/CGCApple/roms or
# ./roms, or asks for the folder on first launch.
set -euo pipefail
cd "$(dirname "$0")/.."

swift build -c release
APP=build/CGCApple.app
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp .build/release/CGCApple "$APP/Contents/MacOS/CGCApple"

cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key><string>CGC Apple</string>
    <key>CFBundleDisplayName</key><string>CGC Apple ][+</string>
    <key>CFBundleIdentifier</key><string>local.cgcapple</string>
    <key>CFBundleExecutable</key><string>CGCApple</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>0.1</string>
    <key>CFBundleVersion</key><string>1</string>
    <key>LSMinimumSystemVersion</key><string>13.0</string>
    <key>NSHighResolutionCapable</key><true/>
    <key>CFBundleDocumentTypes</key>
    <array>
        <dict>
            <key>CFBundleTypeName</key><string>Apple II Disk Image</string>
            <key>CFBundleTypeRole</key><string>Editor</string>
            <key>CFBundleTypeExtensions</key><array><string>dsk</string><string>do</string><string>po</string><string>nib</string><string>woz</string><string>hdv</string><string>2mg</string><string>2img</string><string>2meg</string></array>
        </dict>
    </array>
</dict>
</plist>
PLIST

if [[ "${1:-}" == "--with-roms" ]]; then
    cp -R roms "$APP/Contents/Resources/roms"
    if [[ -f disks/call3327.dsk ]]; then
        mkdir -p "$APP/Contents/Resources/disks"
        cp disks/call3327.dsk "$APP/Contents/Resources/disks/"
    fi
fi

codesign --force --sign - "$APP" >/dev/null 2>&1 || true
echo "Built $APP"
