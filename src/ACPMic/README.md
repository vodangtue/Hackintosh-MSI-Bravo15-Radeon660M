# ACPMic

macOS driver for the built-in **digital microphone** on AMD Rembrandt laptops
(AMD Audio Coprocessor ACP 6.x, PCI `1022:15e2`, PDM DMIC). It shows up as
**Built-in Digital Microphone** (48 kHz, stereo) in Sound settings.

- Needs IOAudioFamily, so it lives in the Auxiliary Kernel Collection, not in OpenCore.
- Does nothing without the boot-arg `-acpmic`.
- `-acpmicnoreset`: skip the ACP soft reset when the firmware already powered
  the ACP on (recommended; keeps the HDA speakers untouched).
- `-acpmicprobe`: read-only probe, no audio device.
- Optional tunables: `acpmicgain=0..3`, `acpmicboost=0..4`.

## Build and install

```bash
sh build.sh                    # runs the host tests, then builds build/ACPMic.kext
sudo sh tools/install.sh       # installs into the Auxiliary Kernel Collection
sudo sh tools/uninstall.sh     # removes it
```

Tested on MSI Bravo 15 B7ED (Ryzen 5 7535HS), macOS Tahoe 26.7.3.

## License

MIT.
