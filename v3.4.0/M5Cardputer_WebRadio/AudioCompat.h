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

// Die fuenf Frequenzen. **Dieselben Zahlen stehen als Beschriftung in
// eqFreqLabel[] im Sketch** - sie sind nicht gekoppelt, wer hier etwas
// aendert, muss sie dort mitziehen.
//
// Fuer die drei mittleren ist es die Mitte einer Glocke, fuer die beiden
// aeusseren die **Ecke eines Kuhschwanzes** - dort, wo die halbe Anhebung
// erreicht ist. Siehe acEqCalcBand().
static const float acEqFreq[AC_EQ_BANDS] =
    { 150.0f, 350.0f, 1000.0f, 3500.0f, 8000.0f };

// Guete der drei Glocken in der Mitte. 1,0 sind 1,39 Oktaven bei halber
// Anhebung - breit genug, dass sich Nachbarn nicht ins Gehege kommen, und
// schmal genug, dass jeder Regler seinen eigenen Bereich hat. Die beiden
// Kuhschwaenze aussen benutzen sie nicht, die haben ihre Steilheit S = 1.
static const float AC_EQ_Q = 1.0f;

// Die beiden aeusseren Baender sind Kuhschwaenze, keine Glocken.
//
// Warum: eine Glocke faellt hinter ihrer Mitte wieder ab. Beim obersten Band
// hiess das, dass die Anhebung genau dort aufhoert, wo die Luft anfaengt -
// und beim untersten, dass der Tiefbass unter der Mitte wieder verlorengeht.
// Ein Kuhschwanz hebt alles jenseits seiner Ecke an und bleibt dort.
//
// Fuer das oberste Band kommt ein handfester Grund dazu: **AAC+ liefert
// 22.050 Hz**, Nyquist liegt dann bei 11.025. Eine Glocke bei 10 kHz wird
// dort zur Nadel von 0,15 Oktaven (bei 8 kHz nur noch +0,7 dB von +6), eine
// bei 12,5 kHz liefe sogar in die Klemme unten in dieser Funktion. Der
// Kuhschwanz braucht keine Spitze unterhalb von Nyquist und wirkt auch dort:
// +3,0 dB bei 8 kHz, +5,9 bei 10 kHz. Am 16.8.2026 durchgerechnet.
#define AC_EQ_SHELF_LO 0
#define AC_EQ_SHELF_HI (AC_EQ_BANDS - 1)

static AcBiquad acEq[2][AC_EQ_BANDS];
static int8_t   acEqGainDb[AC_EQ_BANDS] = { 0, 0, 0, 0, 0 };
static int      acEqRate   = 44100;
static bool     acEqActive = false;   // steht alles auf 0, wird nicht gerechnet

// Vordaempfung, am Geraet einstellbar - links neben dem ersten Band.
//
// Wozu sie da ist: SetGain() erreicht bei voller Lautstaerke 1,0 und
// Amplify() laeuft **nach** dem Filter. Ein angehobenes Band ginge also ueber
// den Anschlag. Die feste Vordaempfung von 6 dB, die bis hierher immer anlag,
// ist raus - was gedaempft wird, sagt jetzt der Regler.
//
// Gezaehlt wird in **Daempfung**, so wie sie auf dem Schirm steht: 0 laesst
// das Signal, wie es kommt, positive Werte machen leiser, negative lauter.
// -6 dB ist also doppelte Amplitude und kann uebersteuern - ausdruecklich so
// gewollt, um den Punkt zu finden, an dem es kippt.
//
// Sie gilt **immer**, auch bei glatter Kurve. Eine Daempfung, die erst
// einsetzt, wenn ein Regler ueber 0 geht, macht die laufende Musik mitten im
// Einstellen leiser - am Geraet ausprobiert und verworfen.
// Einblenden nach dem Verbinden, siehe setVolume() weiter unten.
#define AC_FADE_WAIT_MS   500   // so lange bleibt es nach dem Start noch still
#define AC_FADE_RAMP_MS  2000   // und so lange dauert das Hochfahren

#define AC_EQ_PRE_MIN_DB (-6)   // lauter als der Sender
#define AC_EQ_PRE_MAX_DB (24)   // 16-fach leiser
#define AC_EQ_PRE_DEF_DB  (6)   // Vorgabe: dieselben 6 dB wie bisher fest
static int8_t acEqPreDb = AC_EQ_PRE_DEF_DB;
static float  acEqPre   = 0.501187f;   // 10^(-dB/20), zu acEqPreDb passend

