#!/bin/sh
# Remove ACPMic.kext from /Library/Extensions and rebuild the auxiliary
# collection without it, keeping every other kext. Run by a person:
#   sudo sh tools/uninstall.sh
# (Quicker way to switch it OFF: remove -acpmic from boot-args. No rebuild.)
set -eu
[ "$(id -u)" = 0 ] || { echo "run with sudo: sudo sh $0" >&2; exit 1; }
boot=/System/Library/KernelCollections/BootKernelExtensions.kc
sys=/System/Library/KernelCollections/SystemKernelExtensions.kc
live=/Library/KernelCollections/AuxiliaryKernelExtensions.kc
scratch=/private/var/tmp/aux-acpmic-rm.kc
[ -d /Library/Extensions/ACPMic.kext ] || { echo "ACPMic.kext is not in /Library/Extensions"; }
acp="$(dwarfdump --uuid /Library/Extensions/ACPMic.kext/Contents/MacOS/ACPMic 2>/dev/null | awk '{print $2}' | tr a-f A-F)"
keep="$(kmutil inspect -B "$live" --show-kext-uuids 2>/dev/null \
    | awk -F '\t' 'NF >= 3 { print $NF }' \
    | grep -oiE '[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}' \
    | tr a-f A-F | sort -u | grep -vx "${acp:-NONE}" || true)"
mkdir -p /private/var/tmp/acpmic-removed
rm -rf /private/var/tmp/acpmic-removed/ACPMic.kext
[ -d /Library/Extensions/ACPMic.kext ] && mv /Library/Extensions/ACPMic.kext /private/var/tmp/acpmic-removed/
rm -f "$scratch"
kmutil create -n aux -a x86_64 --allow-missing-kdk -B "$boot" -S "$sys" -A "$scratch" -r /Library/Extensions >/dev/null
[ -s "$scratch" ] || { echo "dry run wrote nothing; live collection untouched. Kext moved to /private/var/tmp/acpmic-removed" >&2; exit 1; }
got="$(kmutil inspect -B "$scratch" --show-kext-uuids 2>/dev/null)"
for u in $keep; do echo "$got" | grep -qi "$u" || { echo "dry run lost kext $u; live collection untouched" >&2; exit 1; }; done
cp "$live" "$live.acpmic-rm-bak"
cp "$scratch" "$live"
rm -f "$scratch"
echo "REMOVED. ACPMic is out of $live (backup: $live.acpmic-rm-bak). Reboot."
