// ===========================================================================
// Sprachumschaltung fuer das WebRadio.
//
// Deutsch und Englisch. Umgestellt wird im Systemmenue unter
// "Sprache" / "Language", die Wahl steht im NVS und ueberlebt den Neustart.
//
// ---------------------------------------------------------------------------
// Speicher
//
// langStrings ist doppelt const: die Zeiger sind const UND das, worauf sie
// zeigen. Damit landet die ganze Tabelle in .rodata, also im Flash, und
// kostet kein Byte RAM. Das ist hier keine Feinheit, sondern Bedingung -
// im Betrieb sind rund 13 KB Heap frei.
//
// Wer hier Texte ergaenzt, benutzt deshalb "const char* const" und niemals
// String. Ein einziges String-Objekt in dieser Tabelle zoege die komplette
// Textmenge in den RAM.
//
// PROGMEM steht bewusst nicht dabei: auf dem ESP32 ist der Flash in den
// Adressraum eingeblendet, const-Daten sind ohnehin dort und werden ganz
// normal dereferenziert. pgm_read_* braucht es nur auf AVR.
//
// ---------------------------------------------------------------------------
// Benutzung
//
//   T(STR_MENU_EXIT)   liefert den Text in der eingestellten Sprache
//
// Neuen Text anlegen: Kennung in StrId eintragen, dann in BEIDE Sprachbloecke
// eine Zeile an derselben Stelle. Die Reihenfolge muss uebereinstimmen -
// deshalb steht hinter jedem Eintrag die Kennung als Kommentar.
//
// Platz auf dem Bildschirm: Menue und Meldungen sind Font0 in doppelter
// Groesse, also 12 px je Zeichen. Ein Menueeintrag hat 180 px = 15 Zeichen,
// eine Meldungszeile 196 px = 16 Zeichen. Laengeres wird am Fensterrand
// abgeschnitten.
// ===========================================================================

#pragma once

#include <Preferences.h>

// Muss zu NVS_NAMESPACE aus CardWifiSetup.h passen. Steht hier noch einmal,
// damit diese Datei fuer sich allein uebersetzbar bleibt und vor
// CardWifiSetup.h eingebunden werden kann.
#define LANG_NVS_NS  "M5_settings"
#define LANG_NVS_KEY "lang"

// Sprache beim allerersten Start, solange nichts im NVS steht.
// Fuer die eigene Kiste hier auf LANG_DE stellen.
#define LANG_DEFAULT LANG_EN

enum Lang : uint8_t { LANG_DE, LANG_EN, LANG_COUNT };

enum StrId : uint8_t {
  // Hauptmenue
  STR_MENU_WIFI,
  STR_MENU_ONLINE,
  STR_MENU_LOCAL,
  STR_MENU_SAVE,
  STR_MENU_LANG,
  STR_MENU_TEST,
  STR_MENU_EXIT,

  // WLAN-Untermenue
  STR_WM_INFO,
  STR_WM_SCAN,
  STR_WM_SAVED,
  STR_WM_RESET,

  // in beiden Untermenues
  STR_BACK,

  // Die Masken aus CardWifiSetup.h stehen bewusst NICHT hier: sie sind fest
  // englisch. Sie laufen beim Start, bevor jemand eine Sprache waehlen kann,
  // und benutzen als einzige den Skalenfont in 1,5-facher Groesse.

  // Meldungen aus sysMessage(), zwei Zeilen zu je 16 Zeichen
  STR_MSG_SD_CLOSED,
  STR_MSG_UNTIL_BOOT,
  STR_MSG_NOT_ONLINE,
  STR_MSG_ALREADY,
  STR_MSG_SLOT,
  STR_MSG_FULL,
  STR_MSG_ALL_USED,
  STR_MSG_SAVED,
  STR_MSG_SLOT_OF,
  STR_MSG_NOT_SAVED,

  // Online-Browser und lokale Liste. Beide Vollbildschirme zeichnen in
  // FOOTER_FONT, also Font0 in einfacher Groesse: 6 px je Zeichen, damit
  // passen 38 Zeichen auf die Breite. Nicht mit der Nokia-Schrift
  // verwechseln, die ist proportional und viel breiter.
  STR_RB_ALL,
  STR_RB_TITLE_C,
  STR_RB_TITLE_S,
  STR_RB_EMPTY,
  STR_RB_E_WIFI,
  STR_RB_E_CONN,
  STR_RB_E_TIME,
  STR_RB_E_STATUS,
  STR_RB_E_MEM,
  STR_RB_E_NONE,
  STR_RB_HTTP,
  STR_RB_STATS,
  STR_RB_FOOT_PLAY,
  STR_RB_FOOT_NEXT,
  STR_RB_FOOT_BACK,
  STR_RB_SEARCH,

  STR_LOC_TITLE,
  STR_LOC_EMPTY,
  STR_LOC_FOOT,

  // Radio-Oberflaeche. Hier gilt UI_FONT, also die Nokia-Schrift.
  STR_IDLE_HDR,

  STR_COUNT
};

