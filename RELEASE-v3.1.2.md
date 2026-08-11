This is the release that stops the reboots.

## What was wrong

Playing a stream took the device down every few minutes. Four crashes, four
different places, one picture: the lwIP connection structures held garbage.
A corrupted free list inside `tlsf_malloc`. A receive window of impossible
size. A netconn in a state its own close routine rejects. A TCP socket that
dispatched into the UDP branch because its type field no longer said TCP.

None of them was a shortage of memory — the heap was healthy when the last two
happened. Something was writing over memory it did not own, and the network
stack, which allocates constantly, was simply the first to walk into the
damage.

## What's new

- **The audio layer runs on [ESP8266Audio](https://github.com/earlephilhower/ESP8266Audio).**
  The sketch still speaks the old interface; `AudioCompat.h` maps it onto the
  new library. Going back is one include line. Measured after the change: two
  hours of playback and **193 station changes** across MP3, AAC and AAC+ with
  no restart, no crash, and no memory lost — free heap at the end was in the
  same band as at the start.
- **Both library patches are obsolete.** No more mbedTLS workaround for https
  redirects, no more swapping the AAC decoder back to Helix. ESP8266Audio
  brings its own Helix decoders and never attempts https. If you built earlier
  versions, you can stop reapplying patches after every library update.
- **AAC and AAC+ have a fixed 90,048-byte field** instead of asking the heap
  for a contiguous block. `Low RAM - codec too big` cannot happen any more,
  and it is gone from the code.
- **The bitrate in the footer is measured, not reported.** Stations that send
  no `icy-br` header used to leave it at zero, which made the radio believe
  the stream had failed. It now counts the bytes actually arriving; measured
  against fourteen test streams it lands within two percent.
- **The stream title moved.** It used to be a second header line that scrolled
  past once. It now stands still below the red rule, in the empty display
  where there is room for it — reachable with `F`, the same key that cycles
  the VU meter and the spectrum. Set in Noto Sans at 14 px, wrapped at word
  boundaries, **with umlauts and accents**. Drawing waits while the input
  buffer is low: the sound has priority over the display.
- **VU meter and spectrum moved up** into the space the header gave back, same
  size, re-centred. They now read the decoder's own level, so they no longer
  drop when you turn the volume down.
- **A lost stream comes back by itself.** Until now the radio simply went
  silent: no message, no second attempt, the footer still showing the old
  bitrate. It now notices after eight seconds and retries at 5, 10, 20 and
  then every 40 seconds.
- **Wi-Fi reconnects on its own.** When the access point disappears the driver
  gives up after one attempt, even with auto-reconnect switched on. A keeper
  now tries again every 15 seconds without blocking playback.

## Known limits

AAC+ works, but the decoder field is sized for it — 90,048 bytes are reserved
whether you play AAC or not. That is the price of never failing for want of a
contiguous block on a device with no PSRAM.

The pre-flight request that reads `Content-Type` costs one extra connection
per station change. It is what makes codec detection reliable instead of
guessed.

https is still not supported, and for the same reason as before: mbedTLS wants
about 32 KB that this device does not have.

## Flashing

| File | Use with |
|---|---|
| `Cardputer-WebRadio_v3.1.2.bin` | SD card / M5Launcher — application only |
| `Cardputer-WebRadio_v3.1.2_merged.bin` | M5Burner or a web flasher — complete image, write to address `0x0` |

Building from source now needs only
[ESP8266Audio](https://github.com/earlephilhower/ESP8266Audio) alongside the
M5 libraries. Board settings are unchanged: **Huge APP** partition scheme,
**PSRAM disabled**.
