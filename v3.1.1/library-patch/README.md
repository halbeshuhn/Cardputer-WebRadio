# Required patch for ESP32-audioI2S

**Without this patch the device can crash and lose its stored Wi-Fi
credentials.** Apply it before building.

File: `~/Documents/Arduino/libraries/ESP32-audioI2S-master/src/Audio.cpp`,
function `Audio::connecttohost()`.

```bash
cd ~/Documents/Arduino/libraries/ESP32-audioI2S-master/src
patch -p0 < audio-https-psram.patch
```

Or add these four lines by hand, directly **before** the line that reads
`if(startsWith(h_host, "https")) {m_f_ssl = true; ...}`:

```c
if(startsWith(h_host, "https") && !psramFound()) {
    AUDIO_INFO("https needs PSRAM - refused");
    stopSong();
    goto exit;
}
```

## Why

The Cardputer Adv has **no PSRAM**. For https the library needs
`WiFiClientSecure`, and mbedTLS allocates both record buffers at full size —
the SDK sets `CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN=16384` and does **not** set
`CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN`, so that is roughly **32 KB**. During
playback this device has 13 to 25 KB free.

Measured on hardware with `str.topradio.be`, a station listed as `http` that
redirects to `https` when you press play:

```
[audio] connect to: "str.topradio.be" on port 80 path "/topradio.mp3"
[audio] redirect to new host "https://playerservices.streamtheworld.com/..."
[audio] connect to: "playerservices.streamtheworld.com" on port 443
E (273640) esp-aes: Failed to allocate memory
[heap] frei 55404  min 996  groesster Block 38900
```

**996 bytes of free heap.** At that point any allocation in any task can fail —
the Wi-Fi stack, the display driver, and the NVS reads that load your saved
networks. `preferences.getString()` returns its *default value* on failure, so
a stored network reads back as empty, `findFreeSlot()` reports an occupied slot
as free, and the next save overwrites a real entry.

## Why the server-side filter is not enough

The station directory is queried with `is_https=false`, so radio-browser filters
https itself. But that only covers the **stored** URL. Where a server
**redirects** at playback time is in no database. It cannot be filtered in
advance.

## Why this one place is enough

`connecttohost()` is the entry point for both the initial URL **and** every
redirect — the header parser calls `connecttohost(c_host)` again on `location:`.
`httpPrint()` and `httpRange()` also set `m_f_ssl`, but they can no longer
receive an https host once this check is in place.

## Effect

Instead of a near-out-of-memory event, the library reports
`https needs PSRAM - refused`, the sketch shows "Stream unavailable", and the
heap never drops. The station stays unplayable — it already was, just
dangerously so.

The library already does exactly this for FLAC (`"FLAC works only with
PSRAM!"`). This patch follows that pattern.

**On a device with PSRAM nothing changes**, since `psramFound()` is true there.

## After a library update

An update overwrites the patch without warning. Reapply it, or the crash risk
returns.
