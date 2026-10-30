#!/bin/sh
# Packs the iPhone build as an unsigned .ipa: Sideloadly signs it with the
# player's own Apple ID when it installs it. Runs on macOS with Xcode.
#
#   package.sh <build dir with nbajam_ofe> <SDK install prefix> <out dir> <version> [base|2020s|Legends]
#
# with MOLTENVK_FRAMEWORK set to the ios-arm64 MoltenVK.framework from
# Khronos' MoltenVK-ios.tar release.
#
# The app is only the program: the executable, the runtime libraries in
# Frameworks, MoltenVK, and the achievement data. The game goes in the app's
# folder in the Files app and is unpacked on first launch
# (src/ios_platform.mm).
set -eu
build=$1 prefix=$2 out=$3 version=$4 variant=${5:-base}
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)

case $variant in
  base)    id=org.openjam.nbajam;         short="NBA JAM";         file=NBAJamOFE;         v=""        assets=Assets ;;
  2020s)   id=org.openjam.nbajam.2020s;   short="NBA JAM 2020s";   file=NBAJamOFE-2020s;   v=2020s     assets=Assets-2020s ;;
  Legends) id=org.openjam.nbajam.legends; short="NBA JAM Legends"; file=NBAJamOFE-Legends; v=Legends   assets=Assets-Legends ;;
  *) echo "unknown variant $variant" >&2; exit 1 ;;
esac

stage=$out/stage-$variant
app=$stage/Payload/nbajam_ofe.app
rm -rf "$stage" && mkdir -p "$app/Frameworks"

cp "$build/nbajam_ofe" "$app/"
# The runtime and the graphics plugin are built beside the executable;
# MoltenVK is Khronos' prebuilt iOS framework ($MOLTENVK_FRAMEWORK).
find "$build" -maxdepth 1 -name '*.dylib' -exec cp {} "$app/Frameworks/" \;
cp -R "$MOLTENVK_FRAMEWORK" "$app/Frameworks/MoltenVK.framework"
cp "$repo/xbox/game/nbajam_ofe.toml" "$app/"
[ -d "$repo/metadata" ] && cp -R "$repo/metadata" "$app/metadata"

sed -e "s|@IDENTITY@|$id|" -e "s|@SHORT_NAME@|$short|" -e "s|@VERSION@|$version|" \
    -e "s|@VARIANT@|$v|" "$here/Info.plist" > "$app/Info.plist"
plutil -lint "$app/Info.plist"

# The home screen icon, from the Xbox tile.
src="$repo/packaging/xbox/$assets/Square150x150Logo.png"
sips -z 120 120 "$src" --out "$app/AppIcon60x60@2x.png" >/dev/null
sips -z 180 180 "$src" --out "$app/AppIcon60x60@3x.png" >/dev/null

# Each library carries its install name; the executable finds them through
# @rpath, which is @executable_path/Frameworks.
for lib in "$app"/Frameworks/*.dylib; do
  install_name_tool -id "@rpath/$(basename "$lib")" "$lib" 2>/dev/null || true
done
echo "executable links:"
otool -L "$app/nbajam_ofe"
otool -l "$app/nbajam_ofe" | grep -A2 LC_RPATH || true

# Sideloadly re-signs; an ad-hoc signature keeps the bundle well formed.
for lib in "$app"/Frameworks/*.dylib "$app/Frameworks/MoltenVK.framework"; do
  codesign --force --sign - "$lib"
done
codesign --force --sign - "$app"

(cd "$stage" && zip -qry "$out/${file}_${version}.ipa" Payload)
rm -rf "$stage"
ls -la "$out/${file}_${version}.ipa"
