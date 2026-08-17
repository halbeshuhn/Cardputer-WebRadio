// ===========================================================================
// AudioCompat.h - dieselbe Schnittstelle wie ESP32-audioI2S, darunter aber
// ESP8266Audio von earlephilhower.
//
// Warum ueberhaupt: Am 11.8.2026 hat sich gezeigt, dass die Abstuerze alle im
// TCP-Pfad des Streams liegen und die lwIP-Verbindungsstrukturen zerstoert
// sind. Der einzige greifbare Verdaechtige ist der zweite Task, den
// ESP32-audioI2S seit 3.0.12 im Konstruktor startet: der loop-Task schreibt
// in den Ringpuffer, der Bibliothekstask liest daraus, und der Mutex wird nur
// beim Lesen genommen.
//
// ESP8266Audio hat keinen eigenen Task - sie wird Stueck fuer Stueck aus
// loop() getrieben. Damit faellt diese Bauart als Ursache weg oder nicht,
// je nachdem, ob es damit durchlaeuft.
//
// Der Sketch bleibt unveraendert: dieselben Aufrufe, dieselben Rueckrufe.
// Zurueck geht es, indem in M5Cardputer_WebRadio.ino wieder <Audio.h> statt
// dieser Datei eingebunden wird.
//
// Was anders ist, und zwar spuerbar:
//
// 1. Codec und Bitrate stehen bei ESP8266Audio nirgends bereit. Deshalb holt
//    connecttohost() vorab die Kopfzeilen mit einem eigenen HTTPClient und
//    liest Content-Type, icy-br und icy-name aus. Das kostet eine zweite
//    Verbindung je Senderwechsel, liefert dafuer aber alle drei Angaben
//    zuverlaessig statt geraten.
//
// 2. Die Pegel fuer das VU-Meter und die Abtastwerte fuer das Spektrum
//    entstehen in AudioVuOut, einer eigenen Ausgabeklasse. Sie laeuft im
//    loop-Task - das Problem "nicht aus fremdem Task zeichnen" gibt es hier
//    also gar nicht erst.
//
// 3. connecttospeech() gibt es nicht. Der Sketch ruft es nicht auf.
// ===========================================================================
#pragma once

#include <FS.h>
#include <HTTPClient.h>

#include <AudioFileSourceICYStream.h>
#include <AudioFileSourceBuffer.h>
#include <AudioFileSourceFS.h>
#include <AudioGeneratorMP3.h>
#include <AudioGeneratorAAC.h>
#include <AudioOutputI2S.h>

// Rueckrufe des Sketches. Schwach angemeldet, damit diese Datei auch
// uebersetzt, wenn einer davon fehlt - audio_info() gibt es nur mit
// DEBUG_SERIAL.
void audio_info(const char*) __attribute__((weak));
void audio_showstation(const char*) __attribute__((weak));
void audio_showstreamtitle(const char*) __attribute__((weak));
void audio_id3data(const char*) __attribute__((weak));
void audio_process_i2s(int16_t*, uint16_t, uint8_t, uint8_t, bool*)
     __attribute__((weak));

// Wie viele Rahmen gesammelt werden, bevor audio_process_i2s() sie bekommt.
// 64 Rahmen sind 1,5 ms bei 44,1 kHz - klein genug, dass die FFT ihre 512
// Werte schnell zusammen hat, gross genug, dass der Aufruf nicht stoert.
#define AC_BLOCK_FRAMES 64

// ---------------------------------------------------------------------------
// 5-Band-Equalizer
//
// Fuenf Peaking-Filter je Kanal, +-6 dB in 1-dB-Schritten. Gerechnet wird in
// ConsumeSample(), also vor Amplify() und vor der I2S-Ausgabe - VU und
// Spektrum sehen damit das gefilterte Signal.
//
// Am 11.8.2026 gemessen, bevor davon etwas gebaut wurde: mit voller
// Filterarithmetik und ohne fiel der Tiefstwert des Vorpuffers bei einem
// 320-kBit-Sender in beiden Faellen nicht unter 99 %. Die Rechenzeit ist
// also kein Thema - anders als beim Speicher, siehe die Kommentare oben.
#define AC_EQ_BANDS 5

struct AcBiquad {
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    float z1 = 0.0f, z2 = 0.0f;      // Zustand, je Kanal getrennt
};

// Mittenfrequenzen. Dieselben Zahlen stehen als Beschriftung im Sketch.
static const float acEqFreq[AC_EQ_BANDS] =
    { 100.0f, 350.0f, 1000.0f, 3500.0f, 10000.0f };
static const float AC_EQ_Q = 1.0f;

static AcBiquad acEq[2][AC_EQ_BANDS];
static int8_t   acEqGainDb[AC_EQ_BANDS] = { 0, 0, 0, 0, 0 };
static int      acEqRate   = 44100;
static bool     acEqActive = false;   // steht alles auf 0, wird nicht gerechnet

// Vordaempfung, 6 dB, dauerhaft. SetGain() erreicht bei voller Lautstaerke
// 1,0 und Amplify() laeuft nach dem Filter - ein angehobenes Band ginge also
// ueber den Anschlag und kratzte hoerbar. 0,5 sind genau so viel Luft, wie
// ein Band hoechstens anhebt.
//
// Sie gilt **immer**, auch bei glatter Kurve. Eine Daempfung, die erst
// einsetzt, wenn ein Regler ueber 0 geht, macht die laufende Musik mitten im
// Einstellen leiser - am Geraet ausprobiert und verworfen. Der Preis: die
// Anlage spielt 6 dB leiser als ohne Equalizer.
//
// Am Geraet war sie eine Weile einstellbar, um auszuprobieren, wie viel Luft
// wirklich noetig ist. Antwort: diese 6 dB. Seit v3.2.0 steht sie wieder fest.
#define AC_EQ_PREGAIN 0.5f

