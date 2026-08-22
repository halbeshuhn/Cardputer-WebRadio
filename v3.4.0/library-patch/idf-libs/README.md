# idf-libs — rebuilding the ESP-IDF libraries for asymmetric TLS buffers

**This is not a source patch.** It replaces the precompiled ESP-IDF libraries
that ship with the M5Stack core. Without it the radio still builds and still
plays — but **https stations will fail**, because mbedTLS cannot find the
contiguous memory it needs for a handshake.

**You only need this to build the firmware yourself.** The images in
[`../../firmware/`](../../firmware/) are already built with these libraries.

## Why

mbedTLS reserves two record buffers. In the stock configuration both are
16,384 bytes — about 33 KB of heap held for the entire life of a connection,
on a device with **no PSRAM** and 8–86 KB of contiguous heap to work with.

The outgoing buffer does not need that. Measured on 2026-08-18 across 44 https
radio stations from radio-browser (19 countries, `openssl s_client -msg`,
5 seconds of audio each), record sizes by direction:

| | min | median | max |
|---|---:|---:|---:|
| **TX** (we send) | 1,537 | 1,547 | **1,568** |
| **RX** (server sends) | 2,413 | 4,161 | **16,401** |

TX never exceeded 1,568 bytes — that is the ClientHello; the GET that follows
is 150 bytes. RX is a different story: five stations send the protocol maximum
of 16,401 bytes, so **IN must stay at 16,384** or those stations break.

`IN 16384 / OUT 2048` covers 100 % of the stations tested and saves ~14 KB.

The second change matters just as much: `CONFIG_MBEDTLS_DYNAMIC_BUFFER` is
**on** in current builder masters. It allocates and frees the TLS buffers per
record — maximum fragmentation, which is exactly what kills a device whose
limit is the largest contiguous block rather than the total free heap.

## What it gains — measured on the device

| | stock libraries (IDF 5.5.4) | rebuilt (IDF 5.5.5) |
|---|---:|---:|
| boot: free heap | 206,012 | **250,260** |
| boot: largest block | 188,404 | **196,596** |
| running: free heap | 77–85 KB | **129–134 KB** |
| running: lowest free | 67,668 | **121,100** |
| **running: largest block** | **65,524** | **86,004** |
| flash | 2,928,227 | **2,886,791** |

The A/B measurement of the asymmetry alone, same firmware, libraries swapped,
measured at the handshake:

| | free | largest block |
|---|---:|---:|
| stock (16384 + 16384) | −47,164 | −47,104 |
| rebuilt (16384 + **2048**) | −32,200 | −36,864 |
| **saved** | **14,964** | **10,240** |

Note the two effects are separate. Most of the first table comes from the
newer IDF and its component selection, not from the buffer sizes — the
asymmetry is the second table, and it is what makes an https handshake fit.

## Building them

Prerequisites on macOS (the build scripts want the GNU versions):

```bash
brew install cmake ninja ccache gnu-sed gawk coreutils
```

Then:

```bash
git clone https://github.com/espressif/esp32-arduino-lib-builder
cd esp32-arduino-lib-builder
git checkout ee57070          # the state this release was built with
```

Edit `configs/defconfig.common`:

```
CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN=y
CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=16384
CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=2048
# CONFIG_MBEDTLS_DYNAMIC_BUFFER is not set
```

Build for the S3 only — everything else is wasted hours:

```bash
./build.sh -t esp32s3
```

ESP-IDF `release/v5.5` is the default in `tools/config.sh`; leave it alone.
The result lands in `out/tools/esp32-arduino-libs/esp32s3/`.

## Installing them

Keep the original. It is the only way back.

```bash
cd ~/Library/Arduino15/packages/m5stack/tools/esp32s3-libs
mv 3.3.8 3.3.8_ORIGINAL
cp -R ~/esp32-arduino-lib-builder/out/tools/esp32-arduino-libs/esp32s3 3.3.8
```

Going back is two `mv` commands.

M5Stack forks only the core files; the precompiled libraries come from
Espressif (tag 3.3.8) either way, so there is no fork to reconcile.

## Two things to know

- **A core update through the Boards Manager wipes this.** The rebuilt tree is
  gone and has to be put back. There is no warning.
- The rebuilt tree carries **168** libraries instead of 152 and IDF 5.5.**5**
  instead of 5.5.**4** — a consequence of the current builder state. No effect
  has shown up so far, but a long-running soak test has not been done either.

## If you skip this

The sketch compiles and runs. http stations play. Station logos load.

What breaks is https: an `open()` on a TLS stream takes 30–43 KB in one piece,
and with `TLS_DAUERTOPF 1` (the persistent TLS context, on by default in
`M5Cardputer_WebRadio.ino`) the stock libraries leave **22,516 bytes** as the
largest block. The handshake cannot fit. Set `TLS_DAUERTOPF 0` and you get the
memory back, at the price of the fragmentation the pot was built to avoid.
