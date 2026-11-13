#!/bin/sh
# Make a double-click launcher on the Desktop: "Jedi Knight Remaster.app", with the GOG icon,
# which runs scripts/run.sh. The first launch may ask to let it access files on the external
# drive: allow it once.
#
#   scripts/make_launcher.sh [destination folder, default ~/Desktop]
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="${1:-$HOME/Desktop}"
APP="$DEST/Jedi Knight Remaster.app"
TMP="$(mktemp -d)"
ICO="$(ls "$ROOT"/gamedata/jk1/goggame-*.ico 2>/dev/null | head -1)"

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
if [ -n "$ICO" ]; then
    python3 - "$ICO" "$TMP/JediKnight.iconset" <<'EOF'
import os, sys
from PIL import Image
ico = Image.open(sys.argv[1])
ico.size = max(ico.info.get("sizes", [ico.size]))
im = ico.convert("RGBA")
os.makedirs(sys.argv[2], exist_ok=True)
for s in (16, 32, 128, 256, 512):
    im.resize((s, s), Image.LANCZOS).save("%s/icon_%dx%d.png" % (sys.argv[2], s, s))
    im.resize((s * 2, s * 2), Image.LANCZOS).save("%s/icon_%dx%d@2x.png" % (sys.argv[2], s, s))
EOF
    iconutil -c icns "$TMP/JediKnight.iconset" -o "$APP/Contents/Resources/JediKnight.icns"
fi
cat > "$APP/Contents/Info.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleName</key><string>Jedi Knight Remaster</string>
	<key>CFBundleDisplayName</key><string>Jedi Knight Remaster</string>
	<key>CFBundleIdentifier</key><string>com.dom.jediknight.launcher</string>
	<key>CFBundleExecutable</key><string>launcher</string>
	<key>CFBundleIconFile</key><string>JediKnight</string>
	<key>CFBundlePackageType</key><string>APPL</string>
	<key>CFBundleVersion</key><string>1.0</string>
	<key>LSMinimumSystemVersion</key><string>11.0</string>
</dict>
</plist>
EOF
cat > "$APP/Contents/MacOS/launcher" <<EOF
#!/bin/sh
# Jedi Knight Remaster launcher (scripts/make_launcher.sh): runs scripts/run.sh.
ROOT="$ROOT"
if [ ! -x "\$ROOT/scripts/run.sh" ]; then
    /usr/bin/osascript -e 'display dialog "Can'"'"'t find the game at $ROOT. Is the drive connected?" buttons {"OK"} default button 1 with icon stop' >/dev/null
    exit 1
fi
cd "\$ROOT" && exec scripts/run.sh
EOF
chmod +x "$APP/Contents/MacOS/launcher"
codesign --force -s - "$APP" >/dev/null 2>&1 || true
touch "$APP"
rm -rf "$TMP"
echo "made $APP"