// Peaking-EQ nach Robert Bristow-Johnson.
static void acEqCalcBand(int b) {
    const float A  = powf(10.0f, (float)acEqGainDb[b] / 40.0f);
    float w0 = 2.0f * (float)M_PI * acEqFreq[b] / (float)acEqRate;
    if (w0 > 3.0f) w0 = 3.0f;                  // Band ueber Nyquist: festhalten
    const float cs    = cosf(w0);
    const float alpha = sinf(w0) / (2.0f * AC_EQ_Q);
    const float a0    = 1.0f + alpha / A;

    const float c0 = (1.0f + alpha * A) / a0;
    const float c1 = (-2.0f * cs)       / a0;
    const float c2 = (1.0f - alpha * A) / a0;
    const float d1 = (-2.0f * cs)       / a0;
    const float d2 = (1.0f - alpha / A) / a0;

    for (int ch = 0; ch < 2; ch++) {
        // Nur die Koeffizienten, nicht den Zustand: ein Nullsetzen mitten im
        // Ton waere ein Knacken.
        acEq[ch][b].b0 = c0;
        acEq[ch][b].b1 = c1;
        acEq[ch][b].b2 = c2;
        acEq[ch][b].a1 = d1;
        acEq[ch][b].a2 = d2;
    }
}

static void acEqUpdateActive() {
    acEqActive = false;
    for (int b = 0; b < AC_EQ_BANDS; b++)
        if (acEqGainDb[b] != 0) { acEqActive = true; return; }
}

// Bei jedem Senderwechsel faellig: AAC+ kommt als Basisschicht mit 22.050 Hz
// herein, MP3 mit 44.100. Hier darf der Zustand weg, es laeuft ja kein Ton.
static void acEqSetRate(int hz) {
    if (hz <= 0) return;
    acEqRate = hz;
    for (int b = 0; b < AC_EQ_BANDS; b++) {
        acEqCalcBand(b);
        for (int ch = 0; ch < 2; ch++)
            acEq[ch][b].z1 = acEq[ch][b].z2 = 0.0f;
    }
    acEqUpdateActive();
}

static void acEqSetGain(int b, int db) {
    if (b < 0 || b >= AC_EQ_BANDS) return;
    if (db >  6) db =  6;
    if (db < -6) db = -6;
    acEqGainDb[b] = (int8_t)db;
    acEqCalcBand(b);
    acEqUpdateActive();
}

// Transponierte Direktform II, fuenf Baender hintereinander.
static inline int16_t acEqRun(int ch, int16_t in) {
    float x = (float)in * AC_EQ_PREGAIN;
    for (int b = 0; b < AC_EQ_BANDS; b++) {
        AcBiquad& f = acEq[ch][b];
        const float y = f.b0 * x + f.z1;
        f.z1 = f.b1 * x - f.a1 * y + f.z2;
        f.z2 = f.b2 * x - f.a2 * y;
        x = y;
    }
    // Sicherheitsnetz. Mit der Vordaempfung sollte es nicht mehr greifen.
    if (x >  32767.0f) x =  32767.0f;
    if (x < -32768.0f) x = -32768.0f;

    // Runden, nicht abschneiden. Ein Schnitt Richtung Null laesst einen
    // Fehler stehen, der mit dem Signal mitlaeuft - und das hoert man als
    // feines Kratzen, am 11.8.2026 ueber Kopfhoerer bemerkt. Runden macht
    // daraus ein Rauschen weit unter der Hoerschwelle.
    return (int16_t)lroundf(x);
}

// ---------------------------------------------------------------------------
// Fest belegter Speicher fuer Decoder und Vorpuffer
//
// Am 11.8.2026 um 14:02 hat ein japanischer AAC+-Sender das Geraet neu
// gestartet: beim Umschalten reichte der Heap nicht mehr fuer die DMA-
// Deskriptoren des I2S-Kanals, und AudioOutputI2S::begin() prueft das nicht,
// sondern hat ein assert darin. Tiefstand in den Heapzeilen davor: 200 Bytes.
//
// Die Bibliothek ist fuer diesen Fall gebaut: Generator und Vorpuffer nehmen
// auf Wunsch fertigen Speicher entgegen. Damit steht der Bedarf beim Uebersetzen
// fest, und was zur Laufzeit passiert, kann ihn nicht mehr kippen.
//
// Die beiden Zahlen stammen aus dem WebRadio-Beispiel der Bibliothek:
// 29.192 Bytes braucht der MP3-Decoder, 85.332 der AAC+-Decoder mit SBR.
// Beide Decoder laufen nie gleichzeitig, ein Feld genuegt fuer beide.
// Wieder 85.332, und diesmal begruendet. Mit 40.000 lief ein AAC+-Sender zwar
// an - Bytes flossen, Metadaten kamen -, es kam aber kein Ton: in libhelix-aac
// ist AAC_ENABLE_SBR gesetzt, AACInitDecoderPre() reicht den Rest des Feldes an
// InitSBRPre() weiter, und reicht der nicht, gibt es keinen Decoder.
//
// Die Kuerzung auf 40.000 war eine Notbremse gegen den knappen Heap - der war
// aber vom I2S-Leck verursacht, nicht von der Reservierung. Seit out.stop()
// unbedingt laeuft, stehen im Betrieb ueber 60 KB frei; die 45 KB mehr sind
// also da.
// 90.048, nicht die 85.332 aus dem WebRadio-Beispiel. Die Zahl dort stammt aus
// der Zeit vor PR #807 (18.3.2026, "Fix memory corruption in AudioGeneratorAAC
// on non-ESP8266 platforms") und wurde dabei nicht mitgezogen. Der Fix hat den
// Ausgabepuffer verdoppelt, weil SBR ausserhalb des ESP8266 an ist und der
// Decoder dann 2048 statt 1024 Abtastwerte je Kanal ablegt; die Testdatei
// desselben PR ging von 28000+60000 auf 28000+60000+2048.
//
// Mit den 85.332 fehlten 4.716 Bytes: InitSBRPre() scheiterte, es gab keinen
// Decoder, und alle AAC-Sender blieben stumm - unabhaengig von der Bitrate.
// Am 14.8.2026 von 90.048 auf 30.720 gesenkt, nachdem SBR aus der
// Bibliothek genommen wurde. Gemessen mit der Toolchain des Geraets:
//
//   MP3 (libmad):  buff 1.536 + stream 2.632 + frame 20.784 + synth 4.236
//                  = 29.188  <- der Groessere von beiden, er gibt den Ausschlag
//   AAC ohne SBR:  buff 1.600 + outSample 4.096 + AACDecInfo 96
//                  + PSInfoBase 20.560 = 26.352
//
// Mit SBR waren es 89.428 - daher die alten 90.048. Die 59.328 Bytes
// Unterschied sind am Geraet als freier Heap angekommen: statt 13 bis 23 KB
// sind es 77 bis 88, der groesste zusammenhaengende Block stieg von 7.668
// auf ueber 63.000.
#define AC_CODEC_BYTES 30720

