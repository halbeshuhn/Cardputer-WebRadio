# i2s-stille — Stille statt Wiederholung, wenn der Nachschub ausbleibt

**Gebaut am 22.8.2026.** Eine Zeile in `AudioOutputI2S::begin()`.

```
chan_cfg.auto_clear = true;
```

## Was ohne sie passiert

`I2S_CHANNEL_DEFAULT_CONFIG` lässt `auto_clear` auf `false`, und
`AudioOutputI2S::begin()` übernimmt die Vorgabe ungeprüft. Schreibt dann
niemand rechtzeitig nach, sendet der DMA **den zuletzt geschriebenen Puffer
erneut** — bei fünf Puffern zu 2.304 Bytes sind das 26 ms Ton in
Endlosschleife.

Das ist das Rasseln, das man bei jeder Funkdelle hört. Es klingt nach einem
kaputten Decoder und ist keiner: der Decoder liefert schlicht nichts, und der
Ausgang wiederholt sich, weil ihm niemand gesagt hat, dass er schweigen soll.

## Was sie bewirkt

Mit gesetztem Schalter nullt die ISR den Puffer, sobald er gesendet ist.
Nachgelesen in der IDF, die diese Firmware gebaut hat
(`esp-idf/components/esp_driver_i2s/i2s_common.c`, `i2s_dma_tx_callback`):

```c
if (handle->dma.auto_clear_after_cb) {
    memset(curr_buf, 0, handle->dma.buf_size);
}
```

Das steht in einem eigenen `if`, unabhängig von einem registrierten
`on_sent`-Callback — es greift also auch dort, wo wie hier keiner gesetzt ist.

Kommt nichts nach, läuft **Stille** aus dem Ausgang.

## Was sie kostet

77 `memset` zu 2.304 Bytes je Sekunde bei 44,1 kHz, also 177 kB/s in der ISR.
Auf dem S3 unter 0,1 % Rechenzeit. Kein RAM, kein Flash.

## Der Name

`auto_clear` ist in der IDF ein Alias von `auto_clear_after_cb` (beide liegen
in derselben anonymen Union in `i2s_chan_config_t`). Der Kurzname gilt auch in
älteren Fassungen und ist deshalb hier verwendet.

Es gibt daneben `auto_clear_before_cb`, das den Puffer **vor** dem Callback
nullt. Für einen Ausgang ohne Callback ist der Unterschied bedeutungslos.

## Warum das nicht die ganze Miete ist

Stille statt Rasseln ist die halbe Antwort. Die andere Hälfte steht im Sketch,
in `AudioCompat.h` bei `AC_SAMMEL_AB`: dort wird der Decoder ausgesetzt, solange
der Vorpuffer unter 8 KB liegt, damit er den Ring nicht leerfrisst, bevor die
Verbindung wieder in Führung geht. Ohne das gäbe es zwar keine Wiederholung
mehr, aber weiterhin ein Wechselbad aus Tonfetzen und Löchern.

## Zurück zum Original

    cd libraries/ESP8266Audio/src
    cp .../i2s-stille/AudioOutputI2S.cpp.original AudioOutputI2S.cpp
