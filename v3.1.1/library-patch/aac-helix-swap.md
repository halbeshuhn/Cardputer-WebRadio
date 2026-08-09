# Second change to ESP32-audioI2S: put the Helix AAC decoder back

**Without this, AAC streams reboot the device.** Apply it after the
[https/PSRAM patch](README.md), to the same library.

## Why

`ESP32-audioI2S` used the Helix AAC decoder up to 3.0.0 — `aac_decoder.h`
still says `// based on helix aac decoder`. Later 3.0.x releases switched to
faad2. faad2's `channel_pair_element()` asks for `sizeof(element)` = **22,006
bytes in one piece, per stereo frame** — about 43 times a second — and does
not check the result. Of 71 allocations in `neaacdec.cpp`, two check for NULL.

The Cardputer Adv has no PSRAM, so that block is not reliably there. Measured
on the device: largest free block 12–24 KB during playback. Every AAC station
died on its first frame with `StoreProhibited`, `EXCVADDR 0x00000000`, at
`neaacdec.cpp:8097` — regardless of bitrate. 64 kbit/s AAC failed exactly like
192 kbit/s.

Helix allocates **once, when the station opens**: `PSInfoBase_t` 19,172 +
`ProgConfigElement_t`×16 1,312 + `AACDecInfo_t` 96 bytes, and keeps them. All
fourteen test streams (MP3 64–320, AAC 64–192, AAC+ 32–96) then played with
zero crashes. As a bonus the build shrinks by 62 KB of flash and 1 KB of RAM.

## How

In `libraries/ESP32-audioI2S-master/src/aac_decoder/`:

1. Delete `libfaad/`, `aac_decoder.cpp` and `aac_decoder.h`.
2. Put the 3.0.0 versions of `aac_decoder.cpp` and `aac_decoder.h` in their
   place, from
   `https://github.com/schreibfaul1/ESP32-audioI2S/tree/3.0.0/src/aac_decoder`.
3. Copy [`aac_compat.cpp`](aac_compat.cpp) from this folder in beside them.
4. Add these lines to `aac_decoder.h`, after `int AACGetBitrate();`:

```c
int         AACSetRawBlockParams(int nChans, int sampRateCore, int profile);
int         AACDecode(uint8_t *inbuf, int32_t *bytesLeft, short *outbuf);
uint8_t     AACGetSBR();
uint8_t     AACGetParametricStereo();
const char* AACGetErrorMessage(int8_t err);
```

`Audio.cpp` is not touched, so the https patch stays as it is.

## What `aac_compat.cpp` is for

Audio.cpp 3.0.13 calls three functions the 3.0.0 decoder does not have.
`AACGetSBR` and `AACGetParametricStereo` only feed log lines — checked at both
call sites, the value is stored in a variable nothing ever reads. The third
returns error text. Two more calls share a name but not a signature, so
overloads sit next to them rather than editing the Helix source:
`AACSetRawBlockParams` without faad2's `copyLast`, and `AACDecode` taking
`int32_t*` where Helix takes `int*` (distinct types on the ESP32).

## The catch: no SBR

The decoder enables Spectral Band Replication only under
`#if (defined CONFIG_IDF_TARGET_ESP32S3 && defined BOARD_HAS_PSRAM)`, and its
`PSInfoSBR_t` is 50,788 bytes — out of reach without PSRAM. AAC+ therefore
plays its base layer only: measured 22,050 Hz instead of 44,100, and mono
instead of stereo where parametric stereo is used. It is stable, it just
sounds duller. Plain AAC-LC is unaffected.

## After a library update

An update overwrites `src/aac_decoder/` and the swap is gone — AAC will start
rebooting the device again. Redo steps 1–4.