// Sicherung. Ohne den Bibliotheks-Patch ist SBR an, dann braucht der
// AAC-Decoder 89.428 Bytes und faende hier nur 30.720 - er wuerde erst zur
// Laufzeit mit "Out of memory" aussteigen, und zwar nur bei AAC-Sendern.
// Lieber gleich beim Uebersetzen.
#if defined(AAC_ENABLE_SBR)
#error "AAC_ENABLE_SBR ist an. Diese Fassung erwartet ESP8266Audio ohne SBR - siehe library-patch/aac-sbr-aus.md"
#endif

static uint8_t acCodecMem[AC_CODEC_BYTES];

// ---------------------------------------------------------------------------
// Ausgabe mit Abgriff: Pegel fuer das VU-Meter, Bloecke fuer das Spektrum.
// ---------------------------------------------------------------------------
class AudioVuOut : public AudioOutputI2S {
public:
    // Beide Kanaele in einem Wort, links im oberen Byte - genau so, wie es
    // updateVuMeter() im Sketch erwartet. Das Lesen setzt zurueck, damit die
    // Anzeige den Spitzenwert seit dem letzten Bild zeigt.
    uint16_t vuLevel() {
        uint16_t v = ((uint16_t)peakL << 8) | peakR;
        peakL = 0;
        peakR = 0;
        return v;
    }

    // Was der Decoder zuletzt gemeldet hat - fuer den Info-Schirm.
    uint32_t lastRate()     { return rateHz; }
    uint8_t  lastChannels() { return chans; }

    // Die Koeffizienten haengen an der Abtastrate, siehe acEqSetRate().
    virtual bool SetRate(int hz) override {
        procPending = false;     // neuer Stream, nichts Altes anbieten
        rateHz = (hz > 0) ? (uint32_t)hz : 0;
        acEqSetRate(hz);

        // Mit DEBUG_SERIAL in der Konsole sichtbar: klingt ein Sender schraeg,
        // steht die Ursache oft hier - 22.050 Hz statt 44.100 etwa, oder Mono.
        if (audio_info) {
            char m[64];
            snprintf(m, sizeof(m), "Rate %d Hz", hz);
            audio_info(m);
        }
        return AudioOutputI2S::SetRate(hz);
    }

    virtual bool SetChannels(int ch) override {
        chans = (ch > 0 && ch < 256) ? (uint8_t)ch : 0;
        if (audio_info) {
            char m[64];
            snprintf(m, sizeof(m), "Kanaele %d", ch);
            audio_info(m);
        }
        return AudioOutputI2S::SetChannels(ch);
    }

    // Die Bibliothek bietet denselben Abtastwert **erneut** an, wenn er nicht
    // in den I2S-Puffer gepasst hat - AudioGeneratorMP3.cpp sagt es selbst:
    // "First, try and push in the stored sample. If we can't, then punt and
    // try later". Deshalb wird hier nur beim ersten Mal gerechnet und das
    // Ergebnis gemerkt. Wurde frueher der Wert an Ort und Stelle veraendert,
    // lief bei jeder Wiederholung der Filter noch einmal darueber und die
    // Daempfung wirkte doppelt - einzelne Werte fielen in ein Loch, und genau
    // das war das Kratzen vom 11.8.2026.
    //
    // Der Ursprungswert bleibt jetzt unberuehrt. Die Basisklasse kopiert sich
    // ihre eigene Fassung (AudioOutputI2S.cpp), das gemerkte Feld darf ihr
    // also mehrfach angeboten werden.
    virtual bool ConsumeSample(int16_t sample[2]) override {
        if (!procPending) {
            if (acEqActive) {
                proc[0] = acEqRun(0, sample[0]);
                proc[1] = acEqRun(1, sample[1]);
            } else {
                proc[0] = (int16_t)lroundf((float)sample[0] * AC_EQ_PREGAIN);
                proc[1] = (int16_t)lroundf((float)sample[1] * AC_EQ_PREGAIN);
            }

            // Fuer die Anzeige die Vordaempfung wieder herausrechnen: VU und
            // Spektrum sollen den Pegel des Senders zeigen. Steht ebenfalls
            // hier drin, sonst zaehlte ein wiederholter Wert doppelt.
            int32_t ml = (int32_t)((float)proc[0] / AC_EQ_PREGAIN);
            int32_t mr = (int32_t)((float)proc[1] / AC_EQ_PREGAIN);
            if (ml >  32767) ml =  32767;
            if (ml < -32768) ml = -32768;
            if (mr >  32767) mr =  32767;
            if (mr < -32768) mr = -32768;

            const int32_t l = ml < 0 ? -ml : ml;
            const int32_t r = mr < 0 ? -mr : mr;

            const uint8_t pl = (uint8_t)(l >> 8);   // 0..127, wie bisher
            const uint8_t pr = (uint8_t)(r >> 8);
            if (pl > peakL) peakL = pl;
            if (pr > peakR) peakR = pr;

            if (audio_process_i2s) {
                blk[2 * n]     = (int16_t)ml;
                blk[2 * n + 1] = (int16_t)mr;
                if (++n >= AC_BLOCK_FRAMES) {
                    bool cont = true;
                    audio_process_i2s(blk, n, 16, 2, &cont);
                    n = 0;
                }
            }

            procPending = true;
        }

        // Geht es nicht durch, wird genau dieses Ergebnis spaeter noch einmal
        // angeboten - ohne es neu zu berechnen.
        if (!AudioOutputI2S::ConsumeSample(proc)) return false;

        procPending = false;
        return true;
    }

private:
    uint8_t  peakL = 0, peakR = 0;
    uint32_t rateHz = 0;             // zuletzt gemeldete Abtastrate
    uint8_t  chans  = 0;             // ... und Kanalzahl
    int16_t  proc[2] = { 0, 0 };     // fertig gerechneter Wert
    bool     procPending = false;    // wartet noch auf den I2S-Puffer
    int16_t  blk[2 * AC_BLOCK_FRAMES];
    uint16_t n = 0;
};

