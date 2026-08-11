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

    virtual bool ConsumeSample(int16_t sample[2]) override {
        int16_t l = sample[0] < 0 ? -sample[0] : sample[0];
        int16_t r = sample[1] < 0 ? -sample[1] : sample[1];

        uint8_t pl = (uint8_t)(l >> 8);   // 0..127, wie bei der alten Library
        uint8_t pr = (uint8_t)(r >> 8);
        if (pl > peakL) peakL = pl;
        if (pr > peakR) peakR = pr;

        if (audio_process_i2s) {
            blk[2 * n]     = sample[0];
            blk[2 * n + 1] = sample[1];
            if (++n >= AC_BLOCK_FRAMES) {
                bool cont = true;
                audio_process_i2s(blk, n, 16, 2, &cont);
                n = 0;
            }
        }

        return AudioOutputI2S::ConsumeSample(sample);
    }

private:
    uint8_t  peakL = 0, peakR = 0;
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

    bool connecttohost(const char* url) {
        stopSong();
        if (!url || !*url) return false;

        // Vorab die Kopfzeilen holen. Content-Type entscheidet ueber den
        // Decoder, icy-br liefert die Bitrate, icy-name den Sendernamen.
        bool isAac = false;
        if (!probeHeaders(url, isAac)) {
            // Kein Kopf zu bekommen: es trotzdem als MP3 versuchen, die
            // allermeisten Sender sind welche.
            isAac = false;
        }

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
    uint32_t icyBitrate      = 0;      // aus dem Kopf, kann fehlen
    uint32_t measuredBitrate = 0;      // selbst gemessen, die verlaessliche
    const char* codecName = "";
    uint8_t  vol = 10;
};

// Speicherplatz der statischen Member.
uint8_t Audio::streamMem[12000];
