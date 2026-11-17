#!/bin/sh
# Install ACPMic.kext into the AUXILIARY kernel collection. Run by a person:
#
#   sudo sh tools/install.sh
#
# Why not the EFI: ACPMic links against com.apple.iokit.IOAudioFamily, which on
# macOS 26 lives only in SystemKernelExtensions.kc. OpenCore injects into the
# BOOT collection, which cannot link to the system one. The auxiliary
# collection (/Library/Extensions) can - that is how RBAMDFramebuffer is
# installed on this machine, and this script follows the same steps as
# src/Radeonized-v010/tools/fbinstall.sh.
#
# Installing changes nothing at runtime by itself: without the boot-arg
# -acpmic the driver's probe() returns at once and touches no hardware.
#
# Steps: stage into /Library/Extensions -> build a scratch aux collection
# -> refuse unless it carries ACPMic's UUID AND every kext the live one
# carries -> back up the live collection -> write it -> verify again, restore
# the backup on any mismatch.
set -eu
[ "$(id -u)" = 0 ] || { echo "run with sudo: sudo sh $0" >&2; exit 1; }

here="$(cd "$(dirname "$0")/.." && pwd)"
src="$here/build/ACPMic.kext"
bin="$src/Contents/MacOS/ACPMic"
boot=/System/Library/KernelCollections/BootKernelExtensions.kc
sys=/System/Library/KernelCollections/SystemKernelExtensions.kc
live=/Library/KernelCollections/AuxiliaryKernelExtensions.kc
scratch=/private/var/tmp/aux-acpmic.kc

[ -f "$bin" ] || { echo "no kext at $src - run: sh $here/build.sh" >&2; exit 1; }
uuid="$(dwarfdump --uuid "$bin" | awk '{print $2}')"
[ -n "$uuid" ] || { echo "cannot read UUID of $bin" >&2; exit 1; }
echo "installing ACPMic $uuid"

# Every kext UUID in the live collection now, minus a previous ACPMic build;
# the new collection must keep all of them (RBAMDFramebuffer above all).
prev="$(dwarfdump --uuid /Library/Extensions/ACPMic.kext/Contents/MacOS/ACPMic 2>/dev/null | awk '{print $2}' | tr a-f A-F)"
before="$(kmutil inspect -B "$live" --show-kext-uuids 2>/dev/null \
    | awk -F '\t' 'NF >= 3 { print $NF }' \
    | grep -oiE '[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}' \
    | tr a-f A-F | sort -u | grep -vx "${prev:-NONE}" || true)"
echo "  live collection holds $(echo "$before" | grep -c . || true) other kext UUID(s); all must survive"

echo "  staging into /Library/Extensions"
rm -rf /Library/Extensions/ACPMic.kext
cp -R "$src" /Library/Extensions/
chown -R root:wheel /Library/Extensions/ACPMic.kext
chmod -R 755 /Library/Extensions/ACPMic.kext

check() {   # $1 = collection; fails unless ACPMic's UUID and all $before UUIDs are in it
    got="$(kmutil inspect -B "$1" --show-kext-uuids 2>/dev/null)"
    echo "$got" | grep -qi "$uuid" || { echo "  $1 does NOT carry ACPMic $uuid" >&2; return 1; }
    for u in $before; do
        echo "$got" | grep -qi "$u" || { echo "  $1 LOST kext $u (was in the live collection)" >&2; return 1; }
    done
    return 0
}

echo "  dry run -> $scratch"
rm -f "$scratch"
kmutil create -n aux -a x86_64 --allow-missing-kdk \
    -B "$boot" -S "$sys" -A "$scratch" -r /Library/Extensions >/dev/null
[ -s "$scratch" ] || {
    echo "DRY RUN WROTE NOTHING (kmutil exits 0 when it refuses). Live collection untouched." >&2
    echo "Undo the staging with: sudo rm -rf /Library/Extensions/ACPMic.kext" >&2
    exit 1
}
check "$scratch" || { echo "Dry run rejected. Live collection untouched." >&2; exit 1; }
echo "  dry run carries ACPMic and every previously installed kext"

echo "  backing up $live -> $live.acpmic-bak"
cp "$live" "$live.acpmic-bak"
echo "  writing $live"
kmutil create -n aux -a x86_64 --allow-missing-kdk \
    -B "$boot" -S "$sys" -A "$live" -r /Library/Extensions >/dev/null
if check "$live"; then
    rm -f "$scratch"
    echo
    echo "INSTALLED. $live carries ACPMic $uuid."
    echo "It stays inert until the boot-arg -acpmic is present. Next: README.md step 2."
    exit 0
fi
echo "LIVE COLLECTION CHECK FAILED - restoring the backup." >&2
cp "$live.acpmic-bak" "$live"
exit 1