// ---------------------------------------------------------------------------
// ICY-Strom, der mitzaehlt, wie viele Nutzbytes durchgehen.
//
// Notwendig, weil ESP8266Audio die Bitrate nirgends nennt. Der erste Anlauf
// nahm sie aus der Kopfzeile icy-br - und fiel auf die Nase, sobald ein
// Sender die nicht schickt: getBitRate() blieb 0, der Streamwaechter des
// Sketches ging nie auf OK, der Footer meldete "Stream unavailable", das
// VU-Meter wurde auf null gezwungen und der Wiederholzaehler baute die
// Verbindung alle paar Sekunden neu auf. Ein fehlender Kopfeintrag, vier
// Symptome.
//
// Gemessen ist besser als gemeldet: read() liefert hier bereits die reinen
// Audiodaten, die Metadaten hat ICYStream schon herausgeschnitten.
// ---------------------------------------------------------------------------
class CountingICYStream : public AudioFileSourceICYStream {
public:
    virtual uint32_t read(void* data, uint32_t len) override {
        uint32_t n = AudioFileSourceICYStream::read(data, len);
        bytes += n;
        return n;
    }

    // Beide Wege zaehlen, sonst stimmt die Rechnung nicht: AudioFileSourceBuffer
    // fuellt seinen Vorrat ueber readNonBlock() (AudioFileSourceBuffer.cpp:160,
    // 168, 178), read() wird nur nebenbei benutzt. Der erste Anlauf zaehlte nur
    // read() und zeigte deshalb 33 statt 320 kBit.
    virtual uint32_t readNonBlock(void* data, uint32_t len) override {
        uint32_t n = AudioFileSourceICYStream::readNonBlock(data, len);
        bytes += n;
        return n;
    }

    // Mittelt ueber je eine Sekunde. Liefert 0, solange noch nicht genug
    // Zeit vergangen ist, damit der Anrufer den alten Wert behaelt.
    uint32_t takeBitrate() {
        uint32_t now = millis();
        if (now - since < 1000) return 0;

        uint32_t bits = bytes * 8;
        uint32_t ms   = now - since;
        bytes = 0;
        since = now;

        return (bits * 1000UL) / ms;   // bit/s
    }

private:
    uint32_t bytes = 0;
    uint32_t since = millis();
};

// ---------------------------------------------------------------------------
// Wiedergabelisten
//
// Zwei verschiedene Dinge heissen so, und nur eines davon ist aufloesbar:
//
// .pls und einfaches .m3u sind kein Ton, sondern ein Zettel, auf dem die
// echte Stream-Adresse steht - bei SomaFM woertlich "File1=http://ice2...".
// Solche Adressen verteilen Sender auf ihren eigenen Seiten, sie landen also
// in station_list.txt und im Menue "Sender eintragen". Bis v3.2.0 endeten
// sie in "Playlist - not supported"; jetzt wird der Zettel gelesen.
//
// .m3u8/HLS enthaelt keine Stream-Adresse, sondern eine Liste von Haeppchen
// (media_649.ts, media_650.ts, ...), die im Sekundentakt neu geholt und
// aneinandergehaengt werden muessten. Das ist ein HLS-Client mit
// MPEG-TS-Demuxer - nichts, was hier hineinpasst. Erkennungszeichen ist eine
// Zeile "#EXT-X-...", die es in gewoehnlichen m3u nicht gibt.
//
// Am 14.8.2026 an 250 deutschen und 120 taiwanesischen Sendern des
// Verzeichnisses gemessen: in Deutschland keine einzige Liste (url_resolved
// ist dort schon aufgeloest), in Taiwan 21 - und alle 21 waren HLS. Der
// Auflöser ist also fuer selbst eingetragene Sender da, nicht fuer das
// Verzeichnis; dessen HLS-Sender sortiert rbParse() schon am hls-Merker aus.
// ---------------------------------------------------------------------------
#define AC_URL_MAX     256   // wie MAX_URL_LENGTH im Sketch
#define AC_LIST_MS     1500  // laenger wird auf eine Liste nicht gewartet
#define AC_LIST_LINES  30    // File1= steht in Zeile 3, danach kommt nichts mehr

// Endet der Pfad der URL auf ext? Der Frageteil zaehlt nicht mit -
// "...playlist.m3u8?token=1" ist eine Liste.
static bool acPathEndsWith(const char* url, const char* ext) {
    const char* end = url + strlen(url);
    for (const char* p = url; *p; p++) {
        if (*p == '?' || *p == '#') { end = p; break; }
    }
    size_t n = strlen(ext);
    if ((size_t)(end - url) < n) return false;
    return strncasecmp(end - n, ext, n) == 0;
}

static bool acUrlLooksLikeList(const char* url) {
    return acPathEndsWith(url, ".pls")
        || acPathEndsWith(url, ".m3u")
        || acPathEndsWith(url, ".m3u8");
}

// ---------------------------------------------------------------------------
// https_Crack
//
// TLS kann dieses Geraet nicht - mbedTLS legt mangels
// CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN zwei Puffer zu je 16 KB an, wo 13 bis
// 23 KB frei sind. Deshalb stand im Verzeichnisfilter jahrelang is_https=false.
//
// Nur: die meisten dieser Sender sind gar keine https-Sender, im Verzeichnis
// steht bloss die https-Adresse. Am 14.8.2026 gemessen an den 80
// meistgeklickten deutschen Sendern, die radio-browser als https fuehrt:
// **80 von 80** liefern denselben Ton ueber plain http - zehn unmittelbar,
// siebzig ueber eine Umleitung, die selbst wieder auf http zeigt (der folgt
// AudioFileSourceICYStream von allein). Kein einziger bestand auf TLS.
//
// Also wird es einfach versucht: https:// wird zu http://, der Rest der
// Adresse bleibt Zeichen fuer Zeichen stehen. Geht es schief, kostet es einen
// Verbindungsversuch, und der Sketch schreibt "https not work" in die
// Fusszeile. Das Verzeichnis darf seitdem auch https-Sender liefern.
//
// Gemessen ist das fuer Deutschland. Bei US-Sendern hinter grossen CDNs kann
// es anders liegen - dort faellt es dann eben auf die Meldung zurueck.
static bool acHttpsToHttp(const char* url, char* out, size_t cap) {
    if (strncasecmp(url, "https://", 8) != 0) return false;

    const char* rest = url + 8;              // alles hinter dem Schema
    if (strlen(rest) + 8 > cap) return false;  // "http://" + rest + Nullbyte

    strcpy(out, "http://");
    strcpy(out + 7, rest);
    return true;
}

