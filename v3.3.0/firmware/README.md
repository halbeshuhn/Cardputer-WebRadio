# Prebuilt firmware

Built from the source in this repository, **with the library patch applied** —
SBR is taken out of the AAC decoder, see [`../library-patch/`](../library-patch/).
You do not need the patch to flash these files; you need it to rebuild them.

| File | Size | Contents | Flash to |
|---|---|---|---|
| `Cardputer-WebRadio_v3.3.0.bin` | 2,919,184 bytes (2.9 MB) | application only | SD card / M5Launcher |
| `Cardputer-WebRadio_v3.3.0_merged.bin` | 4,194,304 bytes (4.2 MB) | bootloader + partition table + application | address `0x0` |

## Which one do I want?

**M5Launcher or an SD card:** the small one. Copy it to the card, pick it in the
launcher.

**M5Burner or a web flasher:** the `_merged` one, written to address `0x0`. It
contains everything, so a blank device works too.

**Arduino IDE / arduino-cli:** you do not need these files, you compile the
source instead.

## Build settings

| Setting | Value |
|---|---|
| Board | M5Cardputer |
| Partition Scheme | Huge APP (3MB No OTA / 1MB SPIFFS) |
| PSRAM | Disabled |

```
Sketch:          2,919,043 bytes (92% of 3,145,728)
Global variables:  102,164 bytes (31% of 327,680)
```

Global variables dropped by 59 KB against 3.2.0: the AAC decoder's workspace
went from 90,048 to 30,720 bytes once SBR was out of it. That is the memory the
station logos are made of.

Most of the flash is fonts: the four CJK typefaces from M5GFX take 1.36 MB.
They live in flash and cost no RAM.

**The M5Launcher gives an application 2880 KB**, and this build is 2851 KB —
**29 KB to spare**. Anything added from here on has to fit in that.

## Note

`DEBUG_SERIAL` is 0 in these builds — the device stays quiet on USB. Set it to 1
in the sketch and rebuild if you need the heap, stack and audio diagnostics.
