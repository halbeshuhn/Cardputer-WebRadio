# prefill — erst sammeln, dann spielen (und der Notausstieg)

**Gebaut am 19.8.2026.** Zusätze an `AudioFileSourceBuffer`, sonst nichts.

| Datei | Änderung |
|---|---|
| `AudioFileSourceBuffer.h` | Methode `preFill(ziel, fristMs)`, Merkmal `abortNow` |
| `AudioFileSourceBuffer.cpp` | ihre Umsetzung; `abortNow` in beiden Konstruktoren auf false, geprüft als Erstes in `read()` |

Der Zwischenstand vor dem Notausstieg liegt als `*.vor-notausstieg` daneben.

## Der Notausstieg

`abortNow` gesetzt heißt: das nächste `read()` liefert **0 Bytes**. Der
Generator beendet sich daraufhin selbst — `AudioGeneratorMP3.cpp:144`,
`if ((len == 0) && (unused == 0)) return MAD_FLOW_STOP;`. Damit kehrt `loop()`
zurück und der Aufrufer kann aufräumen.

Gebraucht wird das für Ströme, die der Decoder nicht versteht. Am 19.8.2026
mit Radio SRF 3 erlebt: ein AAC-Strom landete im MP3-Decoder, der suchte
innerhalb **eines** `loop()`-Aufrufs endlos nach einem gültigen Rahmen und
ließ dabei immer neu nachfüllen. Das Gerät war nicht mehr bedienbar und kam
allein nicht heraus; im Mitschnitt stand über Sekunden keine `[heap]`-Zeile
mehr. Gesetzt wird `abortNow` in `AudioCompat.h`, wenn mehr als zehn
Unterläufe in drei Sekunden gemeldet werden.

**Achtung beim Nachpflegen:** in `read()` gibt es eine Stelle, die beim
Unterlauf `filled = false` setzt. Dort darf `abortNow` **nicht** mit
zurückgesetzt werden — sonst löscht der erste Unterlauf nach dem Auslösen das
Merkmal wieder, und der Ausstieg greift nie.

## Warum

Ohne den Patch fängt der Ton bei fast leerem Ring an. `read()` füllt beim
ersten Zugriff zwar „vollständig", aber nur mit dem, was der Sender in seiner
Lesefrist hergibt — und **jeder Unterlauf wirft den Ring wieder ganz weg**
(`length = 0; filled = false`). Am Gerät gemessen: Start bei 21 %, dann 0 %,
hörbar als Stottern, bis der Vorrat einmal über rund 28 % gestiegen war.
Dasselbe beim Verlassen der Onlineliste, denn dort wird der Stream neu
aufgebaut.

## Zwei Hindernisse, die den Patch nötig machen

1. `fill()` ist **privat** — von außen nicht aufrufbar.
2. `filled` ist nach einem Vorfüllen von außen weiterhin `false`, und der erste
   `read()` würde den Ring dann verwerfen und blockierend neu füllen. Deshalb
   setzt `preFill()` das Feld selbst. `writePtr` und `length` stehen durch
   `fill()` schon richtig, `readPtr` ist seit dem Konstruktor 0.

## Achtung

Die `.original`-Dateien hier sind **rekonstruiert**: die Bibliotheksdateien
waren schon gepatcht, als der Ordner angelegt wurde, und die Originale
entstanden durch Herausnehmen genau der eingefügten Blöcke (825 bzw. 776
Zeichen). Sie enthalten kein `preFill` mehr. Wer ganz sichergehen will, holt
`AudioFileSourceBuffer.*` aus einer frischen ESP8266Audio-Kopie.

## Zurück zum Original

    cd libraries/ESP8266Audio/src
    cp .../prefill/AudioFileSourceBuffer.h.original   AudioFileSourceBuffer.h
    cp .../prefill/AudioFileSourceBuffer.cpp.original AudioFileSourceBuffer.cpp