// Glocke und Kuhschwanz nach Robert Bristow-Johnson, beide als ein Biquad.
// Welche Form ein Band bekommt, haengt nur an seiner Nummer - die Rechnung
// zur Laufzeit ist danach fuer alle fuenf dieselbe.
static void acEqCalcBand(int b) {
    const float A  = powf(10.0f, (float)acEqGainDb[b] / 40.0f);
    float w0 = 2.0f * (float)M_PI * acEqFreq[b] / (float)acEqRate;
    if (w0 > 3.0f) w0 = 3.0f;                  // Band ueber Nyquist: festhalten
    const float cs = cosf(w0);

    float c0, c1, c2, d1, d2;

    if (b == AC_EQ_SHELF_LO || b == AC_EQ_SHELF_HI) {
        // Steilheit S = 1, damit ist alpha = sin(w0)/2 * Wurzel 2. Das ist
        // die steilste Flanke, die noch nicht ueberschwingt.
        const float alpha = sinf(w0) * 0.70710678f;
        const float tsa   = 2.0f * sqrtf(A) * alpha;
        const float Ap    = A + 1.0f;
        const float Am    = A - 1.0f;

        if (b == AC_EQ_SHELF_LO) {
            const float a0 = Ap + Am * cs + tsa;
            c0 =  A * (Ap - Am * cs + tsa) / a0;
            c1 =  2.0f * A * (Am - Ap * cs) / a0;
            c2 =  A * (Ap - Am * cs - tsa) / a0;
            d1 = -2.0f * (Am + Ap * cs)     / a0;
            d2 =  (Ap + Am * cs - tsa)      / a0;
        } else {
            const float a0 = Ap - Am * cs + tsa;
            c0 =  A * (Ap + Am * cs + tsa) / a0;
            c1 = -2.0f * A * (Am + Ap * cs) / a0;
            c2 =  A * (Ap + Am * cs - tsa) / a0;
            d1 =  2.0f * (Am - Ap * cs)     / a0;
            d2 =  (Ap - Am * cs - tsa)      / a0;
        }
        // Bei 0 dB ist A = 1, damit Am = 0 - dann wird c0 = 1, c1 = d1 und
        // c2 = d2. Das Filter ist rechnerisch durchsichtig, kein Rest bleibt.
    } else {
        const float alpha = sinf(w0) / (2.0f * AC_EQ_Q);
        const float a0    = 1.0f + alpha / A;

        c0 = (1.0f + alpha * A) / a0;
        c1 = (-2.0f * cs)       / a0;
        c2 = (1.0f - alpha * A) / a0;
        d1 = (-2.0f * cs)       / a0;
        d2 = (1.0f - alpha / A) / a0;
    }

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

// Daempfung in dB, positiv = leiser. Begrenzt selbst auf AC_EQ_PRE_MIN_DB
// bis AC_EQ_PRE_MAX_DB.
static void acEqSetPreDb(int db) {
    if (db < AC_EQ_PRE_MIN_DB) db = AC_EQ_PRE_MIN_DB;
    if (db > AC_EQ_PRE_MAX_DB) db = AC_EQ_PRE_MAX_DB;
    acEqPreDb = (int8_t)db;
    acEqPre   = powf(10.0f, -(float)db / 20.0f);
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
    float x = (float)in * acEqPre;
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
// ---------------------------------------------------------------------------
// Lautstaerke, gleichmaessig hoerbar
//
// **Nicht ueber SetGain().** Die Bibliothek legt den Faktor in einem uint8_t
// im Festkommaformat 2.6 ab (AudioOutput.h: gainF2P6, Amplify() rechnet
// (s * gainF2P6) >> 6). Zwischen Stille und 0 dB liegen damit genau **64
// Stufen, linear in der Amplitude** - und weil das Ohr logarithmisch hoert,
// ist die Regelung unbrauchbar verteilt:
//
//     F2P6  64 =   0,0 dB
//     F2P6  32 =  -6,0 dB   Schritt darueber 0,27 dB
//     F2P6   4 = -24,1 dB   Schritt darueber 1,94 dB
//     F2P6   1 = -36,1 dB   Schritt darueber 6,02 dB
//     F2P6   0 = Stille
//
// Unter -36 dB gibt es nichts mehr, und der letzte Schritt vor der Stille ist
// ein Sprung von 6 dB. Eine gleichmaessige Kennlinie laesst sich da nicht
// hineinlegen, egal wie fein man vorne rechnet. Genau daran ist der erste
// Anlauf gescheitert.
//
// Deshalb bleibt SetGain() auf **1,0** stehen - Amplify() rechnet dann
// (s * 64) >> 6, also verlustfrei und nur noch als Begrenzer - und die
// Lautstaerke wird in ConsumeSample() selbst angewandt, mit Q16-Festkomma:
// 65.536 Stufen statt 64, und die Kennlinie darf krumm sein.
//
// Die Kennlinie: v = 0 ist Stille (daran haengt die Stummschaltung), 1..255
// laufen gleichmaessig in dB von AC_VOL_MIN_DB bis 0 dB. Mit VOLUME_STEP 10
// im Sketch sind das 26 Tastendruecke zu je rund 1,9 dB - jeder einzelne
// hoerbar, keiner wirkungslos.
//
// Vorher war es das Gegenteil: der erste hoerbare Druck brachte 6,02 dB, der
// letzte 0,42 dB, und vier der 26 Druecke aenderten gar nichts, weil
// map(0..255 -> 0..21) sie auf denselben Wert warf.
// ---------------------------------------------------------------------------
#define AC_VOL_MIN_DB (-48.0f)   // leiseste Stufe ueber Stille

static int32_t acVolQ = 65536;   // Q16, 65536 = 1,0

// Sample mal Faktor, gerundet und begrenzt. Q16 passt in int32: der
// schlimmste Fall -32768 * 65536 ist genau INT32_MIN.
static inline int16_t acVolAnwenden(int16_t s) {
    if (acVolQ == 65536) return s;
    if (acVolQ == 0) return 0;
    int32_t v = (int32_t)(((int64_t)s * acVolQ + 32768) >> 16);
    if (v >  32767) v =  32767;
    if (v < -32768) v = -32768;
    return (int16_t)v;
}

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
                // Dasselbe Sicherheitsnetz wie in acEqRun(): seit die
                // Daempfung auch negativ sein darf, ist acEqPre bis zu 2,0 -
                // ohne Begrenzung liefe der Wert ueber den int16 hinaus und
                // kippte ins Vorzeichen. Und runden, nicht abschneiden.
                float l = (float)sample[0] * acEqPre;
                float r = (float)sample[1] * acEqPre;
                if (l >  32767.0f) l =  32767.0f;
                if (l < -32768.0f) l = -32768.0f;
                if (r >  32767.0f) r =  32767.0f;
                if (r < -32768.0f) r = -32768.0f;
                proc[0] = (int16_t)lroundf(l);
                proc[1] = (int16_t)lroundf(r);
            }

            // Fuer die Anzeige die Vordaempfung wieder herausrechnen: VU und
            // Spektrum sollen den Pegel des Senders zeigen. Steht ebenfalls
            // hier drin, sonst zaehlte ein wiederholter Wert doppelt.
            int32_t ml = (int32_t)((float)proc[0] / acEqPre);
            int32_t mr = (int32_t)((float)proc[1] / acEqPre);
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

            // Lautstaerke ganz zuletzt, nach der VU-Messung: die Anzeige
            // soll den Pegel des Senders zeigen, nicht den des Reglers.
            //
            // **Innerhalb** dieses Blocks, nicht darunter. Geht das Sample
            // nicht durch, wird proc[] beim naechsten Aufruf erneut
            // angeboten - eine Skalierung ausserhalb liefe ein zweites Mal
            // darueber. Genau das war 2026 als feines Knistern hoerbar.
            proc[0] = acVolAnwenden(proc[0]);
            proc[1] = acVolAnwenden(proc[1]);

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

// AAC an der Adresse erkennen, wenn der Server keinen Content-Type schickt.
//
// Anlass am 19.8.2026: Radio SRF 3 leitet von https auf
// http://stream.srg-ssr.ch/srgssr/srf3/aac/96 um, und die Antwort dort kommt
// **ohne** Content-Type. isAac blieb damit false, ein AAC-Strom landete im
// MP3-Decoder, und der fand nie einen gueltigen Rahmen: "MP3:ERROR_BUFLEN 0",
// Unterlauf, verwerfen, von vorn - bis das Geraet nicht mehr bedienbar war.
//
// Gesucht wird "aac" nur dort, wo kein Buchstabe davorsteht - sonst zoege ein
// Sender namens "Isaac" die Erkennung mit. Was danach kommt, darf ruhig ein
// Buchstabe sein: "aacp", "aac_96", "aacplus" sind alle gemeint.
static bool acUrlLooksLikeAac(const char* url) {
    if (!url) return false;
    for (const char* p = url; (p = strcasestr(p, "aac")) != nullptr; p += 3) {
        const char davor = (p == url) ? '/' : p[-1];
        if (!isalpha((unsigned char)davor)) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Echtes https, seit 19.8.2026.
//
// Hier stand bis heute der https_Crack: jede https-Adresse wurde auf http
// heruntergeschrieben, weil mbedTLS zwei 16-KB-Puffer wollte, wo 13 bis 23 KB
// frei waren. Das ist erledigt. Was den Weg frei gemacht hat:
//
//   SBR-Ausbau                  +59 KB   (14.8.2026)
//   eigene ESP-IDF-Bibliotheken +20 KB am Stueck, +50 KB frei (18.8.2026)
//   Asymmetrie IN 16384/OUT 2048 -14 KB je TLS-Verbindung
//   Logopfad und Listen bei stillem Ton, Vorpuffer dynamisch
//
// Die Adresse geht jetzt unangetastet weiter, http wie https.
// AudioFileSourceICYStream ist gepatcht (clientForUrl(), siehe
// library-patch/http-tls) und legt bei https einen NetworkClientSecure an.
// ---------------------------------------------------------------------------

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
// Messfassung vom 19.8.2026: was jeder einzelne Schritt des
// Verbindungsaufbaus vom Heap nimmt.
//
// Anlass: nach dem Umstieg auf echtes https bekam jeder zweite Senderstart
// nur 36.000 statt 64.000 Vorpuffer. Was die Zeilen gezeigt haben:
//
//   - **Es ist TLS.** Bei einem https-Sender nimmt icy->open() 30,4 bis 43,3
//     KB und laesst den groessten Block bei 14.324 stehen; bei einem
//     http-Sender sind es 2,7 bis 13,1 KB und der Block bleibt bei 31.732.
//     Wer mehr als einen Handschlag machen muss (Umleitung auf einen anderen
//     Rechner, wie Deutschlandfunk nach d131.rndfnk.com), liegt am oberen
//     Ende. Warum es dann 43 statt der erwarteten 19 bis 21 KB sind, ist
//     nicht geklaert.
//   - Die Decke faellt einmal je Sitzung dauerhaft: 90.100 -> 65.524 im einen
//     Lauf, 86.004 -> 63.476 im anderen. Der freie Heap bleibt dabei bei rund
//     134 K. **Ursache ungeklaert** - einmal geschah es nach einem http-, im
//     anderen Lauf nach einem https-Sender, es gibt also keine Zuordnung, die
//     zu beiden Laeufen passt. Verwandt mit der Zerstueckelung vom 16.8.2026,
//     siehe cardputer-https-tls.
//   - Damit war eine Anforderung ueber 64.000 nach dem Verlust **nicht mehr
//     erfuellbar**: 524 Bytes fehlten an der Decke.
//
// Daraus folgte beides: der Vorpuffer wird zuerst genommen, und die
// Wunschgroesse liegt unter der Decke. Ergebnis am Geraet: zwoelf Starts,
// zwoelfmal die vollen 56.000, https wie http.
//
// (Die erste Auswertung dieser Zeilen war falsch zugeordnet - "[logo] Schirm
// sucht" wird **nach** dem Senderstart gezeichnet und gehoert zum Messblock
// davor, nicht danach. Daher stand hier zwischenzeitlich das Gegenteil.)
//
// Auf 0 setzen, dann faellt alles davon weg.
// ---------------------------------------------------------------------------
#define AC_TLS_MESSUNG 0

#if AC_TLS_MESSUNG
static void acMess(const char* was, int nr = -1) {
    if (!audio_info) return;
    char m[96];
    if (nr >= 0) {
        snprintf(m, sizeof(m), "[mess] %s %d: frei %lu  Block %lu", was, nr,
                 (unsigned long)ESP.getFreeHeap(),
                 (unsigned long)ESP.getMaxAllocHeap());
    } else {
        snprintf(m, sizeof(m), "[mess] %s: frei %lu  Block %lu", was,
                 (unsigned long)ESP.getFreeHeap(),
                 (unsigned long)ESP.getMaxAllocHeap());
    }
    audio_info(m);
}
#define AC_MESS(...) acMess(__VA_ARGS__)
#else
#define AC_MESS(...) do {} while (0)
#endif

// ---------------------------------------------------------------------------
// Vorfuellen des Ringpuffers, bevor der Decoder das erste Byte zieht.
//
// Am 19.8.2026 am Geraet gemessen: ein Sender startete mit 21 % Fuellstand,
// fiel auf 0 % und stotterte, bis der Vorrat ueber rund 28 % gestiegen war.
// Dasselbe beim Verlassen der Onlineliste - dort wird der Stream neu
// aufgebaut, also faengt der Vorrat wieder bei null an.
//
// 50 % von 56.000 sind 28.000 Bytes: 0,7 s bei 320 kBit, 1,75 s bei 128.
// Die Frist ist die Notbremse fuer Sender, die nur im Echtzeittakt liefern -
// dann faengt der Ton eben mit dem an, was bis dahin da ist.
// ---------------------------------------------------------------------------
#define AC_PREFILL_PCT 50
#define AC_PREFILL_MS  3000

// ---------------------------------------------------------------------------
// Notausstieg fuer unspielbare Stroeme, 19.8.2026.
//
// Anlass: Radio SRF 3 leitete auf einen AAC-Strom ohne Content-Type um, der
// im MP3-Decoder landete. Der sucht dann innerhalb EINES loop()-Aufrufs
// endlos nach einem gueltigen Rahmen und laesst immer wieder nachfuellen -
// das Geraet war nicht mehr bedienbar, im Mitschnitt keine einzige
// [heap]-Zeile mehr, und der Streamwaechter kam nicht dran, denn der laeuft
// selbst in loop().
//
// Also zaehlt die Unterlaufmeldung mit. Mehr als AC_UNTERLAUF_MAX in
// AC_UNTERLAUF_MS heisst "unspielbar": das Merkmal im Puffer wird gesetzt,
// read() liefert 0 Bytes, der Generator beendet sich, loop() kehrt zurueck,
// und der Waechter im Sketch meldet den Fehlschlag wie bei jedem anderen
// toten Sender.
//
// Die Schwelle ist bewusst hoch: ein gesunder Sender hat **null** Unterlaeufe
// (gemessen am 19.8.2026 ueber einen ganzen Mitschnitt), ein stotternder auf
// schwachem Netz ein paar vereinzelte. SRF 3 machte acht in der Sekunde.
// ---------------------------------------------------------------------------
#define AC_UNTERLAUF_MAX 10
#define AC_UNTERLAUF_MS  3000

static AudioFileSourceBuffer* acNotausPuffer   = nullptr;
static uint32_t               acUnterlaufAb    = 0;
static uint16_t               acUnterlaufZahl  = 0;

// ---------------------------------------------------------------------------
// Sammeln statt hungern - die Erholung nach einer Funkdelle
//
// Was ohne diese Sperre passiert, steht in AudioFileSourceBuffer.cpp:163: hat
// der Ring nicht genug, holt read() den Rest **blockierend** aus dem Netz
// (bis 500 ms, AudioFileSourceHTTPStream.cpp:116), wirft dann den **ganzen**
// Ring weg (length = 0, filled = false) und faengt von vorn an. Der naechste
// Zugriff nimmt wieder, was in 500 ms hereinkam - vielleicht 8 KB in einem
// 56-KB-Ring - und die sind sofort wieder verbraucht. Das ist die Schleife:
// ein Fetzen Ton, ein Loch, ein Fetzen Ton. Und weil read() dabei blockiert,
// ruckelt die Oberflaeche mit.
//
// Die Sperre setzt eine Ebene hoeher an, dort, wo noch niemand hingesehen
// hatte: **gen->loop() wird gar nicht erst aufgerufen.** Der Gedanke stammt
// von **Heotsan**, der ihn am 21.8.2026 unter Meldung #3 des Repos
// beschrieben hat - an seiner eigenen Fassung erprobt, mit 12 KB Ring.
// Uebernommen mit Dank; die Schwellen hier sind auf unsere 56 KB gerechnet. Damit gibt es kein
// read(), keinen Unterlauf und kein Verwerfen. Der Puffer fuellt sich
// derweil ueber buf->loop() weiter, und das laeuft ueber readNonBlock -
// blockiert also nichts.
//
// Der Weg ueber readNonBlock im Puffer selbst, am 19.8.2026 verworfen, waere
// falsch geblieben: ein read() mit 0 Bytes beendet den Generator
// (AudioGeneratorMP3.cpp:144). Nicht bedienen ist etwas anderes als leer
// bedienen.
//
// Gehoert wird waehrenddessen **Stille**, nicht der wiederholte DMA-Block -
// dafuer sorgt auto_clear im I2S-Treiber, siehe library-patch/i2s-stille.
//
// Die Schwellen in Bytes, gerechnet bei 128 kBit = 16 kB/s:
//   AC_SAMMEL_AB   8.000 = 0,5 s Vorrat. Darunter lohnt Weiterspielen nicht.
//   AC_SAMMEL_BIS 24.000 = 1,5 s. So viel Vorsprung, dass die naechste Delle
//                          nicht sofort wieder trifft.
// Beide sind auf den 56-KB-Ring des Radios gemuenzt; im Dateibetrieb mit
// 32 KB passen sie ebenso.
// ---------------------------------------------------------------------------
#define AC_SAMMEL_AB      8000
#define AC_SAMMEL_BIS    24000

// Wie lange gesammelt werden darf, bevor der Sender als tot gilt. Derselbe
// Wert wie STREAM_LOST_MS im Sketch - laenger schweigen heisst: die Quelle
// ist weg, nicht langsam.
//
// **Diese Grenze ist nicht schmueckendes Beiwerk.** Der Notausstieg oben
// zaehlt Unterlaeufe, und die gibt es mit der Sperre nicht mehr. Ohne eine
// eigene Uhr bliebe das Radio bei toter Quelle fuer immer still stehen: der
// Generator laeuft ja, isRunning() sagt ja, und getBitRate() haelt den
// zuletzt gemessenen Wert fest (loop(): "if (br) measuredBitrate = br"). Der
// Streamwaechter saehe nichts.
#define AC_SAMMEL_TOT_MS  8000

static bool     acSammelt   = false;   // Decoder ausgesetzt, Puffer fuellt
static uint32_t acSammeltAb = 0;       // seit wann
static bool     acStilleTot = false;   // 8 s nichts - der Sketch fragt es ab

// Der Puffer meldet Unterlauf und Nachfuellen selbst - bisher hat das niemand
// abgeholt. Gedrosselt, weil eine Meldung je Unterlauf bei einem hungernden
// Sender die Schnittstelle zustopft und damit selbst zum Stottern beitraegt.
static void acBufferStatus(void*, int code, const char* str) {
    if (code == AudioFileSourceBuffer::STATUS_UNDERFLOW) {
        const uint32_t nun = millis();
        if (nun - acUnterlaufAb > AC_UNTERLAUF_MS) {
            acUnterlaufAb   = nun;
            acUnterlaufZahl = 0;
        }
        if (++acUnterlaufZahl > AC_UNTERLAUF_MAX
            && acNotausPuffer && !acNotausPuffer->abortNow) {
            acNotausPuffer->abortNow = true;
            if (audio_info) audio_info("[puffer] unspielbar - Notausstieg");
        }
    }

    if (!audio_info || !str) return;

    static uint32_t letzte = 0;
    static uint32_t verschluckt = 0;
    const uint32_t jetzt = millis();

    if (jetzt - letzte < 250) { verschluckt++; return; }
    letzte = jetzt;

    char m[96];
    if (verschluckt) {
        snprintf(m, sizeof(m), "[puffer] %s (und %lu weitere)", str,
                 (unsigned long)verschluckt);
        verschluckt = 0;
    } else {
        snprintf(m, sizeof(m), "[puffer] %s", str);
    }
    audio_info(m);
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
        // Nur der Wunsch. Ob er erfuellbar ist, entscheidet sich beim
        // Verbinden - dort wird angefordert, mit Rueckfall.
        if (ram > 0) bufBytes = (uint32_t)ram;
    }

    // ---------------------------------------------------------------------
    // Einblenden nach dem Verbinden
    //
    // Beim Zappen knackte es am Kopfhoerer: der Decoder setzt mit voller
    // Lautstaerke ein, und was am Anfang eines Stroms steht, ist selten eine
    // Null - der Sprung ist als Knacken zu hoeren.
    //
    // Deshalb: solange keine Verbindung steht, ist der Ausgang stumm. Sobald
    // der Generator laeuft, bleibt es AC_FADE_WAIT_MS still, dann faehrt der
    // Pegel in AC_FADE_RAMP_MS linear auf die eingestellte Lautstaerke.
    //
    // Der Faktor sitzt vor SetGain() und nicht in vol: eine Lautstaerke, die
    // waehrend des Einblendens verstellt wird, gilt sofort und richtig - die
    // Blende multipliziert nur.
    // ---------------------------------------------------------------------
    void setVolume(uint8_t v) {           // 0..255, wie curVolume im Sketch
        vol = v;
        applyGain();
    }

    // Was die eingestellte Stufe wirklich bedeutet - fuer den Info-Schirm
    // und zum Nachmessen.
    float volumeDb() {
        if (!vol) return -99.0f;
        return AC_VOL_MIN_DB * (1.0f - (float)vol / 255.0f);
    }

    // Blende sofort auf stumm und anhalten. Ruft stopSong() beim Wechsel.
    void fadeMute() {
        fadeStart = 0;
        fadeGain  = 0.0f;
        applyGain();
    }

    // Von hier an laeuft die Blende: erst warten, dann hochfahren.
    void fadeBegin() {
        fadeStart = millis();
        if (!fadeStart) fadeStart = 1;
        fadeGain = 0.0f;
        applyGain();
    }

    // Einen Schritt weiter. Kommt aus loop(), also im Takt des Decoders.
    void fadeStep() {
        if (!fadeStart) return;

        const uint32_t dt = millis() - fadeStart;
        float f;
        if (dt < AC_FADE_WAIT_MS) {
            f = 0.0f;
        } else if (dt < AC_FADE_WAIT_MS + AC_FADE_RAMP_MS) {
            f = (float)(dt - AC_FADE_WAIT_MS) / (float)AC_FADE_RAMP_MS;
        } else {
            f = 1.0f;
            fadeStart = 0;
        }

        if (f != fadeGain) { fadeGain = f; applyGain(); }
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

    // Sammelt der Puffer gerade? Dann ist es still, und zwar mit Absicht.
    bool isRebuffering() { return acSammelt; }

    // Hat die Sammelsperre aufgegeben, weil AC_SAMMEL_TOT_MS lang nichts kam?
    // Einmal abholen, danach ist die Meldung verbraucht - der Sketch macht
    // daraus seine Fusszeile.
    bool takeStalled() {
        const bool t = acStilleTot;
        acStilleTot = false;
        return t;
    }

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

    // Vordaempfung in dB, positiv = leiser. Siehe AC_EQ_PRE_*.
    void   eqSetPreDb(int db) { acEqSetPreDb(db); }
    int8_t eqPreDb()          { return acEqPreDb; }

    int8_t eqGain(int band) {
        return (band >= 0 && band < AC_EQ_BANDS) ? acEqGainDb[band] : 0;
    }

    void stopSong() {
        fadeMute();               // ab hier ist der Ausgang still

        if (gen) {
            if (gen->isRunning()) gen->stop();
            delete gen;
            gen = nullptr;
        }
        // Erst abmelden, dann loeschen: der Notausstieg darf nie auf einen
        // freigegebenen Puffer zeigen.
        acNotausPuffer = nullptr;
        acSammelt      = false;     // ohne Puffer gibt es nichts zu sammeln
        if (buf)  { delete buf;  buf  = nullptr; }

        // Erst der Puffer-Wrapper, dann der Speicher darunter - andersherum
        // haette buf noch einen Zeiger auf Freigegebenes.
        if (streamMem) { free(streamMem); streamMem = nullptr; streamMemBytes = 0; }
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

        AC_MESS("Start, Audio abgebaut");

        // ---------------------------------------------------------------
        // Vorpuffer **zuerst**, vor jedem Netzverkehr. Seit 19.8.2026.
        //
        // Vorher stand diese Anforderung hinter icy->open(), und dort war der
        // Heap schon zerlegt: ein https-Start nimmt darin 30 bis 43 KB und
        // laesst nur 14.324 am Stueck stehen. Eine Anforderung ueber 64.000
        // fiel danach auf 36.000 zurueck - also genau bei den Sendern, fuer
        // die der ganze Umbau gemacht wurde.
        //
        // Jetzt wird der Platz genommen, solange er da ist. Was danach kommt -
        // Handschlag, Kopfzeilen, Umleitung - muss mit dem Rest auskommen.
        // Das ist am 19.8.2026 am Geraet nachgemessen und geht auf: nach dem
        // Vorpuffer bleiben rund 31,7 KB am Stueck, und damit kommt jeder
        // TLS-Handschlag zustande. Zwoelf Starts, zwoelfmal 56.000.
        //
        // Der Preis steht in denselben Zeilen: waehrend ein https-Sender
        // laeuft, sind noch 13,8 bis 28,5 KB frei und der groesste Block liegt
        // bei 7.668 bis 9.716. Das ist die duennste Luft, die dieses Programm
        // je hatte - kein Absturz in der Messung, aber nichts mehr zu
        // verschenken.
        // ---------------------------------------------------------------
        if (!streamMem) {
            AC_MESS("vor dem Vorpuffer");
            uint32_t w = bufBytes;
            while (w >= 8000 && !streamMem) {
                streamMem = (uint8_t*)malloc(w);
                if (streamMem) { streamMemBytes = w; break; }
#if AC_TLS_MESSUNG
                // Jede abgelehnte Stufe einzeln nennen. Bisher stand am Ende
                // nur, womit es geklappt hat - nicht, wie weit es daneben lag.
                if (audio_info) {
                    char m[96];
                    snprintf(m, sizeof(m),
                             "[mess] Vorpuffer %lu abgelehnt, Block %lu",
                             (unsigned long)w,
                             (unsigned long)ESP.getMaxAllocHeap());
                    audio_info(m);
                }
#endif
                w = (w * 3) / 4;
            }
            AC_MESS("nach dem Vorpuffer");
        }
        if (!streamMem) { stopSong(); return false; }

        if (audio_info && streamMemBytes != bufBytes) {
            char m[80];
            snprintf(m, sizeof(m), "Vorpuffer nur %lu statt %lu",
                     (unsigned long)streamMemBytes, (unsigned long)bufBytes);
            audio_info(m);
        }

        // Puffer fuer eine Adresse, die unterwegs ausgetauscht wird. Er lebt
        // bis zum Ende der Funktion, url zeigt hinein. Frueher stand hier der
        // https_Crack und schrieb das Schema um - seit dem 19.8.2026 nicht
        // mehr, die Adresse geht so weiter, wie sie kam.
        char plain[AC_URL_MAX];

        // Umleitungen selbst verfolgen, hoechstens vier. ICYStream folgt zwar
        // von allein, aber nur solange das Schema gleich bleibt: HTTPClient
        // bekommt seinen Client beim begin() fest zugewiesen und behaelt ihn
        // ueber die Umleitung hinweg. Ein einfacher Client kann dann kein
        // https, ein TLS-Client kein plain http. Genau die Wechsel nimmt
        // diese Schleife ihm ab (siehe probeHeaders).
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
                // Kein Kopf zu bekommen: dann entscheidet die Adresse, und
                // sagt auch die nichts, wird MP3 versucht - die allermeisten
                // Sender sind welche.
                isAac = acUrlLooksLikeAac(url);
                break;
            }
            AC_MESS("nach probeHeaders", i);

            if (!hop[0]) break;              // keine Umleitung, fertig

            // Zeigt die Umleitung dorthin zurueck, wo eben angeklopft wurde,
            // ist nichts gewonnen. Mit dem alten https_Crack war das der
            // Normalfall (er schrieb auf http um, der Server schickte zurueck
            // nach https) und kostete drei Anfragen umsonst. Die Abfrage
            // bleibt trotzdem stehen: sie kostet nichts und faengt jede
            // Schleife ab, die ein Server sonst noch baut.
            if (!strcmp(hop, url)) break;

            strcpy(plain, hop);              // plain lebt bis zum Ende
            url = plain;
            if (audio_info) {
                char m[AC_URL_MAX + 16];
                snprintf(m, sizeof(m), "redirect -> %s", url);
                audio_info(m);
            }
        }

        // Zettel gelesen, Adresse gefunden: einmal umschwenken und den
        // richtigen Kopf holen - erst der sagt, ob es MP3 oder AAC ist.
        // Genau einmal. Eine Liste, die auf eine Liste zeigt, spielt dieses
        // Geraet nicht; der zweite Anlauf loest deshalb nicht weiter auf.
        if (playlist && resolved[0]) {
            url = resolved;
            if (audio_info) {
                char m[AC_URL_MAX + 16];
                snprintf(m, sizeof(m), "playlist -> %s", url);
                audio_info(m);
            }
            if (!probeHeaders(url, isAac, nullptr, 0)) isAac = false;
        }

        // Immer noch eine Liste: gar nicht erst anfangen. Der Sketch fragt mit
        // wasPlaylist() und wasHls() nach und schreibt es in die Fusszeile.
        //
        // stopSong() statt eines nackten return: der Vorpuffer liegt seit dem
        // 19.8.2026 schon hier, und wer ohne ihn zurueckkehrt, laesst 56 KB
        // stehen, bis zufaellig der naechste Sender laeuft.
        if (playlist) { stopSong(); return false; }

        CountingICYStream* icy = new CountingICYStream();
        if (!icy) { stopSong(); return false; }

        // HTTP/1.0 statt 1.1. Grund ist Fehlerbericht #426 der Bibliothek:
        // Mit 1.1 antworten manche Sender in Chunked-Kodierung, und deren
        // Blockkoepfe landen im Audiostrom - hoerbar als Artefakte und
        // Aussetzer. Das Beispiel StreamOnHost.ino macht es genauso.
        icy->useHTTP10();

        // Eingebauter Wiederverbinder der Bibliothek: drei Anlaeufe mit
        // einer halben Sekunde Abstand, bevor sie aufgibt.
        icy->SetReconnect(3, 500);

        icy->RegisterMetadataCB(icyCallback, nullptr);
        if (!icy->open(url)) { delete icy; stopSong(); return false; }
        src = icy;
        counting = icy;

        // Hier steckt bei https alles drin, was ICYStream selbst aufgemacht
        // hat - auch der Sprung auf einen anderen Rechner mit eigenem
        // Handschlag.
        AC_MESS("nach icy->open");

        buf = new AudioFileSourceBuffer(src, streamMem, streamMemBytes);
        if (!buf) { stopSong(); return false; }

        buf->RegisterStatusCB(acBufferStatus, nullptr);
        acNotausPuffer  = buf;      // fuer den Notausstieg
        acUnterlaufZahl = 0;
        acSammelt       = false;    // die Sperre gilt je Sender
        acStilleTot     = false;
        acUnterlaufAb   = millis();

        // Erst sammeln, dann spielen. Die Bibliothek faengt von sich aus mit
        // dem an, was in einer Lesefrist ankommt - das war bei knappen
        // Sendern ein Fingerhut voll, und jeder Unterlauf warf den Ring
        // wieder ganz weg. Hier wird gewartet, bis es traegt.
        {
            const uint32_t ziel = (streamMemBytes / 100) * AC_PREFILL_PCT;
            const uint32_t t0   = millis();
            const uint32_t hat  = buf->preFill(ziel, AC_PREFILL_MS);
            if (audio_info) {
                char m[96];
                snprintf(m, sizeof(m),
                         "Vorfuellen: %lu von %lu Bytes in %lu ms",
                         (unsigned long)hat, (unsigned long)ziel,
                         (unsigned long)(millis() - t0));
                audio_info(m);
            }
        }

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

        fadeBegin();              // ab jetzt kommt Ton - erst still, dann auf

        AC_MESS("fertig");
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
        fadeBegin();
        return true;
    }

    // Muss oft laufen. Ein Rueckgabewert false des Generators heisst: fertig
    // oder abgerissen - dann aufraeumen, damit isRunning() die Wahrheit sagt.
    void loop() {
        if (!gen) return;
        if (!gen->isRunning()) { stopSong(); return; }

        // Sammelt der Puffer gerade, wird der Decoder nicht bedient. Siehe
        // den Block bei AC_SAMMEL_AB.
        if (buf) {
            const uint32_t fuell = buf->getFillLevel();

            if (acSammelt) {
                // buf->loop() fuellt ueber readNonBlock nach und liefert
                // false, wenn die Quelle zu Ende ist - beim Radio das
                // Abreissen, im Dateibetrieb das Ende der Folge.
                if (!buf->loop()) { acSammelt = false; stopSong(); return; }

                // Nicht mehr verlangen, als der Ring hergibt. Im
                // Dateibetrieb sind es 32.000 statt 56.000 - feste 24.000
                // waeren dort 75 %, und bei knappem Nachschub kaeme er nie
                // hin. Dann haette AC_SAMMEL_TOT_MS einen langsamen Strom
                // faelschlich fuer tot erklaert.
                const uint32_t ziel = (bufBytes && AC_SAMMEL_BIS > bufBytes * 3 / 5)
                                      ? bufBytes * 3 / 5 : (uint32_t)AC_SAMMEL_BIS;
                if (fuell >= ziel) {
                    acSammelt = false;
                    if (audio_info) audio_info("[puffer] wieder Vorrat, weiter");
                } else if (millis() - acSammeltAb > AC_SAMMEL_TOT_MS) {
                    // Nichts mehr gekommen. Der Notausstieg zaehlt Unterlaeufe,
                    // und die gibt es hier nicht - also selbst beenden und dem
                    // Sketch sagen, warum.
                    acSammelt   = false;
                    acStilleTot = true;
                    if (audio_info) audio_info("[puffer] kein Nachschub - Sender tot");
                    stopSong();
                    return;
                } else {
                    return;               // still sammeln, kein Ton
                }
            } else if (fuell < AC_SAMMEL_AB) {
                acSammelt   = true;
                acSammeltAb = millis();
                if (audio_info) audio_info("[puffer] leer - sammeln statt hungern");
                buf->loop();
                return;
            }
        }

        if (!gen->loop()) { stopSong(); return; }

        fadeStep();

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

            // http und https sind beide spielbar, seit dem 19.8.2026 auch
            // wirklich beide. Alles andere ist unspielbar: relative Namen
            // sind HLS-Haeppchen. Weitersuchen statt aufgeben - eine .pls
            // listet oft mehrere Server, und der naechste kann gehen.
            if (strncasecmp(v, "http://", 7) != 0
                && strncasecmp(v, "https://", 8) != 0) continue;

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
    // redir/rcap: steht dort Platz bereit und antwortet der Server mit einer
    // Umleitung, die das **Schema wechselt**, landet ihr Ziel unveraendert
    // darin. Der Aufrufer klopft dann dort noch einmal an, mit einem frischen
    // HTTPClient und damit dem passenden Client. Umleitungen ohne
    // Schemawechsel bleiben liegen - die kann ICYStream selbst.
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
            // Kurz gefasst: der String lebt nur bis zum Ende dieses
            // Blocks, nicht bis zum Funktionsende. Siehe die Erklaerung in
            // logoFetch() im Sketch.
            // 512, nicht AC_URL_MAX. Am 18.8.2026 nachgemessen, was die
            // Sender wirklich schicken: MDR 207, Deutschlandfunk 216, 89.0
            // RTL 177 Zeichen - alle mit Sitzungsmerkern (cid, sid, token,
            // tvf), deren Laenge niemand hier kontrolliert. Bei 256 lag MDR
            // 48 Zeichen vor dem Abschneiden.
            //
            // Abschneiden waere still und toedlich: strlcpy kuerzt ohne
            // Klage, und eine verstuemmelte Adresse zeigt ins Leere, waehrend
            // die Fusszeile nur "Stream unavailable" sagt. Deshalb wird zu
            // lang **erkannt und abgelehnt**, nicht gekuerzt - dann folgt
            // ICYStream der Umleitung selbst, wie ohne diese Erweiterung.
            char loc[512];
            bool locZuLang = false;
            {
                String t = h.header("Location");
                locZuLang = (t.length() >= sizeof(loc));
                strlcpy(loc, t.c_str(), sizeof(loc));
            }
            if (locZuLang && audio_info) {
                audio_info("Location zu lang, Umleitung nicht angefasst");
            }

            // **Nur beim Schemawechsel eingreifen.** Bleibt es bei http oder
            // bleibt es bei https, folgt AudioFileSourceICYStream selbst -
            // sein Client passt dann ja. Nur der Wechsel bringt ihn um: der
            // Client steht seit http.begin() fest, und ein einfacher spricht
            // kein TLS, ein TLS-Client kein plain http.
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
            const bool locHttps = !strncasecmp(loc, "https://", 8);
            const bool locHttp  = !strncasecmp(loc, "http://",  7);
            const bool urlHttps = !strncasecmp(url, "https://", 8);

            if (!locZuLang && (locHttps || locHttp) && locHttps != urlHttps
                && strlen(loc) < rcap) {
                strlcpy(redir, loc, rcap);
                h.end();
                return true;
            }
            // Alles andere: weiter wie ohne diese Erweiterung.
        }

        // Alle drei Kopfzeilen sofort in feste Puffer holen und die
        // String-Objekte gleich wieder sterben lassen. Frueher lebten sie bis
        // zum Funktionsende, also ueber resolveList() und h.end() hinweg -
        // drei kleine Bloecke mitten im Heap, waehrend darueber der
        // Stromaufbau belegt. Genau das zerschneidet die zwei 16-KB-Flaechen,
        // die mbedTLS am Stueck braucht.
        char ct[64];
        char nm[64];
        uint32_t brKbit = 0;
        {
            String t = h.header("Content-Type");
            t.toLowerCase();
            strlcpy(ct, t.c_str(), sizeof(ct));
        }
        {
            String t = h.header("icy-br");
            brKbit = (uint32_t)t.toInt();
        }
        {
            String t = h.header("icy-name");
            strlcpy(nm, t.c_str(), sizeof(nm));
        }

        isAac = strstr(ct, "aac") != nullptr;

        // Schweigt der Server oder nennt er etwas Unverfaengliches, entscheidet
        // die Adresse. Ein ausdrueckliches "mpeg" oder "mp3" wird nicht
        // ueberstimmt - das ist eine Aussage, kein Schweigen.
        if (!isAac && !strstr(ct, "mpeg") && !strstr(ct, "mp3")
            && acUrlLooksLikeAac(url)) {
            isAac = true;
            if (audio_info) audio_info("AAC an der Adresse erkannt");
        }

        // Wiedergabelisten sind kein Ton. Wer sie an den MP3-Decoder gibt,
        // bekommt Rauschen - am 12.8.2026 an zwei taiwanesischen Sendern
        // gehoert, die HLS liefern (application/vnd.apple.mpegurl).
        playlist = strstr(ct, "mpegurl")             // m3u, m3u8, HLS
                || strstr(ct, "scpls")               // pls
                || strstr(ct, "playlist");

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
            && !strstr(ct, "audio")
            && !strstr(ct, "ogg")
            && !strstr(ct, "mpeg")) {
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

        if (brKbit) icyBitrate = brKbit * 1000;
        if (nm[0] && audio_showstation) audio_showstation(nm);

        if (audio_info) {
            char m[80];
            snprintf(m, sizeof(m), "content-type: %s", ct);
            audio_info(m);
        }
        return true;
    }

    AudioVuOut          out;
    AudioGenerator*     gen  = nullptr;
    AudioFileSource*    src  = nullptr;
    AudioFileSourceBuffer* buf = nullptr;
    AudioFileSourceFS*  file = nullptr;

    CountingICYStream*  counting = nullptr;   // nur bei Netzstreams gesetzt

    // Vorpuffer, seit dem 18.8.2026 **dynamisch** statt fest.
    //
    // Als statisches Feld haette ein 44-KB-Puffer 32 KB dauerhaft aus dem
    // groessten Block genommen - der laege dann bei rund 23 KB, und der
    // PNG-Dekoder braucht 43.768 am Stueck. Die Logos waeren tot gewesen.
    //
    // Dynamisch gibt stopSong() ihn zurueck. Waehrend der Online-Liste und
    // waehrend logoZu565() ist der Platz damit frei, beim Spielen gehoert er
    // dem Ton. Nacheinander, nie gleichzeitig.
    static uint8_t* streamMem;
    static uint32_t streamMemBytes;

    uint32_t bufBytes        = sizeof(streamMem);
    bool     playlist        = false;   // letzter Versuch war eine Liste
    bool     hls             = false;   // ... und zwar eine HLS-Haeppchenliste
    uint32_t icyBitrate      = 0;      // aus dem Kopf, kann fehlen
    uint32_t measuredBitrate = 0;      // selbst gemessen, die verlaessliche
    const char* codecName = "";
    uint8_t  vol = 128;               // 0..255, Mitte - wie curVolume
    float    fadeGain  = 1.0f;   // 0 = stumm, 1 = eingestellte Lautstaerke
    uint32_t fadeStart = 0;      // 0 = keine Blende im Gang

    // SetGain bleibt auf 1,0 - es taugt nur noch als Begrenzer, siehe den
    // Block bei AC_VOL_MIN_DB. Gerechnet wird hier, in Q16.
    void applyGain() {
        out.SetGain(1.0f);
        if (!vol) { acVolQ = 0; return; }
        const float db = AC_VOL_MIN_DB * (1.0f - (float)vol / 255.0f);
        float g = powf(10.0f, db / 20.0f) * fadeGain;
        if (g < 0.0f) g = 0.0f;
        if (g > 1.0f) g = 1.0f;
        acVolQ = (int32_t)lroundf(g * 65536.0f);
    }
};

// Speicherplatz der statischen Member.
uint8_t* Audio::streamMem = nullptr;
uint32_t Audio::streamMemBytes = 0;
