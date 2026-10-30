#!/bin/sh
# Builds OpenJamTest.ipa, unsigned: Sideloadly signs it with the player's own
# Apple ID when it installs it. Runs on macOS with Xcode.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-$here/out}
app=$out/Payload/OpenJamTest.app
rm -rf "$out" && mkdir -p "$app"
sdk=$(xcrun --sdk iphoneos --show-sdk-path)
xcrun --sdk iphoneos clang++ -std=c++20 -fobjc-arc -O2 -arch arm64 \
  -isysroot "$sdk" -miphoneos-version-min=15.0 \
  -framework UIKit -framework Foundation -framework Metal -framework GameController \
  "$here/main.mm" -o "$app/OpenJamTest"
cp "$here/Info.plist" "$app/Info.plist"
# Sideloadly re-signs; an ad-hoc signature keeps the bundle well formed.
codesign --force --sign - "$app"
(cd "$out" && zip -qry OpenJamTest.ipa Payload)
ls -la "$out/OpenJamTest.ipa"
