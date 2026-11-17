#!/bin/sh
# Build ACPMic.kext with the Xcode toolchain (same style as kexts/DGPUSleep).
# Runs the host tests first; refuses to build if they fail.
# Output: build/ACPMic.kext
set -e
cd "$(dirname "$0")"
./test/run_tests.sh
SDK=$(xcrun --sdk macosx --show-sdk-path)
K="$SDK/System/Library/Frameworks/Kernel.framework/Headers"
OUT=build/ACPMic.kext/Contents
rm -rf build/ACPMic.kext build/*.o && mkdir -p "$OUT/MacOS"
CF="-arch x86_64 -mkernel -nostdlib -isysroot $SDK -I$K -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -O2 -mmacosx-version-min=11.0"
xcrun clang++ $CF -fapple-kext -std=gnu++17 -Wall -Wno-deprecated-declarations -Wno-inconsistent-missing-override -Wno-#warnings -c src/ACPMic.cpp -o build/ACPMic.o
xcrun clang $CF -std=c11 -Wall -Wextra -c src/acp_core.c -o build/acp_core.o
xcrun clang $CF -c src/ACPMic_info.c -o build/info.o
xcrun clang++ -arch x86_64 -mkernel -nostdlib -isysroot "$SDK" -mmacosx-version-min=11.0 \
    -Xlinker -kext -Xlinker -undefined -Xlinker dynamic_lookup \
    build/ACPMic.o build/acp_core.o build/info.o -lkmodc++ -lkmod -lcc_kext -o "$OUT/MacOS/ACPMic"
cp Info.plist "$OUT/"
plutil -lint "$OUT/Info.plist"
# Our own symbols must all be defined: -undefined dynamic_lookup would let a
# dangling extern link clean and fault at runtime.
if nm -u "$OUT/MacOS/ACPMic" | grep -E "_acp_|ACPMic"; then
    echo "ERROR: undefined ACPMic symbol(s) above" >&2; exit 1
fi
codesign -s - -f build/ACPMic.kext >/dev/null 2>&1
echo "built $OUT/MacOS/ACPMic  UUID $(dwarfdump --uuid "$OUT/MacOS/ACPMic" | awk '{print $2}')"