// Eine Zeile lesen, ohne CR. false, wenn nichts mehr kommt oder die Zeit um
// ist. Die Frist gilt fuer den ganzen Vorgang, nicht je Zeile.
static bool acReadLine(WiFiClient* s, char* buf, size_t cap, uint32_t deadline) {
    size_t n = 0;
    while ((long)(millis() - deadline) < 0) {
        if (!s->available()) {
            if (!s->connected()) break;
            delay(5);
            continue;
        }
        int c = s->read();
        if (c < 0)    continue;
        if (c == '\r') continue;
        if (c == '\n') { buf[n] = '\0'; return true; }
        if (n + 1 < cap) buf[n++] = (char)c;
    }
    buf[n] = '\0';
    return n > 0;
}

// ---------------------------------------------------------------------------
// Die Huelle, die nach aussen aussieht wie ESP32-audioI2S.
// ---------------------------------------------------------------------------
class Audio {
public:
    Audio() {}

    bool setPinout(uint8_t bck, uint8_t ws, uint8_t dout) {
        // Beide Kanaele zu einer Summe mitteln, statt dem Codec die Auswahl
        // zu ueberlassen. Der Cardputer Adv hat einen ES8311 - einen
        // **Mono**-Codec mit einem einzigen DAC, an Lautsprecher wie an der
        // Kopfhoererbuchse. Ohne diese Zeile nimmt er sich den linken Kanal,
        // und was ein Sender hart nach rechts legt, waere verloren.
        //
        // VU-Meter und Spektrum sehen davon nichts: die messen in
        // AudioVuOut::ConsumeSample(), also vor dieser Stelle, und zeigen
        // weiterhin beide Kanaele getrennt.
        out.SetOutputModeMono(true);
        return out.SetPinout(bck, ws, dout);
    }

    // ram ist die Groesse des Vorpuffers vor dem Decoder, psram wird
    // uebergangen - der Cardputer Adv hat keines. 0 heisst Vorgabe.
    void setBufsize(int ram, int /*psram*/) {
        // Groesser als das feste Feld geht nicht - dann bliebe der Rest
        // ungenutzt und der Puffer liefe darueber hinaus.
        if (ram > 0 && (uint32_t)ram <= sizeof(streamMem)) {
            bufBytes = (uint32_t)ram;
        }
    }

    void setVolume(uint8_t v) {           // Sketch rechnet auf 0..21
        vol = v > 21 ? 21 : v;
        out.SetGain((float)vol / 21.0f);
    }

    void setBalance(int8_t) {}            // hier ohne Wirkung

    // Sprachausgabe gibt es in ESP8266Audio nicht. Der Sketch ruft das nur an
    // einer Stelle auf: als Rest aus dem Ursprungsprojekt liest er bei einer
    // unbrauchbaren URL einen portugiesischen Satz vor. false heisst hier
    // schlicht "nicht verbunden" - der Streamwaechter meldet das dann sauber,
    // statt dass jemand Portugiesisch hoert.
    bool connecttospeech(const char*, const char*) { return false; }

    bool isRunning() { return gen && gen->isRunning(); }

    // Gemessen schlaegt gemeldet. icyBitrate ist nur der Startwert aus dem
    // Kopf, damit schon der erste Footer eine Zahl zeigt.
    uint32_t getBitRate() {
        if (!isRunning()) return 0;
        return measuredBitrate ? measuredBitrate : icyBitrate;
    }

    const char* getCodecname() { return codecName; }

    // Abtastrate und Kanalzahl, wie der Decoder sie meldet. Bis zum
    // 15.8.2026 landeten beide nur in der Debugausgabe; der Info-Schirm
    // zeigt sie jetzt an. 0 heisst "noch nichts gehoert".
    uint32_t getSampleRate() { return out.lastRate(); }
    uint8_t  getChannels()   { return out.lastChannels(); }

    uint16_t getVUlevel() { return out.vuLevel(); }

    // Fuellstand des Vorpuffers in Prozent. Damit kann die Oberflaeche
    // entscheiden, ob gerade Zeit zum Zeichnen ist - der Ton hat Vorrang.
    uint8_t bufferFillPct() {
        if (!buf || bufBytes == 0) return 100;
        uint32_t f = buf->getFillLevel();
        if (f >= bufBytes) return 100;
        return (uint8_t)((f * 100UL) / bufBytes);
    }

    // Equalizer. Der Sketch haelt die Werte fuer Anzeige und NVS, hier liegt
    // die Rechnung.
    void   eqSetGain(int band, int db) { acEqSetGain(band, db); }
    int8_t eqGain(int band) {
        return (band >= 0 && band < AC_EQ_BANDS) ? acEqGainDb[band] : 0;
    }

    void stopSong() {
        if (gen) {
            if (gen->isRunning()) gen->stop();
            delete gen;
            gen = nullptr;
        }
        if (buf)  { delete buf;  buf  = nullptr; }
        if (src)  { delete src;  src  = nullptr; }
        if (file) { delete file; file = nullptr; }

        // Unbedingt, nicht nur wenn der Generator noch lief: out.stop() ruft
        // i2s_del_channel() und gibt damit die DMA-Deskriptoren frei. Ohne das
        // legt jeder Senderwechsel neue an und gibt die alten nie zurueck -
        // nach ein paar Wechseln scheitert i2s_alloc_dma_desc(), und
        // AudioOutputI2S::begin() hat dort ein assert. Genau das waren die
        // Neustarts um 14:02 und 14:13. Ein zweiter Aufruf schadet nicht, die
        // Methode prueft selbst mit i2sOn.
        out.stop();

        counting = nullptr;
        icyBitrate = 0;
        measuredBitrate = 0;
        codecName = "";
    }

