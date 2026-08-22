# Prebuilt firmware

Built from the source in this repository, **with all four library patches
applied and with rebuilt ESP-IDF libraries** — see
[`../library-patch/`](../library-patch/). You do not need any of that to flash
these files; you need it to rebuild them.

| File | Size | Contents | Flash to |
|---|---|---|---|
| `Cardputer-WebRadio_v3.4.0.bin` | 2,892,240 bytes (2.9 MB) | application only | SD card / M5Launcher |
| `Cardputer-WebRadio_v3.4.0_merged.bin` | 4,194,304 bytes (4.2 MB) | bootloader + partition table + application | address `0x0` |

## Which one do I want?

**M5Launcher or an SD card:** the small one. Copy it to the card, pick it in the
launcher.

**M5Burner or a web flasher:** the `_merged` one, written to address `0x0`. It
contains everything, so a blank device works too.

**Arduino IDE / arduino-cli:** you do not need these files, you compile the
source instead — but read
[`../library-patch/idf-libs/`](../library-patch/idf-libs/) first, or https
stations will not play in your build.

## Build settings

| Setting | Value |
|---|---|
| Board | M5Cardputer |
| Partition Scheme | Huge APP (3MB No OTA / 1MB SPIFFS) |
| PSRAM | Disabled |

```
Sketch:          2,892,091 bytes (91% of 3,145,728)
Global variables:   90,960 bytes (27% of 327,680)
```

Both numbers went **down** against v3.3.0 while the radio gained https, a
charge screen and a rewritten user interface:

| | v3.3.0 | v3.4.0 |
|---|---:|---:|
| sketch | 2,919,043 | **2,892,091** |
| global variables | 102,164 | **90,960** |

Most of the drop in flash is the rebuilt ESP-IDF libraries — measured on their
own, same sketch, libraries swapped: 2,928,227 → 2,886,791 bytes.

Most of the flash that remains is fonts: the four CJK typefaces from M5GFX take
1.36 MB. They live in flash and cost no RAM.

**The M5Launcher gives an application 2880 KB**, and this build is 2824 KB —
**56 KB to spare**, up from 29 KB in v3.3.0.

## Note

`DEBUG_SERIAL` is 0 in these builds — the device stays quiet on USB. Set it to 1
in the sketch and rebuild if you need the heap, stack and audio diagnostics.
`AC_TLS_MESSUNG` in `AudioCompat.h` is 0 as well; at 1 it reports what each TLS
handshake costs in heap.
