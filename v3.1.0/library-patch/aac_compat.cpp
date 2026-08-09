// ===========================================================================
// Bruecke zwischen dem Helix-AAC-Decoder (aus ESP32-audioI2S 3.0.0) und dem
// Audio.cpp der Fassung 3.0.13.
//
// Warum der Decoder getauscht wurde: ab 3.0.x hat die Bibliothek von Helix
// auf faad2 umgestellt. faad2 fordert in channel_pair_element() je
// Stereo-Rahmen 22.006 Bytes am Stueck an (sizeof(element)) und prueft das
// Ergebnis nicht auf NULL - von 71 Anforderungen in neaacdec.cpp tun das nur
// zwei. Ohne PSRAM hat der Cardputer Adv diesen Block nicht dauerhaft frei,
// gemessen am 9.8.2026: groesster Block im Betrieb 12 bis 24 KB. Ergebnis
// war ein Neustart beim ersten Rahmen jedes AAC-Senders, unabhaengig von der
// Bitrate. Helix kommt mit fest angelegten Puffern aus.
//
// Audio.cpp 3.0.13 ruft drei Funktionen auf, die es in der Helix-Fassung
// nicht gibt. Alle drei dienen dort ausschliesslich der Protokollausgabe -
// nachgesehen an den Aufrufstellen: AACGetSBR nur fuer eine AUDIO_INFO-Zeile,
// AACGetParametricStereo setzt eine Variable, die sonst nirgends gelesen
// wird. Sie werden hier nachgereicht, damit nichts fehlt.
//
// Beim Aktualisieren der Bibliothek geht dieser Ordner verloren. Die
// Sicherung liegt unter M5Cardputer_WebRadio_backups/library-aac-<datum>/.
// ===========================================================================

#include "aac_decoder.h"

// ---------------------------------------------------------------------------
// Zwei Aufrufe haben in beiden Fassungen dieselben Namen, aber andere
// Parameter. Statt den Helix-Quelltext zu aendern, stehen hier Ueberladungen
// daneben - der Uebersetzer sucht sich die passende heraus, und die
// Originaldatei bleibt unberuehrt und damit leicht austauschbar.
//
// Das setzt voraus, dass int32_t und int verschiedene Typen sind. Auf dem
// ESP32 ist int32_t ein long int, die Ueberladung ist also zulaessig. Auf
// einer Maschine, wo beide dasselbe sind, waere es eine Doppeldefinition.
// ---------------------------------------------------------------------------

// faad2 kannte kein copyLast. Audio.cpp ruft entsprechend mit drei Werten auf.
int AACSetRawBlockParams(int nChans, int sampRateCore, int profile) {
    return AACSetRawBlockParams(0, nChans, sampRateCore, profile);
}

// Audio.cpp reicht ein int32_t* herein, Helix erwartet ein int*.
int AACDecode(uint8_t* inbuf, int32_t* bytesLeft, short* outbuf) {
    int n = (int)*bytesLeft;
    int ret = AACDecode(inbuf, &n, outbuf);
    *bytesLeft = n;
    return ret;
}

// Spectral Band Replication: Helix meldet die Ausgabe-Abtastrate bereits
// verdoppelt, eine eigene Auskunft daruber gibt es nicht. 0 heisst fuer
// Audio.cpp "keine Angabe", die Zeile wird dann nicht ausgegeben.
uint8_t AACGetSBR() {
    return 0;
}

// Parametric Stereo, die zweite Stufe von AAC+. Wird in Audio.cpp nur fuer
// eine einmalige Meldung ausgewertet.
uint8_t AACGetParametricStereo() {
    return 0;
}

// Klartext zu den Fehlernummern des Helix-Decoders. Die Nummern stehen als
// ERR_AAC_* in aac_decoder.h; Audio.cpp uebergibt den Betrag.
const char* AACGetErrorMessage(int8_t err) {
    switch (err) {
        case 0:  return "NONE";
        case 1:  return "indata underflow";
        case 2:  return "null pointer";
        case 3:  return "invalid ADTS header";
        case 4:  return "invalid ADIF header";
        case 5:  return "invalid frame";
        case 6:  return "MPEG4 unsupported";
        case 7:  return "channel map error";
        case 8:  return "syntax element error";
        case 9:  return "dequant error";
        case 10: return "stereo process error";
        case 11: return "prediction error";
        case 12: return "TNS error";
        case 13: return "filterbank error";
        case 14: return "SBR init error";
        case 15: return "SBR frame error";
        case 16: return "SBR LR error";
        case 17: return "SBR insufficient dynamic range";
        case 18: return "SBR PCM format error";
        case 19: return "SBR PS format error";
        case 20: return "raw block params error";
        default: return "unknown error";
    }
}