    // true, wenn der letzte Verbindungsversuch an einer Wiedergabeliste
    // scheiterte. Der Sketch macht daraus eine eigene Fusszeilenmeldung.
    bool wasPlaylist() { return playlist; }

    // ... und true, wenn diese Liste eine HLS-Haeppchenliste war. Dann ist
    // nichts aufzuloesen, und die Fusszeile sagt etwas anderes.
    bool wasHls() { return hls; }

    bool connecttohost(const char* url) {
        stopSong();
        playlist = false;
        hls      = false;
        if (!url || !*url) return false;

        // Vorab die Kopfzeilen holen. Content-Type entscheidet ueber den
        // Decoder, icy-br liefert die Bitrate, icy-name den Sendernamen.
        // Ist es eine Wiedergabeliste, steht danach die echte Adresse in
        // resolved - sonst bleibt der Puffer leer.
        bool isAac = false;
        char resolved[AC_URL_MAX];

        // https_Crack, siehe oben. Muss vor allem anderen stehen: HTTPClient
        // wie ICYStream bekommen danach nur noch die http-Adresse zu sehen.
        // plain lebt bis zum Ende der Funktion, url zeigt hinein.
        char plain[AC_URL_MAX];
        if (acHttpsToHttp(url, plain, sizeof(plain))) {
            url = plain;
            if (audio_info) {
                String m = "https -> "; m += url;
                audio_info(m.c_str());
            }
        }

        // Umleitungen selbst verfolgen, hoechstens vier. ICYStream folgt zwar
        // von allein, aber es folgt auch auf https - und dort endet die Reise.
        // Hier wird jedes Ziel unterwegs auf http heruntergeschrieben.
        //
        // NTS Radio, am 17.8.2026 gemeldet und nachgemessen, ist genau dieser
        // Fall: stream-relay-geo.ntslive.net zeigt auf https://streams.
        // radiomast.io, das wiederum auf einen Knoten, der plain http spricht
        // und dort 256 kBit MP3 liefert. Ohne diese Schleife scheiterte es an
        // der ersten Umleitung, und die Fusszeile sagte "Stream unavailable".
        //
        // Kostet nichts, wo nicht umgeleitet wird: bei einer 200 bleibt
        // hop leer und die Schleife endet nach dem ersten Durchgang.
        char hop[AC_URL_MAX];
        for (int i = 0; i < 4; i++) {
            if (!probeHeaders(url, isAac, resolved, sizeof(resolved),
                              hop, sizeof(hop))) {
                // Kein Kopf zu bekommen: es trotzdem als MP3 versuchen, die
                // allermeisten Sender sind welche.
                isAac = false;
                break;
            }
            if (!hop[0]) break;              // keine Umleitung, fertig

            // Zeigt die Umleitung dorthin zurueck, wo eben angeklopft wurde,
            // ist nichts gewonnen - dann im Gegenteil.
            //
            // Genau das tun https-Sender: der https_Crack schreibt ihre
            // Adresse auf http um, der Server antwortet darauf mit "302, geh
            // nach https", und diese Schleife schriebe sie brav wieder auf
            // http. Vier Anfragen an dieselbe Adresse, dreimal umsonst - am
            // 17.8.2026 am Geraet als traeger Start und traege Listen
            // aufgefallen. Schlimmer noch: die drei Verbindungen zerstueckeln
            // den Heap so weit, dass logoFetch() an INFO_NEED_TLS haengen
            // bleibt und **gar keine Logos mehr** holt. Der groesste Block
            // liegt im Betrieb nur 1 bis 13 KB ueber dieser Schwelle.
            //
            // Also: einmal ist genug. Danach macht ICYStream weiter, das kann
            // http-Umleitungen von allein.
            if (!strcmp(hop, url)) break;

            strcpy(plain, hop);              // plain lebt bis zum Ende
            url = plain;
            if (audio_info) {
                String m = "redirect -> "; m += url;
                audio_info(m.c_str());
            }
        }

        // Zettel gelesen, Adresse gefunden: einmal umschwenken und den
        // richtigen Kopf holen - erst der sagt, ob es MP3 oder AAC ist.
        // Genau einmal. Eine Liste, die auf eine Liste zeigt, spielt dieses
        // Geraet nicht; der zweite Anlauf loest deshalb nicht weiter auf.
        if (playlist && resolved[0]) {
            url = resolved;
            if (audio_info) {
                String m = "playlist -> "; m += url;
                audio_info(m.c_str());
            }
            if (!probeHeaders(url, isAac, nullptr, 0)) isAac = false;
        }

        // Immer noch eine Liste: gar nicht erst anfangen. Der Sketch fragt mit
        // wasPlaylist() und wasHls() nach und schreibt es in die Fusszeile.
        if (playlist) return false;

        CountingICYStream* icy = new CountingICYStream();
        if (!icy) return false;

        // HTTP/1.0 statt 1.1. Grund ist Fehlerbericht #426 der Bibliothek:
        // Mit 1.1 antworten manche Sender in Chunked-Kodierung, und deren
        // Blockkoepfe landen im Audiostrom - hoerbar als Artefakte und
        // Aussetzer. Das Beispiel StreamOnHost.ino macht es genauso.
        icy->useHTTP10();

        // Eingebauter Wiederverbinder der Bibliothek: drei Anlaeufe mit
        // einer halben Sekunde Abstand, bevor sie aufgibt.
        icy->SetReconnect(3, 500);

        icy->RegisterMetadataCB(icyCallback, nullptr);
        if (!icy->open(url)) { delete icy; return false; }
        src = icy;
        counting = icy;

        buf = new AudioFileSourceBuffer(src, streamMem, bufBytes);
        if (!buf) { stopSong(); return false; }

        // Beide Decoder bekommen dasselbe Feld - es laeuft immer nur einer.
        if (isAac) {
            gen = new AudioGeneratorAAC(acCodecMem, AC_CODEC_BYTES);
            codecName = "AAC";
        } else {
            gen = new AudioGeneratorMP3(acCodecMem, AC_CODEC_BYTES);
            codecName = "MP3";
        }
        if (!gen) { stopSong(); return false; }

        if (!gen->begin(buf, &out)) { stopSong(); return false; }

        if (audio_info) audio_info("stream ready");
        return true;
    }

