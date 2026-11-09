#!/bin/sh
# Build a self-contained "Driver 2.app" in dist/ to share (Apple Silicon only).
# Everything inside: binary, Homebrew libs, game data, disc 1 image. Saves go to ~/.driver2.
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$ROOT/game"
OUT="$ROOT/dist"
APP="$OUT/Driver 2.app"
C="$APP/Contents"
rm -rf "$APP" "$OUT/Driver 2.zip"
mkdir -p "$C/MacOS" "$C/Frameworks" "$C/Resources/game/install"

cp "$ROOT/Driver 2.app/Contents/Info.plist" "$C/Info.plist"
/usr/libexec/PlistBuddy -c "Set :CFBundleIdentifier local.redriver2.share" "$C/Info.plist"
cp "$ROOT/Driver 2.app/Contents/Resources/Driver2.icns" "$C/Resources/"

# binary + libs (rewire Homebrew paths to the bundle)
cp "$SRC/REDRIVER2" "$C/MacOS/REDRIVER2"
for L in /opt/homebrew/opt/openal-soft/lib/libopenal.1.dylib \
         /opt/homebrew/opt/sdl2-compat/lib/libSDL2-2.0.0.dylib \
         /opt/homebrew/opt/jpeg/lib/libjpeg.10.dylib \
         /opt/homebrew/opt/sdl3/lib/libSDL3.0.dylib; do
  N="$(basename "$L")"
  cp -L "$L" "$C/Frameworks/$N"; chmod u+w "$C/Frameworks/$N"
  install_name_tool -id "@executable_path/../Frameworks/$N" "$C/Frameworks/$N" 2>/dev/null
  install_name_tool -change "$L" "@executable_path/../Frameworks/$N" "$C/MacOS/REDRIVER2" 2>/dev/null || true
done
# sdl2-compat dlopens @loader_path/libSDL3.dylib
ln -s libSDL3.0.dylib "$C/Frameworks/libSDL3.dylib"

# game data (APFS clones: no extra disk space)
cp -c "$SRC/config.ini" "$C/Resources/game/"
cp -cR "$SRC/DRIVER2" "$C/Resources/game/DRIVER2"
cp -cR "$SRC/mod" "$C/Resources/game/mod"
cp -cL "$SRC/install/Driver2CD1.bin" "$C/Resources/game/install/Driver2CD1.bin"

cat > "$C/MacOS/Driver2" <<'SH'
#!/bin/sh
# Data is read relative to the working dir; logs go to ~/.driver2 if the bundle is read-only.
cd "$(dirname "$0")/../Resources/game" || exit 1
exec ../../MacOS/REDRIVER2 "$@"
SH
chmod +x "$C/MacOS/Driver2"

xattr -cr "$APP"
codesign --force --deep -s - "$APP"
cd "$OUT" && ditto -c -k --keepParent "Driver 2.app" "Driver 2.zip"
echo "OK: $APP"; du -sh "$APP" "$OUT/Driver 2.zip"
