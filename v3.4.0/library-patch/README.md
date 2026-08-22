# Library patches — required to build

The sketch needs four changes to
[ESP8266Audio](https://github.com/earlephilhower/ESP8266Audio), and a rebuild of
the precompiled ESP-IDF libraries. **You only need any of this to compile the
source.** The images in [`../firmware/`](../firmware/) are already built with
all five.

| | What | Without it |
|---|---|---|
| [`aac-sbr-aus/`](aac-sbr-aus/) | SBR out of the AAC decoder | 59 KB of heap gone; the build stops with an `#error` |
| [`http-tls/`](http-tls/) | TLS in the audio stream | every `https://` station fails to connect |
| [`prefill/`](prefill/) | fill the buffer before playing, and an escape hatch | audio stutters at every start; an undecodable stream freezes the device |
| [`i2s-stille/`](i2s-stille/) | silence instead of repeating the last DMA block | every dropout rattles: 26 ms of audio in a loop |
| [`idf-libs/`](idf-libs/) | asymmetric mbedTLS buffers, no dynamic buffer | https handshakes do not fit in the largest free block |

The first four are source patches to files in
`~/Documents/Arduino/libraries/ESP8266Audio/src/`. The fifth replaces
precompiled libraries and is a longer job — read
[`idf-libs/README.md`](idf-libs/README.md) before starting it.

Each folder holds the changed files as `.original` and `.patched`, plus a
`LIESMICH.md` with the full reasoning and the measurements — in German, as the
author's working notes.

## The one with a patch file

```bash
cd ~/Documents/Arduino/libraries/ESP8266Audio/src
patch -p1 < .../library-patch/aac-sbr-aus/aac-sbr-aus.patch
```

If `patch` refuses because your library version differs, use the `.original` and
`.patched` copies in [`aac-sbr-aus/`](aac-sbr-aus/) and make the same three
edits by hand — they are small and obvious.

The other three are applied by copying the `.patched` files over the originals,
after keeping a copy of what was there. `i2s-stille` is a single line and comes
with a `.patch` as well.

## Why SBR has to go

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
decoder and the TLS connections are made of.

**The sketch checks this one.** `AudioCompat.h` stops the build with an `#error`
if `AAC_ENABLE_SBR` is still defined, so an unpatched library cannot slip
through and fail on the device instead. The other four fail later and more
quietly — a station that will not connect, a start that stutters, a dropout that
rattles instead of falling silent, a handshake that runs out of memory.