    bool connecttoFS(fs::FS& fs, const char* path) {
        stopSong();
        if (!path || !*path) return false;

        AudioFileSourceFS* f = new AudioFileSourceFS(fs);
        if (!f) return false;
        if (!f->open(path)) { delete f; return false; }
        file = f;
        src = f;

        gen = new AudioGeneratorMP3(acCodecMem, AC_CODEC_BYTES);
        codecName = "MP3";
        if (!gen || !gen->begin(src, &out)) { stopSong(); return false; }
        return true;
    }

    // Muss oft laufen. Ein Rueckgabewert false des Generators heisst: fertig
    // oder abgerissen - dann aufraeumen, damit isRunning() die Wahrheit sagt.
    void loop() {
        if (!gen) return;
        if (!gen->isRunning()) { stopSong(); return; }
        if (!gen->loop()) { stopSong(); return; }

        if (counting) {
            uint32_t br = counting->takeBitrate();
            if (br) measuredBitrate = br;
        }
    }

private:
    // ICY-Metadaten. Kommt als Schluessel/Wert; "StreamTitle" ist der Titel.
    static void icyCallback(void*, const char* type, bool, const char* str) {
        if (!type || !str) return;

        if (!strcmp(type, "StreamTitle")) {
            if (audio_showstreamtitle) audio_showstreamtitle(str);
        } else if (!strcmp(type, "icy-name")) {
            if (audio_showstation) audio_showstation(str);
        } else if (audio_id3data) {
            audio_id3data(str);
        }
    }

    // Liest die Liste zeilenweise und schreibt die erste spielbare Adresse
    // nach out. Setzt hls, wenn es eine Haeppchenliste ist - dann bleibt out
    // leer, denn da ist nichts zu holen.
    //
    // Der Puffer fasst eine Zeile, nicht die ganze Liste: gebraucht wird nur
    // die erste Adresse, und der Stapel ist hier knapp (die Schriften bauen
    // ihre Zeichen mit alloca darauf auf).
    void resolveList(WiFiClient* s, char* out, size_t cap) {
        const uint32_t deadline = millis() + AC_LIST_MS;
        char line[AC_URL_MAX];

        for (int i = 0; i < AC_LIST_LINES; i++) {
            if (!acReadLine(s, line, sizeof(line), deadline)) break;

            // "#EXT-X-..." gibt es nur in HLS. Es steht im Kopf der Liste,
            // also vor der ersten Adresse - hier ist Schluss.
            if (!strncasecmp(line, "#EXT-X-", 7)) { hls = true; return; }

            // "#EXTM3U", "#EXTINF:..." und Leerzeilen sind Beiwerk.
            if (line[0] == '#' || line[0] == '\0') continue;

            // Abgeschnittene Zeile. Eine Adresse daraus waere falsch und
            // fuehrte auf einen toten Stream statt auf eine ehrliche
            // Meldung - laenger als der Puffer passt sie ohnehin nicht in
            // stations[].url (MAX_URL_LENGTH, dieselbe Zahl).
            size_t len = strlen(line);
            if (len == sizeof(line) - 1) continue;

            // Angehaengte Leerzeichen kommen vor und gehoeren nicht zur URL.
            while (len && (line[len - 1] == ' ' || line[len - 1] == '\t')) {
                line[--len] = '\0';
            }

            // .pls schreibt "File1=http://...", .m3u die Adresse nackt.
            char* v = line;
            if (!strncasecmp(line, "File", 4)) {
                char* eq = strchr(line, '=');
                if (eq) v = eq + 1;
            }
            while (*v == ' ' || *v == '\t') v++;

            // Anfuehrungszeichen gehoeren nicht zur Adresse. bassdrive.com
            // schreibt File1="http://ice.bassdrive.net:80/stream" - eine
            // saubere .pls mit audio/x-scpls, die am 14.8.2026 nur an diesen
            // beiden Zeichen scheiterte. Das schliessende beendet die
            // Adresse; steht keines da, bleibt die Zeile wie sie ist.
            if (*v == '"' || *v == '\'') {
                const char q = *v++;
                char* end = strchr(v, q);
                if (end) *end = '\0';
            }

            // Alles andere ist unspielbar: https kann dieses Geraet nicht
            // (siehe is_https im Sketch), relative Namen sind HLS-Haeppchen.
            // Weitersuchen statt aufgeben - eine .pls listet oft mehrere
            // Server, und der naechste kann ein brauchbarer sein.
            if (strncasecmp(v, "http://", 7) != 0) continue;

            // Zeigt die Liste auf Haeppchen oder auf die naechste Liste,
            // ist es doch HLS.
            if (acPathEndsWith(v, ".ts") || acPathEndsWith(v, ".m3u8")) {
                hls = true;
                return;
            }

            strncpy(out, v, cap - 1);
            out[cap - 1] = '\0';
            return;
        }
    }

