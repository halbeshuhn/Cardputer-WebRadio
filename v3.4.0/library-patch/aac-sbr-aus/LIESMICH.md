# Eingriff in ESP8266Audio: SBR aus dem AAC-Decoder nehmen

Betrifft `~/Documents/Arduino/libraries/ESP8266Audio/src/`, drei Zeilen in
zwei Dateien plus eine Bedingung in einer dritten.

## Warum

SBR ist die Erweiterung, die aus AAC das AAC+ macht. Sie war übersetzt und hat
den Speicher gekostet — **benutzt wurde sie nie**: das Radio spielt bei AAC+
seit jeher nur die Basisschicht, das steht seit v3.1.0 so in der README.

Gemessen am 14.8.2026 mit der Toolchain des Geräts (`xtensa-esp32s3-elf-gcc`,
Strukturgrößen über `nm` ausgelesen):

| Baustein | mit SBR | ohne SBR |
|---|---:|---:|
| Eingangspuffer `buff` | 1.600 | 1.600 |
| Ausgabepuffer `outSample` | 8.192 | 4.096 |
| `AACDecInfo` | 96 | 96 |
| `PSInfoBase` | 28.752 | 20.560 |
| `PSInfoSBR` | 50.788 | — |
| **zusammen** | **89.428** | **26.352** |

Daher stand `AC_CODEC_BYTES` auf 90.048 — der Wert war auf 620 Bytes genau auf
AAC mit SBR zugeschnitten. Ohne SBR gibt MP3 den Ausschlag (libmad braucht
29.188), der Topf reicht mit **30.720**.

## Am Gerät angekommen

| | vorher | nachher |
|---|---:|---:|
| freier Heap bei laufendem Ton | 13–23 KB | **77–88 KB** |
| größter zusammenhängender Block | 7.668 | **63.476–69.620** |
| Flash | 2.919.027 | 2.884.507 |

Die 59.328 Bytes Unterschied im statischen RAM sind exakt die Verkleinerung
des Topfes — nichts anderes ist mitgewandert.

Damit wurde erst möglich, was vorher an der Speichergrenze scheiterte: der
PNG-Decoder von M5GFX braucht 32.768 Bytes am Stück (gemessen: rund 47 KB mit
allem), eine TLS-Verbindung rund 55 KB.

## Was geändert wurde

**`libhelix-aac/aacdec.h`** und **`libhelix-aac/aaccommon.h`** — die beiden
Definitionen von `AAC_ENABLE_SBR` auskommentiert.

**`AudioGeneratorAAC.h`** — der Ausgabepuffer hing an `#ifdef ESP8266`, nicht
am SBR-Schalter:

```cpp
#ifdef ESP8266
    const int outSampleLen = 1024 * 2;  // SBR disabled
#elif !defined(AAC_ENABLE_SBR)
    const int outSampleLen = 1024 * 2;  // neu
#else
    const int outSampleLen = 2048 * 2;  // SBR enabled
#endif
```

Ohne diese Zeile wären auf dem ESP32 weiterhin 8.192 statt 4.096 Bytes belegt
worden, obwohl SBR gar nicht mehr übersetzt ist.

## Zurück

```
cd ~/Documents/Arduino/M5Cardputer_WebRadio_backups/library-patch/aac-sbr-aus
L=~/Documents/Arduino/libraries/ESP8266Audio/src
cp aacdec.h.original    $L/libhelix-aac/aacdec.h
cp aaccommon.h.original $L/libhelix-aac/aaccommon.h
cp AudioGeneratorAAC.h.original $L/AudioGeneratorAAC.h
```

Dann muss `AC_CODEC_BYTES` in `AudioCompat.h` wieder auf **90048** — sonst
scheitert der AAC-Decoder zur Laufzeit mit „Out of memory".

## Sicherung dagegen

`AudioCompat.h` bricht das Übersetzen ab, wenn die Bibliothek nicht gepatcht
ist:

```cpp
#if defined(AAC_ENABLE_SBR)
#error "AAC_ENABLE_SBR ist an. Diese Fassung erwartet ESP8266Audio ohne SBR"
#endif
```

Ein Fehler beim Übersetzen ist besser als ein Radio, das nur bei AAC-Sendern
stumm bleibt.

## Was zu prüfen bleibt

AAC+ klingt unverändert, weil ohnehin nur die Basisschicht gespielt wurde —
das ist die Erwartung, und sie hat sich am Gerät bestätigt (MP3, AAC und AAC+
laufen). Sollte doch ein Sender auffallen, ist der Weg zurück oben beschrieben.
