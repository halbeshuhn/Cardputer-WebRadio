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
#define AC_CODEC_BYTES 90048

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

    // Die Koeffizienten haengen an der Abtastrate, siehe acEqSetRate().
    virtual bool SetRate(int hz) override {
        procPending = false;     // neuer Stream, nichts Altes anbieten
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

    bool connecttohost(const char* url) {
        stopSong();
        playlist = false;
        if (!url || !*url) return false;

        // Vorab die Kopfzeilen holen. Content-Type entscheidet ueber den
        // Decoder, icy-br liefert die Bitrate, icy-name den Sendernamen.
        bool isAac = false;
        if (!probeHeaders(url, isAac)) {
            // Kein Kopf zu bekommen: es trotzdem als MP3 versuchen, die
            // allermeisten Sender sind welche.
            isAac = false;
        }

        // Wiedergabeliste: gar nicht erst anfangen. Der Sketch fragt mit
        // wasPlaylist() nach und schreibt es in die Fusszeile.
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

    // Kopfzeilen vorab lesen. true, wenn die Anfrage durchging.
    bool probeHeaders(const char* url, bool& isAac) {
        HTTPClient h;
        const char* keys[] = { "Content-Type", "icy-br", "icy-name" };
        h.collectHeaders(keys, 3);

        if (!h.begin(url)) return false;
        h.setConnectTimeout(4000);
        h.setTimeout(4000);
        h.addHeader("Icy-MetaData", "1");

        int code = h.GET();
        if (code <= 0) { h.end(); return false; }

        String ct = h.header("Content-Type");
        String br = h.header("icy-br");
        String nm = h.header("icy-name");
        h.end();

        ct.toLowerCase();
        isAac = (ct.indexOf("aac") >= 0) || (ct.indexOf("aacp") >= 0);

        // Wiedergabelisten sind kein Ton. Wer sie an den MP3-Decoder gibt,
        // bekommt Rauschen - am 12.8.2026 an zwei taiwanesischen Sendern
        // gehoert, die HLS liefern (application/vnd.apple.mpegurl).
        playlist = (ct.indexOf("mpegurl") >= 0)      // m3u, m3u8, HLS
                || (ct.indexOf("scpls")   >= 0)      // pls
                || (ct.indexOf("playlist") >= 0);

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
    uint32_t icyBitrate      = 0;      // aus dem Kopf, kann fehlen
    uint32_t measuredBitrate = 0;      // selbst gemessen, die verlaessliche
    const char* codecName = "";
    uint8_t  vol = 10;
};

// Speicherplatz der statischen Member.
uint8_t Audio::streamMem[12000];