    // Kopfzeilen vorab lesen. true, wenn die Anfrage durchging.
    // out darf 0 sein - dann wird eine Liste nur erkannt, nicht aufgeloest.
    // redir/rcap sind neu: steht dort Platz bereit und antwortet der Server
    // mit einer Umleitung, landet ihr Ziel darin - schon auf http
    // heruntergeschrieben. Der Aufrufer klopft dann dort noch einmal an.
    // Warum nicht HTTPClient das machen lassen: der folgt zwar, aber er folgt
    // auch nach https, und dort kommt dieses Geraet nicht hin.
    bool probeHeaders(const char* url, bool& isAac, char* out, size_t cap,
                      char* redir = nullptr, size_t rcap = 0) {
        if (out && cap) out[0] = '\0';
        if (redir && rcap) redir[0] = '\0';
        playlist = false;

        HTTPClient h;
        const char* keys[] = { "Content-Type", "icy-br", "icy-name", "Location" };
        h.collectHeaders(keys, 4);

        if (!h.begin(url)) return false;
        h.setConnectTimeout(4000);
        h.setTimeout(4000);
        h.addHeader("Icy-MetaData", "1");

        // Sieht die Adresse schon nach Liste aus, wird sie in HTTP/1.0
        // geholt. Grund ist derselbe wie beim Stream weiter oben: unter 1.1
        // darf der Server chunked antworten, und die Blockkoepfe stehen dann
        // mitten im Rumpf - eine .pls aus einem PHP-Skript kommt gerne so.
        // Zeilenweise gelesen wuerde daraus eine zerschnittene Adresse.
        // Nur fuer diesen Fall, damit der gewoehnliche Weg zum Sender
        // unveraendert bleibt.
        if (out && cap && acUrlLooksLikeList(url)) h.useHTTP10();

        int code = h.GET();
        if (code <= 0) { h.end(); return false; }

        // Umleitung: Ziel merken und sonst nichts tun. Kopf und Typ hier
        // gehoeren der Umleitung, nicht dem Sender - danach zu entscheiden
        // waere falsch.
        if (redir && rcap
            && (code == 301 || code == 302 || code == 303
                || code == 307 || code == 308)) {
            String loc = h.header("Location");

            // **Nur bei https eingreifen.** Zeigt die Umleitung auf plain
            // http, wird sie hier nicht angefasst - dann folgt ihr
            // AudioFileSourceICYStream selbst, wie seit jeher.
            //
            // Der Grund ist teuer gelernt (17.8.2026): wer die aufgeloeste
            // Adresse hier anfragt, haengt am **Tonstrom**. Der Server
            // beginnt sofort zu senden, wir lesen drei Kopfzeilen, und
            // Sekundenbruchteile spaeter macht ICYStream dieselbe Adresse
            // ein zweites Mal auf. Zwei Stroeme ueber dieselbe Funkstrecke,
            // dazu Sitzungsmerker in der URL, die fuer die erste Verbindung
            // ausgestellt waren - bei 320 kBit gab das fast einen Unterlauf
            // je Sekunde. Ein Umleiter dagegen antwortet mit ein paar Bytes
            // und ist wieder zu; den anzufragen kostet nichts.
            //
            // Nur vollstaendige Adressen. Ein relativer Pfad muesste gegen
            // die alte zusammengesetzt werden; das kommt bei Sendern nicht
            // vor, und geraten wird hier nichts.
            if (loc.startsWith("https://")
                && acHttpsToHttp(loc.c_str(), redir, rcap)) {
                h.end();
                return true;
            }
            // Alles andere: weiter wie ohne diese Erweiterung.
        }

        String ct = h.header("Content-Type");
        String br = h.header("icy-br");
        String nm = h.header("icy-name");

        ct.toLowerCase();
        isAac = (ct.indexOf("aac") >= 0) || (ct.indexOf("aacp") >= 0);

        // Wiedergabelisten sind kein Ton. Wer sie an den MP3-Decoder gibt,
        // bekommt Rauschen - am 12.8.2026 an zwei taiwanesischen Sendern
        // gehoert, die HLS liefern (application/vnd.apple.mpegurl).
        playlist = (ct.indexOf("mpegurl") >= 0)      // m3u, m3u8, HLS
                || (ct.indexOf("scpls")   >= 0)      // pls
                || (ct.indexOf("playlist") >= 0);

        // Manche Server geben eine Liste als application/octet-stream aus -
        // radioparadise.com/m3u/mp3-128.m3u am 14.8.2026 nachgesehen. Dann
        // entscheidet die Dateiendung. Zwei Bedingungen dazu:
        //
        // Der Content-Type darf nicht schon Ton sagen - ein Sender, der unter
        // .m3u wirklich sendet, soll nicht an dieser Regel haengenbleiben.
        //
        // Und die Antwort muss eine 200 sein. Bei einer Umleitung stehen hier
        // Kopf und Typ der Umleitung, nicht die des Ziels: diese Vorpruefung
        // folgt keiner: AudioFileSourceICYStream tut es dagegen sehr wohl
        // (HTTPC_FORCE_FOLLOW_REDIRECTS, ICYStream.cpp:48). Ohne die Abfrage
        // wuerde ein umgeleiteter Sender mit .m3u in der Adresse hier als
        // Liste abgewiesen, obwohl der Stream danach spielt.
        if (!playlist
            && code == HTTP_CODE_OK
            && ct.indexOf("audio") < 0
            && ct.indexOf("ogg")   < 0
            && ct.indexOf("mpeg")  < 0) {
            playlist = acUrlLooksLikeList(url);
        }

        // Nur bei einer Liste in den Rumpf sehen, und nur bei einer 200. Ein
        // Audiostrom wird hier nicht angefasst - der wird gleich neu und
        // richtig aufgemacht.
        if (playlist && code == HTTP_CODE_OK && out && cap) {
            WiFiClient* s = h.getStreamPtr();
            if (s) resolveList(s, out, cap);
        }
        h.end();

        if (br.length()) icyBitrate = (uint32_t)br.toInt() * 1000;
        if (nm.length() && audio_showstation) audio_showstation(nm.c_str());

        if (audio_info) {
            String m = "content-type: " + ct;
            audio_info(m.c_str());
        }
        return true;
    }

    AudioVuOut          out;
    AudioGenerator*     gen  = nullptr;
    AudioFileSource*    src  = nullptr;
    AudioFileSourceBuffer* buf = nullptr;
    AudioFileSourceFS*  file = nullptr;

    CountingICYStream*  counting = nullptr;   // nur bei Netzstreams gesetzt

    // Vorpuffer, fest belegt wie der Decoderspeicher. AUDIO_INBUF_BYTES aus dem
    // Sketch darf kleiner sein, groesser nicht.
    static uint8_t streamMem[12000];

    uint32_t bufBytes        = sizeof(streamMem);
    bool     playlist        = false;   // letzter Versuch war eine Liste
    bool     hls             = false;   // ... und zwar eine HLS-Haeppchenliste
    uint32_t icyBitrate      = 0;      // aus dem Kopf, kann fehlen
    uint32_t measuredBitrate = 0;      // selbst gemessen, die verlaessliche
    const char* codecName = "";
    uint8_t  vol = 10;
};

// Speicherplatz der statischen Member.
uint8_t Audio::streamMem[12000];
