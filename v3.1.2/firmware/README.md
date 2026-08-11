# Prebuilt firmware

Built from the source in this repository. **No library patches are needed any
more** — since 3.1.2 the audio layer runs on
[ESP8266Audio](https://github.com/earlephilhower/ESP8266Audio), which brings
its own Helix MP3 and AAC decoders.

| File | Size | Contents | Flash to |
|---|---|---|---|
| `Cardputer-WebRadio_v3.1.2.bin` | 1.5 MB | application only | SD card / M5Launcher |
| `Cardputer-WebRadio_v3.1.2_merged.bin` | 4.2 MB | bootloader + partition table + application | address `0x0` |

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
Sketch:          1,510,007 bytes (48% of 3,145,728)
Global variables:  159,644 bytes (48% of 327,680)
```

## Note

`DEBUG_SERIAL` is 0 in these builds — the device stays quiet on USB. Set it to
1 in the sketch and rebuild if you need the heap and audio diagnostics.
