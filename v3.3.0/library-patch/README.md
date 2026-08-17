# Library patch — required to build

The sketch needs **one** change to [ESP8266Audio](https://github.com/earlephilhower/ESP8266Audio):
SBR has to come out of the AAC decoder. Three lines in two files, plus one
condition in a third.

**You only need this to compile the source.** The images in
[`../firmware/`](../firmware/) are already built with it.

## Why

SBR is the “+” in AAC+. It was being compiled in and **never used** — this radio
has played the AAC base layer only since the beginning, because SBR's own state
wants another 50 KB the Cardputer Adv does not have. Compiling it in anyway cost
59 KB of decoder workspace for nothing.

Measured with the device's own toolchain (`xtensa-esp32s3-elf-gcc`, structure
sizes read out with `nm`):

| | with SBR | without |
|---|---:|---:|
| AAC decoder workspace | 89,428 | 26,352 |
| MP3 decoder (libmad) | 29,188 | 29,188 |
| **reserved in the sketch** | **90,048** | **30,720** |

On the device that arrived as free heap: 20–30 KB became **72–104 KB**, and the
largest contiguous block 8–18 KB became **59–78 KB**. That is what the PNG
decoder and the TLS connection for the station logos are made of.

## Applying it

The files live in `~/Documents/Arduino/libraries/ESP8266Audio/src/`.

```bash
cd ~/Documents/Arduino/libraries/ESP8266Audio/src
patch -p1 < .../library-patch/aac-sbr-aus/aac-sbr-aus.patch
```

If `patch` refuses because your library version differs, use the `.original` and
`.patched` copies in [`aac-sbr-aus/`](aac-sbr-aus/) and make the same three
edits by hand — they are small and obvious.

**The sketch checks.** `AudioCompat.h` stops the build with an `#error` if
`AAC_ENABLE_SBR` is still defined, so an unpatched library cannot slip through
and fail on the device instead.

## Full write-up

[`aac-sbr-aus/LIESMICH.md`](aac-sbr-aus/LIESMICH.md) has the complete
measurement and the reasoning — in German, as the author's working notes.
