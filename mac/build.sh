#!/bin/sh
# Build REDRIVER2 (OpenGL) and install it into game/ safely (fresh file + ad-hoc signature).
set -e
cd "$(dirname "$0")/src/src_rebuild"
premake5 gmake >/dev/null
cd project_gmake_macosx
CXXFLAGS="-nostdinc++ -isystem $(xcrun --show-sdk-path)/usr/include/c++/v1" make config=release_arm64 -j6
cd ../../..
rm -f game/REDRIVER2
cp src/src_rebuild/bin/Release/REDRIVER2 game/REDRIVER2.tmp
mv game/REDRIVER2.tmp game/REDRIVER2
codesign --force -s - game/REDRIVER2
echo "OK: game/REDRIVER2"
