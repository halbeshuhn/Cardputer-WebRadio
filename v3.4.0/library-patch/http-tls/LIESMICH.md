# http-tls — echtes https für den Stream

**Gebaut am 19.8.2026.** Ohne diesen Patch spricht `AudioFileSourceICYStream`
kein TLS: `open()` ruft `http.begin(client, url)` mit einem einfachen
`NetworkClient`, und jede `https://`-Adresse scheitert am Verbindungsaufbau.

## Was geändert ist

| Datei | Änderung |
|---|---|
| `AudioFileSourceHTTPStream.h` | `clientForUrl()` + `freeSecure()`, Feld `secure`, `saveURL[128]` → `[256]` |
| `AudioFileSourceHTTPStream.cpp` | `http.begin(clientForUrl(url), url)`, `secure = nullptr` im Konstruktor, `freeSecure()` im Destruktor |
| `AudioFileSourceICYStream.cpp` | dasselbe in `open()` und im Destruktor |

- Bei `https://` wird **einmal** ein `NetworkClientSecure` angelegt und
  behalten — der eingebaute Wiederverbinder ruft `open(saveURL)` erneut auf.
- `setInsecure()`: kein Zertifikatsspeicher. Das Gerät hat weder eine Uhr noch
  ein Wurzelzertifikat-Bündel.
- `setHandshakeTimeout(10)` statt der voreingestellten **120 Sekunden**. Ein
  Server, der annimmt und dann schweigt, hätte das Radio zwei Minuten
  eingefroren — `loop()` kehrt während des Handschlags nicht zurück.
- `freeSecure()` läuft **immer nach** `http.end()`: HTTPClient hält bis dahin
  einen Zeiger auf den Client.
- `saveURL` auf 256: die längste `url_resolved` bei radio-browser war 213
  Zeichen. Abgeschnitten lief der Wiederverbinder ins Leere.

## Was der Patch NICHT kann

Eine Umleitung, die das **Schema wechselt** (http → https oder umgekehrt).
`HTTPClient::setURL()` lehnt das ausdrücklich ab („new URL not the same
protocol"), weil der Client beim `begin()` fest zugewiesen wurde. Diese Fälle
fängt `AudioCompat.h` vorher ab und klopft selbst am neuen Ziel an; der
Logopfad im Sketch macht dasselbe mit einem zweiten Anlauf.

## Zurück zum Original

    cd libraries/ESP8266Audio/src
    cp .../http-tls/AudioFileSourceHTTPStream.h.original AudioFileSourceHTTPStream.h
    cp .../http-tls/AudioFileSourceHTTPStream.cpp.original AudioFileSourceHTTPStream.cpp
    cp .../http-tls/AudioFileSourceICYStream.cpp.original AudioFileSourceICYStream.cpp
