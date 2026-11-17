#!/bin/sh
# Build XHCIPoll.kext with the Xcode toolchain. Output: build/XHCIPoll.kext
set -e
cd "$(dirname "$0")"
SDK=$(xcrun --sdk macosx --show-sdk-path)
K="$SDK/System/Library/Frameworks/Kernel.framework/Headers"
OUT=build/XHCIPoll.kext/Contents
rm -rf build && mkdir -p "$OUT/MacOS"
CF="-arch x86_64 -mkernel -nostdlib -isysroot $SDK -I$K -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -O2 -mmacosx-version-min=11.0"
xcrun clang++ $CF -fapple-kext -std=gnu++17 -Wall -Wno-deprecated-declarations -Wno-inconsistent-missing-override -c XHCIPoll.cpp -o build/XHCIPoll.o
xcrun clang $CF -c XHCIPoll_info.c -o build/info.o
xcrun clang++ -arch x86_64 -mkernel -nostdlib -isysroot "$SDK" -mmacosx-version-min=11.0 \
    -Xlinker -kext -Xlinker -undefined -Xlinker dynamic_lookup \
    build/XHCIPoll.o build/info.o -lkmodc++ -lkmod -lcc_kext -o "$OUT/MacOS/XHCIPoll"
cp Info.plist "$OUT/"
plutil -lint "$OUT/Info.plist"
echo "built $OUT/MacOS/XHCIPoll"
