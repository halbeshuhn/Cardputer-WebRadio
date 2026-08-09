# Prebuilt firmware

Built from the source in this repository, with the
[library patch](../library-patch/) applied.

| File | Size | Contents | Flash to |
|---|---|---|---|
| `Cardputer-WebRadio_v3.0.1.bin` | 1.7 MB | application only | SD card / M5Launcher |
| `Cardputer-WebRadio_v3.0.1_merged.bin` | 4.2 MB | bootloader + partition table + application | address `0x0` |

## Which one do I want?

**M5Launcher or an SD card:** the small one. Copy it to the card, pick it in
the launcher.

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
Sketch:          1,679,215 bytes (53% of 3,145,728)
Global variables:   74,116 bytes (22% of 327,680)
```

## Note

`DEBUG_SERIAL` is 0 in these builds — the device stays quiet on USB. Set it to
1 in the sketch and rebuild if you need the heap and audio diagnostics.
