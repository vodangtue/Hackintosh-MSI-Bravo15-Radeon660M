#!/bin/sh
# Installs RBAMDFramebuffer.kext (iGPU framebuffer) and ACPMic.kext (built-in
# microphone) into the macOS Auxiliary Kernel Collection. Run once after
# installing macOS, from this folder:
#
#   sudo sh install-aux-kexts.sh
#
# Then approve the kexts in System Settings > Privacy & Security if asked,
# and reboot. Safe order: dry run to a scratch collection, check both UUIDs
# are in it, back up the live collection, write it, check again.
set -eu
[ "$(id -u)" = 0 ] || { echo "run with sudo: sudo sh install-aux-kexts.sh" >&2; exit 1; }
here="$(cd "$(dirname "$0")" && pwd)"
boot=/System/Library/KernelCollections/BootKernelExtensions.kc
sys=/System/Library/KernelCollections/SystemKernelExtensions.kc
live=/Library/KernelCollections/AuxiliaryKernelExtensions.kc
scratch=/private/var/tmp/aux-install.kc
uuids=""
for k in RBAMDFramebuffer ACPMic; do
    [ -d "$here/$k.kext" ] || { echo "missing $here/$k.kext" >&2; exit 1; }
    u="$(dwarfdump --uuid "$here/$k.kext/Contents/MacOS/$k" | awk '{print $2}')"
    [ -n "$u" ] || { echo "no UUID in $k" >&2; exit 1; }
    echo "staging $k ($u)"
    rm -rf "/Library/Extensions/$k.kext"
    cp -R "$here/$k.kext" /Library/Extensions/
    chown -R root:wheel "/Library/Extensions/$k.kext"
    uuids="$uuids $u"
done
rm -f "$scratch"
kmutil create -n aux -a x86_64 --allow-missing-kdk -B "$boot" -S "$sys" -A "$scratch" -r /Library/Extensions >/dev/null
[ -s "$scratch" ] || { echo "dry run wrote nothing - live collection untouched" >&2; exit 1; }
for u in $uuids; do
    kmutil inspect -B "$scratch" --show-kext-uuids 2>/dev/null | grep -qi "$u" \
        || { echo "dry run is missing $u - live collection untouched" >&2; exit 1; }
done
[ -f "$live" ] && cp "$live" "$live.bak"
kmutil create -n aux -a x86_64 --allow-missing-kdk -B "$boot" -S "$sys" -A "$live" -r /Library/Extensions >/dev/null
for u in $uuids; do
    kmutil inspect -B "$live" --show-kext-uuids 2>/dev/null | grep -qi "$u" || {
        echo "live collection is missing $u - restoring backup" >&2
        [ -f "$live.bak" ] && cp "$live.bak" "$live"
        exit 1
    }
done
rm -f "$scratch"
echo "INSTALLED. Reboot."