static const char* const langStrings[LANG_COUNT][STR_COUNT] = {

  // ----- LANG_DE ----------------------------------------------------------
  {
    "WiFi",              // STR_MENU_WIFI
    "Sender online",     // STR_MENU_ONLINE
    "Lokale Liste",      // STR_MENU_LOCAL
    "Sender sichern",    // STR_MENU_SAVE
    "Sprache",           // STR_MENU_LANG
    "Testlauf",          // STR_MENU_TEST
    "Exit",              // STR_MENU_EXIT

    "WiFi-Info",         // STR_WM_INFO
    "Scan+Connect",      // STR_WM_SCAN
    "Gespeicherte",      // STR_WM_SAVED
    "Reset",             // STR_WM_RESET

    "Back",              // STR_BACK

    "SD nicht offen",    // STR_MSG_SD_CLOSED
    "nur bis Neustart",  // STR_MSG_UNTIL_BOOT
    "Nicht online",      // STR_MSG_NOT_ONLINE
    "Schon in Liste",    // STR_MSG_ALREADY
    "Platz %d",          // STR_MSG_SLOT
    "Speicher voll",     // STR_MSG_FULL
    "alle %d belegt",    // STR_MSG_ALL_USED
    "Gesichert",         // STR_MSG_SAVED
    "Platz %d von %d",   // STR_MSG_SLOT_OF
    "nicht gesichert",   // STR_MSG_NOT_SAVED

    "Alle",                          // STR_RB_ALL
    "Sender online - Land",          // STR_RB_TITLE_C
    "%s ab %d",                      // STR_RB_TITLE_S
    "Nichts erhalten",               // STR_RB_EMPTY
    "kein WLAN",                     // STR_RB_E_WIFI
    "Server nicht erreichbar",       // STR_RB_E_CONN
    "Zeitueberschreitung",           // STR_RB_E_TIME
    "Antwort ohne Statuszeile",      // STR_RB_E_STATUS
    "Speicher reicht nicht",         // STR_RB_E_MEM
    "Hier gibt es nichts",           // STR_RB_E_NONE
    "HTTP-Status %d",                // STR_RB_HTTP
    "%d Sender  %lu ms  Heap min %lu", // STR_RB_STATS
    "Ok=Play  <  > Seite  ESC",      // STR_RB_FOOT_PLAY
    "Ok=weiter  <  > Seite",         // STR_RB_FOOT_NEXT
    "ESC zurueck, BtnG0=Radio",      // STR_RB_FOOT_BACK
    "tippen zum Suchen",             // STR_RB_SEARCH

    "Lokale Liste  %d/%d",           // STR_LOC_TITLE
    "Liste ist leer",                // STR_LOC_EMPTY
    "ESC=Menue, BtnG0=Radio",        // STR_LOC_FOOT

    "MP3 von SD",                    // STR_IDLE_HDR
  },

  // ----- LANG_EN ----------------------------------------------------------
  {
    "WiFi",              // STR_MENU_WIFI
    "Online stations",   // STR_MENU_ONLINE
    "Local list",        // STR_MENU_LOCAL
    "Save station",      // STR_MENU_SAVE
    "Language",          // STR_MENU_LANG
    "Test run",          // STR_MENU_TEST
    "Exit",              // STR_MENU_EXIT

    "WiFi info",         // STR_WM_INFO
    "Scan+Connect",      // STR_WM_SCAN
    "Saved WiFi",        // STR_WM_SAVED
    "Reset",             // STR_WM_RESET

    "Back",              // STR_BACK

    "SD not open",       // STR_MSG_SD_CLOSED
    "until restart",     // STR_MSG_UNTIL_BOOT
    "Not online",        // STR_MSG_NOT_ONLINE
    "Already in list",   // STR_MSG_ALREADY
    "Slot %d",           // STR_MSG_SLOT
    "List full",         // STR_MSG_FULL
    "all %d used",       // STR_MSG_ALL_USED
    "Saved",             // STR_MSG_SAVED
    "Slot %d of %d",     // STR_MSG_SLOT_OF
    "not saved",         // STR_MSG_NOT_SAVED

    "All",                           // STR_RB_ALL
    "Online - Country",              // STR_RB_TITLE_C
    "%s from %d",                    // STR_RB_TITLE_S
    "Nothing received",              // STR_RB_EMPTY
    "no WiFi",                       // STR_RB_E_WIFI
    "Server unreachable",            // STR_RB_E_CONN
    "Timeout",                       // STR_RB_E_TIME
    "No status line",                // STR_RB_E_STATUS
    "Out of memory",                 // STR_RB_E_MEM
    "Nothing here",                  // STR_RB_E_NONE
    "HTTP status %d",                // STR_RB_HTTP
    "%d stations  %lu ms  heap min %lu", // STR_RB_STATS
    "Ok=Play  <  > Page  ESC",       // STR_RB_FOOT_PLAY
    "Ok=next  <  > Page",            // STR_RB_FOOT_NEXT
    "ESC back, BtnG0=Radio",         // STR_RB_FOOT_BACK
    "type to search",                // STR_RB_SEARCH

    "Local list  %d/%d",             // STR_LOC_TITLE
    "List is empty",                 // STR_LOC_EMPTY
    "ESC=Menu, BtnG0=Radio",         // STR_LOC_FOOT

    "Play MP3 from SD",              // STR_IDLE_HDR
  },
};

// Namen der Sprachen. Stehen bewusst NICHT in der Tabelle oben: eine Sprache
// nennt sich in jeder Oberflaeche gleich, sonst sucht der Englischsprachige
// vergeblich nach "German".
static const char* const langNames[LANG_COUNT] = { "Deutsch", "English" };

static uint8_t uiLang = LANG_DEFAULT;

static inline const char* T(StrId id) {
  return langStrings[uiLang][id];
}

// Beim Start einmal aufrufen. Ein unbekannter Wert im NVS - etwa aus einer
// spaeteren Fassung mit mehr Sprachen - faellt auf die Voreinstellung zurueck.
static void langLoad() {
  Preferences p;
  p.begin(LANG_NVS_NS, true);
  uint8_t v = p.getUChar(LANG_NVS_KEY, LANG_DEFAULT);
  p.end();

  uiLang = (v < LANG_COUNT) ? v : (uint8_t)LANG_DEFAULT;
}

static void langSave() {
  Preferences p;
  p.begin(LANG_NVS_NS, false);
  p.putUChar(LANG_NVS_KEY, uiLang);
  p.end();
}
