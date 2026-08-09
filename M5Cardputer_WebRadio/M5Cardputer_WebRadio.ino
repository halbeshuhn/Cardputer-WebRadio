// ===========================================================================
// WebRadio fuer M5Stack Cardputer Adv
//
// Internetradio mit Senderliste von SD-Karte, WLAN-Speicher, Streamueber-
// wachung und zwei Anzeigen (VU-Meter und Spektrumanalyzer).
//
// Herkunft: cyberwisk/M5Cardputer_WebRadio -> WuSiU -> eigene Erweiterungen.
//
// Uebersetzen: Board "M5Cardputer", Partitionsschema
//              "Huge APP (3MB No OTA/1MB SPIFFS)" - zwingend, der Sketch
//              belegt rund 1,66 MB.
//
// ---------------------------------------------------------------------------
// Bildschirmaufteilung (240 x 135, gedreht)
//
//   y   0..12   Mini-Skala der Lautstaerke links (75 % der Breite),
//                Batterieanzeige rechts
//   y  17..32   Sendername, grosse Schrift
//   y  37..44   Streamtitel in halber Schrift, laeuft einmal durch
//   y  50       rote Trennlinie
//   y  51..118  freie Flaeche: VU-Meter oder Spektrumanalyzer
//   y 119..134  Footer, Schrift halbhoch und unten haengend (y 123..130):
//                links @halbeshuhn, Mitte WLAN-Feldstaerke mit Symbol, rechts
//                Bitrate. Bei totem Stream stattdessen eine Meldung.
//
// Seitenrand ueberall UI_MARGIN. Ausgenommen: die rote Trennlinie und der
// Streamtitel, solange er durchlaeuft.
//
// ---------------------------------------------------------------------------
// Tastatur
//
//   Links / Rechts   Sender wechseln      (Zeichen ',' und '/')
//   Hoch / Runter    Lautstaerke          (Zeichen ';' und '.')
//   M                stumm
//   R                Stream neu verbinden
//   F                Anzeige durchschalten: aus - VU - VU+Peak - Spektrum
//   B                Helligkeit
//   L                Senderliste, ENTER waehlt aus
//   BtnG0            Systemmenue als schwebendes Fenster:
//                    WiFi / Sender online / Lokale Liste / Exit, dazu
//                    "Sender sichern", solange ein Sender aus der
//                    Online-Liste laeuft. WLAN-Untermenue: WiFi-Info /
//                    Scan+Connect / Gespeicherte / Reset / Back.
//                    Hoch/Runter waehlt, ENTER ruft auf, ESC (Zeichen '`')
//                    geht eine Ebene zurueck. In den Listen gilt zusaetzlich
//                    BS = Eintrag loeschen.
//                    BtnG0 fuehrt aus jeder Ebene sofort zum Radio zurueck -
//                    ausser waehrend einer laufenden WLAN-Aktion, die haelt
//                    die Schleife an und hoert nur auf ihre eigenen Tasten.
//
// Die Pfeiltasten des Cardputers liefern keine eigenen Codes, sondern die
// aufgedruckten Zeichen - daher die auf den ersten Blick seltsamen Abfragen
// auf ';' '.' ',' und '/'.
// ===========================================================================

#include "M5Cardputer.h"
#include "NokiaFC.h"         // Nokia-Pixelschrift, aus nokiafc22.ttf erzeugt
#include "Lang.h"            // Texttabelle deutsch/englisch, T() und uiLang
#include "CardWifiSetup.h"   // WLAN-Einrichtung und -Speicher, eigene Datei
#include <Audio.h>           // ESP32-audioI2S: Streamdecoder
#include <SD.h>

// Senderliste: Obergrenzen. Laengere Namen oder URLs werden abgeschnitten.
// Die Masse richten sich nach dem, was aus dem Netz hereinkommt: Namen bis
// 38 Zeichen, URLs bis 213 gemessen. Mit den frueheren 30 / 100 waeren aus
// der Online-Liste uebernommene Sender abgeschnitten und unspielbar.
#define MAX_STATIONS 20
#define MAX_NAME_LENGTH 40
#define MAX_URL_LENGTH 256

// ---------------------------------------------------------------------------
// Fehlersuche ueber die serielle Schnittstelle
//
// Auf 0 setzen, dann faellt alles davon weg - ohne DEBUG_SERIAL sendet das
// Geraet nichts, auch nicht die Meldungen der Audio-Bibliothek.
//
// Ausgegeben werden drei Zahlen, nicht eine:
//   frei  - freier Heap gerade jetzt
//   min   - Tiefstand seit dem Einschalten
//   groesster Block - groesste zusammenhaengende Anforderung, die noch geht
//
// Die dritte ist die wichtigste. Der Heap zerfaellt im Betrieb; faad2 und
// die Decoder fordern grosse Stuecke am Stueck an. "Genug frei" und trotzdem
// ein Fehlschlag ist genau dieser Fall.
//
// audio_info ist ein schwaches Symbol der Bibliothek und bleibt NULL, wenn
// es niemand definiert - dann verschwinden ihre Meldungen spurlos. Hier
// definiert reicht es sie an Serial durch, samt der Zeile, die den freien
// Heap direkt nach dem Anlegen eines Decoders nennt.
//
// Achtung: Der Serial Monitor der IDE belegt den Anschluss. Vor dem Flashen
// schliessen, sonst scheitert der Upload.
// ---------------------------------------------------------------------------
#define DEBUG_SERIAL   0      // fuer die Fehlersuche auf 1 setzen
#define DEBUG_HEAP_MS  1500   // Abstand der Heap-Zeilen

// I2S-Anschluss des Verstaerkers auf dem Cardputer Adv.
#define I2S_BCK 41
#define I2S_WS 43
#define I2S_DOUT 42

#define VOLUME_STEP 10       // Schrittweite je Tastendruck, Skala 0..255
#define FOOTER_HEIGHT 16

// ---------------------------------------------------------------------------
// Footer: drei Felder in halber Schrift, links @halbeshuhn, in der Mitte die
// WLAN-Feldstaerke, rechts die Bitrate. Die Hoehe des Streifens bleibt bei
// FOOTER_HEIGHT, die Schrift haengt aber unten - FOOTER_TEXT_DY Pixel ueber
// der Unterkante des Bildes. Der Platz darueber gehoert der Mitte.
//
// Jedes Feld raeumt beim Zeichnen nur seinen eigenen x-Bereich frei, sonst
// wuerden sich die drei gegenseitig loeschen: sie werden unabhaengig
// voneinander und verschieden oft aufgefrischt.
// ---------------------------------------------------------------------------
#define FOOTER_TEXT_DY   4   // Luft zwischen Schrift und Unterkante des Bildes
#define FOOTER_WIFI_X   90   // linke Kante des WLAN-Feldes
#define FOOTER_RATE_X  152   // linke Kante des Bitratenfeldes

// Schrift des Footers: dieselbe wie die dB-Skala des VU-Meters. Font0 ist
// ein fester 6x8-Zeichensatz, die Luft zwischen den Buchstaben steckt schon
// in der Zeichenbreite - deshalb liest er sich klein besser als die halbe
// Nokia-Schrift. 8 px hoch, die Footerzeile bleibt damit wo sie war.
#define FOOTER_FONT (&fonts::Font0)

// WLAN-Anzeige: Symbol, dann der Prozentwert. Beides zusammen mittig im Feld.
#define WIFI_ICON_W     11
#define WIFI_ICON_H      8
#define WIFI_GAP         3   // Luft zwischen Symbol und Prozentwert
#define WIFI_UPDATE_MS 2000  // wie oft die Feldstaerke geholt wird
#define WIFI_PCT_GREEN  67   // ab hier leuchtet das Symbol gruen
#define WIFI_PCT_YELLOW 34   // ab hier gelb, darunter rot

// Seitenrand der gesamten Oberflaeche. Links faengt alles bei UI_MARGIN an,
// rechts endet alles bei 239 - UI_MARGIN. Gilt fuer Textzeilen, Lautstaerke-
// skala, Batterie, VU-Meter, Analyzer und Footer.
// Zwei Ausnahmen, absichtlich ueber die volle Breite: die rote Trennlinie
// und der Streamtitel, solange er durchlaeuft - der soll am Rand
// hinauslaufen und nicht an einer unsichtbaren Kante abgeschnitten werden.
#define UI_MARGIN 4

// Schriftfarbe im Footer (@halbeshuhn und Bitrate): mittleres Grau, halb so hell
// wie Weiss. Zum Nachjustieren nur diese drei Werte aendern, 0x00 = schwarz,
// 0xFF = weiss. color888() ist noetig, weil eine nackte Zahl von der
// Bibliothek als RGB565 gelesen wuerde und als andere Farbe herauskaeme.
#define FOOTER_TEXT_COLOR (lgfx::color888(0x80, 0x80, 0x80))

// Linkes Footer-Feld. Wer den Sketch nachbaut, traegt hier seinen eigenen
// Namen ein - Font0, es bleiben rund 14 Zeichen bis zum WLAN-Feld.
#define FOOTER_NAME "@halbeshuhn"

// Hauptschrift der Oberflaeche. Nur hier aendern - alle Stellen, die die
// Schrift setzen oder wiederherstellen, benutzen dieses Makro.
// Ausgenommen ist nur die dB-Skala des VU-Meters, die bleibt auf Font0.
#define UI_FONT (&NokiaFC)

// Halbe Groesse derselben Schrift, 8 px hoch. Fuer den Streamtitel und die
// Kanalbeschriftung des VU-Meters. Wer sie benutzt, stellt hinterher wieder
// auf UI_FONT zurueck.
#define UI_FONT_SMALL (&NokiaFCSmall)

// Zeilenhoehe der Oberflaeche, fest verdrahtet statt aus fontHeight().
// Die Menuezeilen sind auf 18 px gebaut; waeren sie an die Schrift
// gekoppelt, wuerde jeder Schriftwechsel das Layout verschieben.
// Die Nokia-Schrift ist 16 px hoch und sitzt darin.
#define UI_LINE_H 18

// ---------------------------------------------------------------------------
// Kopfbereich, y-Koordinaten an einer Stelle. Siehe Bildschirmaufteilung oben.
// ---------------------------------------------------------------------------
#define HDR_BATT_H     13   // Hoehe des Feldes, das die Batterie freiraeumt
#define HDR_TEXT_X     UI_MARGIN  // linker Anschlag beider Textzeilen
#define HDR_NAME_Y     17   // Oberkante Sendername, grosse Schrift
#define HDR_NAME_H     16
#define HDR_TITLE_Y    37   // Oberkante Streamtitel, halbe Schrift
#define HDR_TITLE_H     9
#define HDR_RULE_Y     50   // rote Trennlinie

// ---------------------------------------------------------------------------
// Batteriesymbol oben rechts, in der Form des iPhone-Symbols: ein liegendes
// Gehaeuse mit runden Ecken, der Pluspol als kurzer Balken rechts daneben,
// und innen die Fuellung mit etwas Luft zum Rahmen. Gezeichnet wird
// ausschliesslich mit Rechtecken - vier Linien und vier Eckpunkte fuer den
// Rahmen, zwei Flaechen fuer Pluspol und Fuellung.
//
// Gesamtbreite = BATT_W + BATT_GAP + BATT_TIP_W. Das Symbol steht buendig
// am rechten Rand, also UI_MARGIN vom Bildrand entfernt - derselbe Abstand,
// mit dem die Lautstaerkeskala links beginnt.
//
// Hoehe nach Augenmass eingestellt, in drei Schritten: erst so hoch wie die
// Skala (Zeilen 3..11), das wirkte zu klein; dann so, dass die Fuellung genau
// das Band der Skala fuellt (Rahmen 1..13), das wirkte zu gross. Geblieben
// ist die Mitte: der Rahmen steht oben und unten einen Pixel ueber die Skala
// hinaus, Zeilen 2..12, die Fuellung liegt in 4..10.
//
// HDR_BATT_H muss BATT_Y + BATT_H abdecken, sonst bleiben Reste stehen.
//
// Die Farben kommen von der Lautstaerkeskala (UI_SCALE_ON / UI_SCALE_GRID):
// Rahmen und Pluspol im gedaempften Ton, die Fuellung im hellen.
// ---------------------------------------------------------------------------
#define BATT_Y          2   // Oberkante des Gehaeuses
#define BATT_W         22   // Breite des Gehaeuses
#define BATT_H         11   // Hoehe des Gehaeuses
#define BATT_R          2   // Rundung der Ecken; 1 = nur abgeschraegt
#define BATT_PAD        2   // Luft zwischen Rahmen und Fuellung
#define BATT_GAP        1   // Luft zwischen Gehaeuse und Pluspol
#define BATT_TIP_W      2   // Pluspol
#define BATT_TIP_H      5

// ---------------------------------------------------------------------------
// Systemmenue: schwebendes Fenster mitten auf der Anzeige, geoeffnet mit
// BtnG0 (dem Knopf an der linken Gehaeuseseite). Solange es offen ist, gehen
// alle Tasten an das Menue, gezeichnet wird sonst nichts - die Wiedergabe
// laeuft aber weiter, audio.loop() steht vor der Abfrage.
//
// Zwei Ebenen: Hauptmenue und WLAN-Untermenue. Steuerung:
//   Hoch / Runter   Marken bewegen (Zeichen ';' und '.')
//   ENTER           Eintrag aufrufen
//   ESC ('`') / BS  eine Ebene zurueck, im Hauptmenue schliessen
//   BtnG0           immer sofort zurueck zum Radio, von jeder Ebene aus
//
// Die Fensterhoehe waechst mit der Zahl der Eintraege der gezeigten Ebene.
//
// Schrift ist Font0 in doppelter Groesse, also 12 x 16 px je Zeichen. Sie
// wird nur fuers Menue gesetzt und danach zurueckgestellt - setTextSize()
// gilt global und wuerde sonst die ganze Oberflaeche aufblasen.
// ---------------------------------------------------------------------------
#define MENU_FONT       (&fonts::Font0)
#define MENU_TEXT_SIZE   2

#define SYS_MENU_W      208   // "Sender sichern" braucht 168, Meldungen 16 Zeichen
#define SYS_MENU_PAD      6   // Innenabstand zum Rahmen
#define SYS_MENU_LINE_H  20   // Zeilenhoehe eines Eintrags
#define SYS_MENU_MARK_W  16   // Spalte fuer den Marker ">"

// ---------------------------------------------------------------------------
// Senderliste aus dem Netz (radio-browser.info), erster Versuch.
//
// Feste Abfrage: Deutschland, die 15 meistgeklickten, tote aussortiert. Der
// Dienst antwortet auf reinem HTTP - kein TLS noetig, das spart die 40 bis
// 50 KB, die der Stack sonst neben den Audiopuffern braucht.
//
// Die Antwort ist zu gross, um sie am Stueck zu halten (30 Sender sind rund
// 38 KB, eine serverseitige Feldauswahl gibt es nicht). Deshalb wird der
// Datenstrom im Vorbeilaufen nach "name":"..." abgesucht und nur das
// Gefundene behalten. Zwischendurch laeuft audio.loop() weiter, damit die
// Wiedergabe nicht abreisst.
//
// Der Abruf misst nebenbei Dauer, Datenmenge und freien Speicher - genau
// darum geht es in diesem Schritt.
// ---------------------------------------------------------------------------
// Zwei Namen, weil der Dienst zeitweise ausfaellt. radio-browser antwortet
// dann mit HTTP 503 und dem Rumpf "no available server" - das ist die
// Meldung ihres eigenen Vorschaltservers, wenn er gerade kein gesundes
// Hintersystem hat. Am 8.8.2026 auf dem Geraet gesehen, wenige Minuten
// spaeter war derselbe Name wieder sauber (12 von 12 Abrufen).
//
// RB_HOST2 ist bewusst der Rundlaufname: er loest auf mehrere Server auf,
// der zweite Versuch landet also mit einiger Wahrscheinlichkeit woanders
// als der erste. Ein zweiter fester Spiegel waere dagegen nur ein weiterer
// Name auf denselben kranken Vorschaltserver.
#define RB_HOST  "de1.api.radio-browser.info"
#define RB_HOST2 "all.api.radio-browser.info"
#define RB_PORT 80
#define RB_RETRY_PAUSE_MS 400
// Der Pfad wird gebaut, nicht festverdrahtet: Land, optional Region, Seite.
//
// Drei Filter stehen fest drin:
//
// stateExact=true - ohne das ist der state-Filter eine Teilstringsuche und
// "Sachsen" zieht Niedersachsen und Sachsen-Anhalt mit herein (gemessen:
// 286 statt 128 Sender). Steht weiter unten, wo die Region angehaengt wird.
//
// is_https=false - https-Streams sind auf diesem Geraet nicht abspielbar.
// Die Audio-Bibliothek braucht dafuer WiFiClientSecure, und mbedTLS legt
// mangels CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN beide Datensatzpuffer mit
// je 16 KB an - rund 32 KB, wo nur 13 bis 23 KB frei sind. Der Dienst
// filtert das selbst; ueber 200 Sender gegengeprueft, kein einziger hatte
// danach noch ein url_resolved mit https. Selbst aussortieren waere
// schlechter: dann stimmte die Seitenrechnung ueber offset nicht mehr.
//
// codec=MP3 - AAC stuerzt das Geraet ab, reproduzierbar. Die Bibliothek
// nimmt dafuer faad2, und dessen Allokator faellt ohne PSRAM auf malloc()
// zurueck. In neaacdec.cpp stehen 71 Aufrufe von faad_malloc, davon pruefen
// 2 auf NULL - bei 13 KB freiem Heap liefert die erste knappe Anforderung
// NULL, das Ergebnis wird ungeprueft benutzt, und das Geraet startet neu.
// Fuer FLAC hat die Bibliothek dagegen einen ausdruecklichen PSRAM-Riegel;
// bei AAC fehlt er.
//
// Der Anteil ist erheblich: nach dem https-Filter sind in Australien 42 %
// der Sender AAC, in Polen 33 %, in Deutschland 13 %. Was der Filter kostet
// (gemessen am 8.8.2026): AU 1443->814, DE 2674->2306, PL 619->405,
// IN 184->157.
//
// Achtung, das schuetzt nur den Online-Browser. Ein AAC-Sender, der von Hand
// in /station_list.txt steht, stuerzt weiterhin ab.
#define RB_PATH_BASE "/json/stations/search?hidebroken=true&is_https=false" \
                     "&codec=MP3&order=clickcount&reverse=true"

#define RB_AGENT      "M5CardputerWebRadio/1.0"  // verlangt der Dienst
#define RB_TIMEOUT_MS 8000

// Eigene Kennungen fuer die Fehleranzeige. Werte > 0 sind der HTTP-Status
// des Servers, alles andere ist hier drin entstanden.
#define RB_ERR_NO_WIFI   -1
#define RB_ERR_CONNECT   -2
#define RB_ERR_TIMEOUT   -3
#define RB_ERR_NO_STATUS -4
#define RB_ERR_NO_TABLE  -5   // Speicher fuer die Tabellen reichte nicht
#define RB_NAME_LEN   38   // 38 Zeichen a 6 px passen mit Rand auf die Breite
// Deutlich mehr als die 100 der SD-Liste: gemessen an der echten Antwort
// war die laengste URL 213 Zeichen lang, zwei von 15 waeren bei 100 Bytes
// abgeschnitten und damit unspielbar gewesen.
#define RB_URL_LEN    256
#define RB_VISIBLE    11   // gleichzeitig sichtbare Zeilen
#define RB_TOP_Y      12   // Oberkante der ersten Zeile
#define RB_LINE_H      9

// Eine Seite ist genau ein Bildschirm. Damit steht auf der Senderebene nie
// etwas ausserhalb des Sichtbaren - geblaettert wird mit links/rechts,
// gescrollt gar nicht. Die beiden Werte muessen gleich bleiben.
#define RB_MAX_NAMES  RB_VISIBLE

// ---------------------------------------------------------------------------
// Laender und Regionen, fest hinterlegt.
//
// Die Regionsliste des Dienstes (/json/states) ist unbrauchbar: 234 Eintraege
// allein fuer Deutschland, darunter "Baden Wuerttemberg", "Berlim",
// "Belize City" und "Augsburg, MP3 192 kbits" - von Hand eingetragener
// Freitext. Deshalb hier feste Tabellen; sie liegen im Flash, nicht im RAM.
//
// label ist, was auf dem Bildschirm steht (nur ASCII, die Schriften koennen
// nicht mehr). query ist, was an den Dienst geht - dort zaehlt bei
// stateExact=true jedes Zeichen, und die Umlautschreibweise ist die
// gebraeuchliche: "Baden-Wuerttemberg" hat 247 Sender, "Baden-Wuerttemberg"
// in ASCII nur 3.
// ---------------------------------------------------------------------------
struct RbRegion {
  const char* label;
  const char* query;
};

struct RbCountry {
  const char* name;
  const char* code;
  const RbRegion* regions;
  int regionCount;
};

static const RbRegion deRegions[] = {
  { "Baden-Wuerttemberg",  "Baden-W\xC3\xBCrttemberg" },
  { "Bayern",              "Bayern" },
  { "Berlin",              "Berlin" },
  { "Brandenburg",         "Brandenburg" },
  { "Bremen",              "Bremen" },
  { "Hamburg",             "Hamburg" },
  { "Hessen",              "Hessen" },
  { "Mecklenb.-Vorpommern","Mecklenburg-Vorpommern" },
  { "Niedersachsen",       "Niedersachsen" },
  { "Nordrhein-Westfalen", "Nordrhein-Westfalen" },
  { "Rheinland-Pfalz",     "Rheinland-Pfalz" },
  { "Saarland",            "Saarland" },
  { "Sachsen",             "Sachsen" },
  { "Sachsen-Anhalt",      "Sachsen-Anhalt" },
  { "Schleswig-Holstein",  "Schleswig-Holstein" },
  { "Thueringen",          "Th\xC3\xBCringen" },
};

static const RbRegion usRegions[] = {
  { "Alabama", "Alabama" },               { "Alaska", "Alaska" },
  { "Arizona", "Arizona" },               { "Arkansas", "Arkansas" },
  { "California", "California" },         { "Colorado", "Colorado" },
  { "Connecticut", "Connecticut" },       { "Delaware", "Delaware" },
  { "Florida", "Florida" },               { "Georgia", "Georgia" },
  { "Hawaii", "Hawaii" },                 { "Idaho", "Idaho" },
  { "Illinois", "Illinois" },             { "Indiana", "Indiana" },
  { "Iowa", "Iowa" },                     { "Kansas", "Kansas" },
  { "Kentucky", "Kentucky" },             { "Louisiana", "Louisiana" },
  { "Maine", "Maine" },                   { "Maryland", "Maryland" },
  { "Massachusetts", "Massachusetts" },   { "Michigan", "Michigan" },
  { "Minnesota", "Minnesota" },           { "Mississippi", "Mississippi" },
  { "Missouri", "Missouri" },             { "Montana", "Montana" },
  { "Nebraska", "Nebraska" },             { "Nevada", "Nevada" },
  { "New Hampshire", "New Hampshire" },   { "New Jersey", "New Jersey" },
  { "New Mexico", "New Mexico" },         { "New York", "New York" },
  { "North Carolina", "North Carolina" }, { "North Dakota", "North Dakota" },
  { "Ohio", "Ohio" },                     { "Oklahoma", "Oklahoma" },
  { "Oregon", "Oregon" },                 { "Pennsylvania", "Pennsylvania" },
  { "Rhode Island", "Rhode Island" },     { "South Carolina", "South Carolina" },
  { "South Dakota", "South Dakota" },     { "Tennessee", "Tennessee" },
  { "Texas", "Texas" },                   { "Utah", "Utah" },
  { "Vermont", "Vermont" },               { "Virginia", "Virginia" },
  { "Washington", "Washington" },         { "West Virginia", "West Virginia" },
  { "Wisconsin", "Wisconsin" },           { "Wyoming", "Wyoming" },
};

// Die 42 Laender mit den meisten Sendern bei radio-browser.info, alphabetisch.
// Ausgewaehlt nach den Bestandszahlen des Dienstes vom 8.8.2026, nicht nach
// Einwohnerzahl. Wer ein Land vermisst, traegt seinen Sender von Hand in
// /station_list.txt ein - dafuer ist die Datei da.
//
// Die Namen stehen bewusst **englisch** und nicht in Lang.h: ein Land heisst
// in jeder Oberflaeche gleich, und die Schriften koennen ohnehin nur ASCII
// (0x20..0x7E), also "Turkey" statt "Tuerkiye".
//
// name ist die Anzeige, code geht als countrycode an den Dienst (ISO 3166-1
// alpha-2). Nur Deutschland und die USA haben Regionstabellen; bei allen
// anderen steht NULL, dann ueberspringt rbEnter() die Regionsebene.
//
// Achtung: die Bestandszahlen oben gelten fuer alle Sender. Das Geraet
// filtert https weg (siehe rbFetch), danach bleiben je nach Land 15 bis 60
// Prozent uebrig - gemessen: DE 6122->2674, US 7696->2976, IN 1163->184.
static const RbCountry rbCountries[] = {
  { "Argentina",            "AR", NULL, 0 },
  { "Australia",            "AU", NULL, 0 },
  { "Austria",              "AT", NULL, 0 },
  { "Belgium",              "BE", NULL, 0 },
  { "Brazil",               "BR", NULL, 0 },
  { "Bulgaria",             "BG", NULL, 0 },
  { "Canada",               "CA", NULL, 0 },
  { "Chile",                "CL", NULL, 0 },
  { "China",                "CN", NULL, 0 },
  { "Colombia",             "CO", NULL, 0 },
  { "Croatia",              "HR", NULL, 0 },
  { "Czechia",              "CZ", NULL, 0 },
  { "Denmark",              "DK", NULL, 0 },
  { "Finland",              "FI", NULL, 0 },
  { "France",               "FR", NULL, 0 },
  { "Germany",              "DE", deRegions,
    (int)(sizeof(deRegions) / sizeof(deRegions[0])) },
  { "Greece",               "GR", NULL, 0 },
  { "Hungary",              "HU", NULL, 0 },
  { "India",                "IN", NULL, 0 },
  { "Indonesia",            "ID", NULL, 0 },
  { "Ireland",              "IE", NULL, 0 },
  { "Italy",                "IT", NULL, 0 },
  { "Japan",                "JP", NULL, 0 },
  { "Mexico",               "MX", NULL, 0 },
  { "Netherlands",          "NL", NULL, 0 },
  { "New Zealand",          "NZ", NULL, 0 },
  { "Peru",                 "PE", NULL, 0 },
  { "Philippines",          "PH", NULL, 0 },
  { "Poland",               "PL", NULL, 0 },
  { "Portugal",             "PT", NULL, 0 },
  { "Romania",              "RO", NULL, 0 },
  { "Russia",               "RU", NULL, 0 },
  { "Serbia",               "RS", NULL, 0 },
  { "South Africa",         "ZA", NULL, 0 },
  { "Spain",                "ES", NULL, 0 },
  { "Sweden",               "SE", NULL, 0 },
  { "Switzerland",          "CH", NULL, 0 },
  { "Turkey",               "TR", NULL, 0 },
  { "Ukraine",              "UA", NULL, 0 },
  { "United Arab Emirates", "AE", NULL, 0 },
  { "United Kingdom",       "GB", NULL, 0 },
  { "United States",        "US", usRegions,
    (int)(sizeof(usRegions) / sizeof(usRegions[0])) },
};

#define RB_COUNTRY_COUNT ((int)(sizeof(rbCountries) / sizeof(rbCountries[0])))

// Die drei Ebenen des Browsers.
enum RbPage { RB_PAGE_COUNTRY, RB_PAGE_REGION, RB_PAGE_STATIONS };

// ---------------------------------------------------------------------------
// Mini-Skala der Lautstaerke, im Stil des Spektrumanalyzers gerastert:
// drei Punktzeilen uebereinander, Punktspalten im festen Abstand.
//
// Sie steht links in der Zeile der Batterie, deshalb nur 75 % der Breite -
// rechts davon bleibt das Batteriesymbol. Genau: 61 Punkte im Abstand 3 px,
// x 4..184, das sind 180 von 240 px. Hoehe wie das Batteriesymbol: drei
// Zeilen im Analyzerabstand 4 px, y 3..11 gegen y 2..11 der Batterie.
//
// Alle 3 x 61 Punkte stehen dauerhaft als dunkles Raster. Hell leuchten:
// die unterste Zeile auf ganzer Laenge ("die Linie"), die Raender links und
// rechts ueber alle drei Zeilen, die Marken bei 25 / 50 / 75 Prozent ueber
// zwei Zeilen - und der Ausschlag, der von links her alle drei Zeilen fuellt.
// Ausschlag und Dauermarken haben denselben Ton, hell ist doppelt so hell
// wie das Raster.
//
// VOL_DOTS ist bewusst ungerade, sonst gaebe es keine echte Mitte fuer die
// 50-Prozent-Marke. Rechte Kante = VOL_DOT_L + (VOL_DOTS-1) * VOL_DOT_PITCH.
// ---------------------------------------------------------------------------
#define VOL_DOT_L UI_MARGIN // x der ersten Punktspalte
#define VOL_DOTS        61  // Punktspalten, rechte Kante liegt damit bei 184
#define VOL_DOT_PITCH    3  // Abstand der Punktspalten
#define VOL_ROWS         3  // Punktzeilen uebereinander
#define VOL_BASE_Y      11  // unterste Punktzeile, buendig mit der Batterie
#define VOL_ROW_PITCH    4  // Abstand wie beim Analyzer, Skala ist 9 px hoch

// Die beiden Toene der Skala, urspruenglich aus dem Spektrumanalyzer: helles
// kaltes Blauweiss fuer den Ausschlag, gedaempftes Blau fuer das Raster
// dahinter. Stehen hier an einer Stelle, weil sich auch das Batteriesymbol
// daraus bedient - wer den Ton aendert, aendert beides zugleich.
#define UI_SCALE_ON   (M5Cardputer.Display.color565(120, 165, 210))
#define UI_SCALE_GRID (M5Cardputer.Display.color565(45, 60, 75))

// Helligkeitsstufen, die B der Reihe nach durchschaltet. Stufe 0 ist aus.
uint8_t brightnessLevels[5] = {0, 32, 64, 128, 255};
uint8_t currentBrightnessIndex = 4;

// Unterkante des Kopfbereichs. Alles darunter gehoert der Anzeige.
static int header_height = 50;

Audio audio;

// Zustand der Senderliste (Vollbildmenue, blendet alles andere aus).
bool stationMenuActive = false;
int menuIndex = 0;
const int MENU_TOP = 15;
const int MENU_VISIBLE = 6;   // gleichzeitig sichtbare Eintraege
int MENU_LINE_H = 0;          // wird in setup() aus der Schrifthoehe gesetzt

// Zustand des Systemmenues (schwebendes Fenster, BtnG0).
// Neue Eintraege einfach in die passende Liste - Fenster und Steuerung
// ziehen mit, nur die Behandlung in sysMenuSelect() muss dazu.
enum SysMenuPage { SYS_PAGE_MAIN, SYS_PAGE_WIFI, SYS_PAGE_LANG };

// Das Hauptmenue wird bei jedem Oeffnen zusammengestellt: "Sender sichern"
// erscheint nur, wenn gerade ein Sender aus der Online-Liste laeuft.
enum SysItem { IT_WIFI, IT_ONLINE, IT_LOCAL, IT_SAVE, IT_LANG, IT_EXIT };

static SysItem sysMainIds[6];
static int     sysMainCount = 0;

// Untermenues als Kennungen statt fertiger Texte - die Beschriftung holt
// sysMenuLabel() erst beim Zeichnen, sonst bliebe sie beim Sprachwechsel
// stehen. Die Reihenfolge ist zugleich die Nummerierung in sysMenuSelect().
static const StrId sysWifiItems[] = { STR_WM_INFO, STR_WM_SCAN,
                                      STR_WM_SAVED, STR_WM_RESET, STR_BACK };

// Sprachmenue: erst die Sprachen in ihrer eigenen Schreibweise, dann Back.
#define SYS_LANG_COUNT (LANG_COUNT + 1)

bool        sysMenuActive = false;
SysMenuPage sysMenuPage   = SYS_PAGE_MAIN;
int         sysMenuIndex  = 0;

// Sender aus dem Netz, ausserhalb der Liste von SD-Karte. Ist das gesetzt,
// spielt Playfile() diesen statt stations[curStation]. Jeder Senderwechsel
// mit den Pfeiltasten oder ueber die Senderliste hebt es wieder auf.
bool extStationActive = false;
char extStationName[MAX_NAME_LENGTH] = "";
char extStationUrl[RB_URL_LEN]       = "";   // Netz-URLs sind laenger als 100

// Ergebnis und Messwerte des letzten Abrufs bei radio-browser.info.
// Vollbildliste der lokalen Sender (BtnG0 -> Lokale Liste).
bool localListActive = false;
int  localSel = 0;
int  localTop = 0;

bool     rbListActive = false;   // Browser offen, egal auf welcher Ebene
RbPage   rbPage       = RB_PAGE_COUNTRY;
int      rbCountryIdx = 0;
int      rbRegionIdx  = 0;       // 0 = "Alle", sonst Region rbRegionIdx-1
int      rbOffset     = 0;       // Seitenanfang der Senderliste
// Namens- und URL-Tabelle, zusammen 4,4 KB. Wie die Analyzerpuffer nur
// vorhanden, solange sie gebraucht werden - also waehrend die Online-Liste
// offen ist. Zeiger auf Felder, die Zugriffe rbNames[i] bleiben dieselben.
char (*rbNames)[RB_NAME_LEN] = NULL;
char (*rbUrls)[RB_URL_LEN]   = NULL;
int      rbCount      = 0;
int      rbSel        = 0;
int      rbTop        = 0;   // erste sichtbare Zeile
int      rbStatus     = 0;   // HTTP-Status oder eine RB_ERR_-Kennung
uint32_t rbBytes      = 0;   // empfangene Nutzdaten
uint32_t rbMillis     = 0;   // Dauer des ganzen Abrufs
// Liegt gerade irgendein Fenster oder eine Vollbildliste ueber der
// Oberflaeche? Dann darf nichts an seine feste Position zeichnen. Wichtig
// vor allem fuer die Rueckrufe der Audio-Bibliothek: die feuern
// asynchron, wenn eine Verbindung zustande kommt, also womoeglich lange
// nachdem eine Liste aufgebaut wurde.
static inline bool uiOverlayActive() {
  return stationMenuActive || sysMenuActive || rbListActive || localListActive;
}

uint32_t rbHeapBefore = 0;   // freier Heap bei laufendem Stream
uint32_t rbHeapStopped = 0;  // ... nachdem der Stream angehalten wurde
uint32_t rbHeapAfter  = 0;
uint32_t rbHeapLow    = 0;   // kleinster freier Heap waehrend des Abrufs

// Hauptmenue neu zusammenstellen. extStationActive entscheidet, ob es den
// Eintrag zum Sichern gibt.
static void sysBuildMain() {
  int n = 0;
  sysMainIds[n++] = IT_WIFI;
  sysMainIds[n++] = IT_ONLINE;
  sysMainIds[n++] = IT_LOCAL;
  if (extStationActive) sysMainIds[n++] = IT_SAVE;
  sysMainIds[n++] = IT_LANG;
  sysMainIds[n++] = IT_EXIT;
  sysMainCount = n;
}

// Anzahl und Beschriftung der gerade gezeigten Ebene.
static int sysMenuCount() {
  switch (sysMenuPage) {
    case SYS_PAGE_WIFI:
      return (int)(sizeof(sysWifiItems) / sizeof(sysWifiItems[0]));
    case SYS_PAGE_LANG:
      return SYS_LANG_COUNT;
    default:
      return sysMainCount;
  }
}

static const char* sysMenuLabel(int i) {
  if (sysMenuPage == SYS_PAGE_WIFI) return T(sysWifiItems[i]);

  // Sprachmenue: die Sprachnamen bleiben unuebersetzt, damit sie in jeder
  // Oberflaeche wiedererkennbar sind. Nur "Back" zieht mit.
  if (sysMenuPage == SYS_PAGE_LANG) {
    return (i < LANG_COUNT) ? langNames[i] : T(STR_BACK);
  }

  switch (sysMainIds[i]) {
    case IT_WIFI:   return T(STR_MENU_WIFI);
    case IT_ONLINE: return T(STR_MENU_ONLINE);
    case IT_LOCAL:  return T(STR_MENU_LOCAL);
    case IT_SAVE:   return T(STR_MENU_SAVE);
    case IT_LANG:   return T(STR_MENU_LANG);
    default:        return T(STR_MENU_EXIT);
  }
}

// Streamtitel aus den ICY-Metadaten des Senders.
char currentStreamTitle[128] = "";
bool streamTitleChanged = false;   // loest den Neustart des Durchlaufs aus

// ---------------------------------------------------------------------------
// Ablage fuer die Rueckrufe der Audio-Bibliothek
//
// Die Bibliothek startet mit xTaskCreateStaticPinnedToCore() einen EIGENEN
// Task. audio_showstation(), audio_id3data() und audio_showstreamtitle()
// laufen darin - nicht in loop().
//
// Deshalb darf dort NICHT gezeichnet werden. M5GFX ist nicht dafuer gebaut,
// dass zwei Taskes gleichzeitig hineinschreiben; es belegt beim Zeichnen
// selbst Speicher, und setFont() aendert einen globalen Zustand mitten in
// eine laufende Ausgabe hinein. Genau daran ist der Heap zerbrochen: Der
// Absturz kam erst Minuten spaeter, im malloc des WLAN-Tasks, weil die
// Verkettung der freien Bloecke laengst zerstoert war.
//
// Regel: Rueckruf legt ab und setzt einen Merker, gezeichnet wird in loop().
// audio_showstreamtitle() macht das schon immer so, die beiden anderen
// ziehen jetzt nach.
//
// Der Merker wird erst beim Zeichnen geloescht. Steht gerade ein Fenster
// offen, bleibt die Meldung also stehen und erscheint, sobald es zu ist -
// frueher fiel sie in dem Fall ersatzlos aus.
// ---------------------------------------------------------------------------
char pendingStation[64] = "";
bool pendingStationChanged = false;

char pendingId3[128] = "";
bool pendingId3Changed = false;

unsigned long lastUpdate = 0;

struct RadioStation {
  char name[MAX_NAME_LENGTH];
  char url[MAX_URL_LENGTH];
};

// Notliste, falls keine SD-Karte steckt oder station_list.txt fehlt.
const PROGMEM RadioStation defaultStations[] = {
  {"RMF FM", "http://rmfstream1.interia.pl:80/rmf_fm"},
};

RadioStation stations[MAX_STATIONS];
size_t numStations = 0;
size_t curStation = 0;
uint16_t curVolume = 128;        // interne Skala 0..255
// Verhindert unnoetiges Neuzeichnen. -1 erzwingt es, 255 waere dafuer
// untauglich - das ist ein gueltiger Lautstaerkewert.
int lastVolumeDrawn = -1;

// Entprellung: alle Tasten teilen sich einen Zeitstempel.
unsigned long lastButtonPress = 0;
const unsigned long DEBOUNCE_DELAY = 200;

// Wie lange nach dem Verbindungsaufbau auf Audiodaten gewartet wird,
// bevor der Stream als nicht erreichbar gilt.
#define STREAM_TIMEOUT_MS 5000

enum StreamState { STREAM_CONNECTING, STREAM_OK, STREAM_FAILED };
StreamState streamState = STREAM_OK;
unsigned long streamStartMs = 0;

// Zuletzt im Footer gezeichnete Bitrate in kbit/s, -1 = Bereich ist leer.
int lastDrawnKbit = -1;

// Zuletzt gezeichnete WLAN-Feldstaerke in Prozent, -1 = Feld ist leer.
int lastDrawnWifiPct = -1;

// ---------------------------------------------------------------------------
// VU-Meter
// Zwei LED-Zeilen (L oben, R unten) mit dB-Skala dazwischen, im freien
// Bereich zwischen roter Trennlinie (y=50) und Footer (y=119).
// ---------------------------------------------------------------------------

#define VU_SEGMENTS  12     // LEDs pro Kanal
#define VU_SEG_W     16     // Breite einer LED (liegendes Format)
#define VU_SEG_GAP    3     // Luecke dazwischen
// Linke Kante der LED-Reihe. So gewaehlt, dass die Reihe rechts genau auf
// dem Seitenrand endet: 12 * (16+3) - 3 = 225 px breit, 236 - 225 = 11.
// Davor bleiben die 7 px zwischen Seitenrand und Reihe fuer L und R.
#define VU_X0        11
#define VU_BAR_H      8     // Hoehe einer LED
#define VU_L_Y       66     // Oberkante linke Zeile
#define VU_SCALE_Y   81     // Skala in der Mitte
#define VU_R_Y       96     // Oberkante rechte Zeile

#define VU_DB_FLOOR  -30.0f // linkes Ende der Skala in dB
#define VU_GREEN_MAX      7 // Segmente 0..7 gruen
#define VU_YELLOW_MAX     9 // 8..9 gelb, darueber rot
#define VU_OFF_DIV       10 // Helligkeitsteiler der erloschenen LEDs

#define VU_FRAME_MS      40 // Bildrate der Balken
#define VU_PEAK_HOLD_MS 1500 // Haltezeit des Peak-Segments
#define VU_PEAK_FALL_MS   90 // danach faellt es ein Segment pro ... ms

// Reihenfolge bestimmt, was F durchschaltet.
enum VisMode { VIS_OFF, VIS_VU, VIS_VU_PEAK, VIS_SPECTRUM };
#define VIS_MODES 4
VisMode visMode = VIS_VU;   // Startzustand: VU-Meter ohne Peak-Anzeige

// ---------------------------------------------------------------------------
// Spektrumanalyzer im VFD-Stil
// Die PCM-Samples kommen ueber den Callback audio_process_i2s() herein, der im
// Audio-Task laeuft. Dort wird nur kopiert; FFT und Zeichnen passieren in loop().
// ---------------------------------------------------------------------------

#define FFT_N          512  // Fensterlaenge, bei 44,1 kHz rund 86 Hz je Bin

#define SPEC_BANDS      10  // Saeulen
#define SPEC_COL_W      18  // Breite einer Saeule
#define SPEC_COL_PITCH  22  // Abstand der Saeulenmitten
#define SPEC_X0         12  // linke Kante der ersten Saeule
#define SPEC_ROWS       13  // Zeilen je Saeule, Zeile 0 ist die rote Grundlinie
#define SPEC_ROW_PITCH   4  // 1 px Linie + 3 px Luft
#define SPEC_BOTTOM_Y  108  // y der Grundlinie
#define SPEC_DOT_L UI_MARGIN        // Punktreihe am linken Rand
#define SPEC_DOT_R (239 - UI_MARGIN) // Punktreihe am rechten Rand

#define SPEC_DB_FLOOR -48.0f // unteres Ende der Dynamik
#define SPEC_GAIN_DB   18.0f // Anhebung: Musik verteilt sich auf viele Bins
#define SPEC_FRAME_MS    60  // Bildrate
#define SPEC_DECAY      0.5f // Zeilen, um die eine Saeule je Bild faellt

// Aufnahme- und Rechenpuffer des Analyzers, zusammen 7,2 KB.
//
// Sie liegen NICHT dauerhaft im Speicher, sondern werden erst angelegt,
// wenn F auf Spektrum steht, und beim Weiterschalten wieder freigegeben.
// Auf dem Cardputer ohne PSRAM sind im Betrieb nur rund 13 KB Heap frei -
// 7,2 KB davon dauerhaft fuer eine Anzeige zu belegen, die meist gar nicht
// laeuft, war der Hauptgrund fuer die Speichernot.
//
// specBuffersReady schuetzt gegen den Audio-Task: der Rueckruf steigt
// zusaetzlich zur Abfrage auf visMode auch hier aus, und beim Freigeben
// wird das Flag zuerst geloescht.
static volatile bool specBuffersReady = false;

static int16_t* specSamples = NULL;      // gefuellt vom Audio-Task
static float*   fftRe  = NULL;
static float*   fftIm  = NULL;
static float*   fftWin = NULL;           // Hannfenster, einmalig berechnet

// Uebergabe per Handschlag: solange specReady gesetzt ist, fasst der
// Audio-Task den Puffer nicht an und loop() darf ihn in Ruhe auswerten.
static volatile int  specFill = 0;
static volatile bool specReady = false;

static bool  specTablesReady = false;
static int   specBandBin[SPEC_BANDS + 1]; // Bandgrenzen als FFT-Bin-Nummern

static float specLevel[SPEC_BANDS];      // aktuelle Hoehe, gleitend fallend
static int   specPrevRows[SPEC_BANDS];   // zuletzt gezeichnete Hoehe

// Zuletzt gezeichneter Zustand je LED: 0 = aus, 1 = an, 2 = Peak.
// 0xFF erzwingt Neuzeichnen.
uint8_t vuPrevState[2][VU_SEGMENTS];
uint8_t vuPeakSeg[2] = {0, 0};
unsigned long vuPeakHoldUntil[2] = {0, 0};
unsigned long vuPeakNextFall[2] = {0, 0};

// Raeumt die Anzeigeflaeche zwischen Kopfbereich und Footer.
void clearContentArea() {
  M5Cardputer.Display.fillRect(
    0,
    header_height,
    M5Cardputer.Display.width(),
    M5Cardputer.Display.height() - header_height - FOOTER_HEIGHT,
    TFT_BLACK
  );
}

// Grundfarbe einer LED nach Zone: gruen - gelb - rot.
static void vuBandRgb(int seg, uint8_t &r, uint8_t &g, uint8_t &b) {
  if (seg <= VU_GREEN_MAX)       { r = 0;   g = 230; b = 60; }
  else if (seg <= VU_YELLOW_MAX) { r = 255; g = 190; b = 0;  }
  else                           { r = 255; g = 0;   b = 0;  }
}

// state: 0 = erloschen (dunkler Eigenfarbton, LED bleibt erkennbar),
//        1 = an, 2 = Peak (aufgehellt).
static uint16_t vuColor(int seg, uint8_t state) {
  uint8_t r, g, b;
  vuBandRgb(seg, r, g, b);

  if (state == 0) {
    return M5Cardputer.Display.color565(r / VU_OFF_DIV, g / VU_OFF_DIV, b / VU_OFF_DIV);
  }
  if (state == 2) {
    return M5Cardputer.Display.color565(128 + r / 2, 128 + g / 2, 128 + b / 2);
  }
  return M5Cardputer.Display.color565(r, g, b);
}

// Pegel 0..127 logarithmisch auf die Segmentzahl abbilden. Linear waere
// unbrauchbar: Musik saesse dann fast immer am oberen Anschlag.
static int vuLevelToSegs(uint8_t level) {
  if (level == 0) return 0;

  float db = 20.0f * log10f((float)level / 127.0f);
  if (db <= VU_DB_FLOOR) return 0;
  if (db > 0.0f) db = 0.0f;

  int segs = (int)(((db - VU_DB_FLOOR) / -VU_DB_FLOOR) * VU_SEGMENTS + 0.5f);
  if (segs > VU_SEGMENTS) segs = VU_SEGMENTS;
  return segs;
}

// Zeichnet eine einzelne LED. ch 0 = links, 1 = rechts.
static void vuDrawSeg(int ch, int seg, uint8_t state) {
  int y = (ch == 0) ? VU_L_Y : VU_R_Y;
  int x = VU_X0 + seg * (VU_SEG_W + VU_SEG_GAP);
  M5Cardputer.Display.fillRect(x, y, VU_SEG_W, VU_BAR_H, vuColor(seg, state));
}

// Merker auf "unbekannt" setzen, damit der naechste Durchlauf alles neu malt.
void vuReset() {
  for (int ch = 0; ch < 2; ch++) {
    for (int s = 0; s < VU_SEGMENTS; s++) vuPrevState[ch][s] = 0xFF;
    vuPeakSeg[ch] = 0;
    vuPeakHoldUntil[ch] = 0;
    vuPeakNextFall[ch] = 0;
  }
}

// Unveraenderliche Teile: Kanalbeschriftung und dB-Skala.
// Achtung: benutzt zwischendurch die halbe Schrift und Font0 und stellt die
// Hauptschrift am Ende wieder her - sonst zeichnet der Rest der Oberflaeche
// falsch.
void drawVuFrame() {
  const int barW = VU_SEGMENTS * (VU_SEG_W + VU_SEG_GAP) - VU_SEG_GAP;

  // L und R in halber Schrift: 8 px hoch, genau die Hoehe einer LED-Zeile,
  // damit sitzt die Beschriftung ohne Versatz mittig davor.
  M5Cardputer.Display.setFont(UI_FONT_SMALL);
  const int labelDy = (VU_BAR_H - (int)M5Cardputer.Display.fontHeight()) / 2;

  M5Cardputer.Display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  M5Cardputer.Display.drawString("L", UI_MARGIN, VU_L_Y + labelDy);
  M5Cardputer.Display.drawString("R", UI_MARGIN, VU_R_Y + labelDy);

  M5Cardputer.Display.setFont(&fonts::Font0);
  M5Cardputer.Display.setTextColor(TFT_DARKGREY, TFT_BLACK);
  M5Cardputer.Display.drawString("dB", UI_MARGIN, VU_SCALE_Y);

  // Die Marken passen zu VU_DB_FLOOR = -30. Wird der Fusspunkt geaendert,
  // wandern die LEDs, diese Zahlen aber nicht - dann hier nachziehen.
  const int marksDb[] = {-24, -18, -12, -9, -6, -3, 0};
  const char* marksTxt[] = {"-24", "-18", "-12", "-9", "-6", "-3", "0"};

  for (int i = 0; i < 7; i++) {
    if (marksDb[i] == 0) {
      M5Cardputer.Display.drawRightString(marksTxt[i], VU_X0 + barW, VU_SCALE_Y);
    } else {
      int x = VU_X0 + (int)(((marksDb[i] - VU_DB_FLOOR) / -VU_DB_FLOOR) * barW);
      M5Cardputer.Display.drawCentreString(marksTxt[i], x, VU_SCALE_Y);
    }
  }

  M5Cardputer.Display.setFont(UI_FONT);
  M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
}

// Holt die echten Pegel beider Kanaele und zieht nur geaenderte LEDs nach.
// getVUlevel() liefert beide Kanaele in einem Wort: links im oberen Byte.
void updateVuMeter() {
  if (visMode != VIS_VU && visMode != VIS_VU_PEAK) return;

  static unsigned long lastFrame = 0;
  unsigned long now = millis();
  if (now - lastFrame < VU_FRAME_MS) return;
  lastFrame = now;

  uint16_t vu = audio.getVUlevel();
  uint8_t level[2] = { (uint8_t)(vu >> 8), (uint8_t)(vu & 0xFF) };

  for (int ch = 0; ch < 2; ch++) {
    int segs = (streamState == STREAM_OK) ? vuLevelToSegs(level[ch]) : 0;

    // Peak steigt sofort mit, faellt nach Ablauf der Haltezeit stufenweise.
    if (segs > vuPeakSeg[ch]) {
      vuPeakSeg[ch] = segs;
      vuPeakHoldUntil[ch] = now + VU_PEAK_HOLD_MS;
      vuPeakNextFall[ch] = vuPeakHoldUntil[ch];
    }
    else if (now > vuPeakHoldUntil[ch] && now >= vuPeakNextFall[ch] && vuPeakSeg[ch] > 0) {
      vuPeakSeg[ch]--;
      vuPeakNextFall[ch] = now + VU_PEAK_FALL_MS;
    }

    for (int s = 0; s < VU_SEGMENTS; s++) {
      uint8_t state = (s < segs) ? 1 : 0;

      if (visMode == VIS_VU_PEAK && vuPeakSeg[ch] > 0 && s == vuPeakSeg[ch] - 1) {
        state = 2;
      }

      // Nur zeichnen, was sich geaendert hat - sonst flackert die Reihe.
      if (state != vuPrevState[ch][s]) {
        vuPrevState[ch][s] = state;
        vuDrawSeg(ch, s, state);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Spektrumanalyzer
// ---------------------------------------------------------------------------

// Hannfenster und logarithmisch verteilte Bandgrenzen einmalig aufbauen.
static void specInitTables() {
  if (specTablesReady) return;

  for (int i = 0; i < FFT_N; i++) {
    fftWin[i] = 0.5f * (1.0f - cosf(2.0f * PI * i / (FFT_N - 1)));
  }

  const float lo = 1.0f;              // tiefster genutzter Bin
  const float hi = FFT_N / 2 - 1;     // hoechster genutzter Bin

  for (int i = 0; i <= SPEC_BANDS; i++) {
    int bin = (int)(lo * powf(hi / lo, (float)i / SPEC_BANDS) + 0.5f);
    // Rundung kann im Bass mehrere Grenzen auf denselben Bin werfen.
    if (i > 0 && bin <= specBandBin[i - 1]) bin = specBandBin[i - 1] + 1;
    specBandBin[i] = bin;
  }

  specTablesReady = true;
}

// Iterative Radix-2-FFT, arbeitet direkt auf fftRe/fftIm.
// Erst Bitumkehr-Vertauschung, dann log2(N) Schmetterlingsstufen.
static void fftRadix2() {
  for (int i = 1, j = 0; i < FFT_N; i++) {
    int bit = FFT_N >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      float tr = fftRe[i]; fftRe[i] = fftRe[j]; fftRe[j] = tr;
      float ti = fftIm[i]; fftIm[i] = fftIm[j]; fftIm[j] = ti;
    }
  }

  for (int len = 2; len <= FFT_N; len <<= 1) {
    float ang = -2.0f * PI / len;
    float wr = cosf(ang), wi = sinf(ang);
    int half = len >> 1;

    for (int i = 0; i < FFT_N; i += len) {
      // Drehfaktor wird fortlaufend weitergedreht statt je Schritt neu
      // berechnet - spart die teuren Winkelfunktionen in der Innenschleife.
      float cr = 1.0f, ci = 0.0f;
      for (int k = 0; k < half; k++) {
        float vr = fftRe[i + k + half] * cr - fftIm[i + k + half] * ci;
        float vi = fftRe[i + k + half] * ci + fftIm[i + k + half] * cr;
        float ur = fftRe[i + k], ui = fftIm[i + k];

        fftRe[i + k] = ur + vr;
        fftIm[i + k] = ui + vi;
        fftRe[i + k + half] = ur - vr;
        fftIm[i + k + half] = ui - vi;

        float ncr = cr * wr - ci * wi;
        ci = cr * wi + ci * wr;
        cr = ncr;
      }
    }
  }
}

// Eine waagerechte Linie einer Saeule. row 0 ist die Grundlinie ganz unten.
static void specDrawRow(int band, int row, bool lit) {
  const int x = SPEC_X0 + band * SPEC_COL_PITCH;
  const int y = SPEC_BOTTOM_Y - row * SPEC_ROW_PITCH;

  if (row == 0) {   // Grundlinie, bleibt immer an
    M5Cardputer.Display.fillRect(x, y, SPEC_COL_W, 1,
                                 M5Cardputer.Display.color565(180, 75, 65));
    return;
  }

  // An: kaltes Blauweiss. Aus: derselbe Ton, nur ganz blass.
  const uint16_t col = lit ? M5Cardputer.Display.color565(120, 165, 210)
                           : M5Cardputer.Display.color565(15, 22, 30);

  M5Cardputer.Display.fillRect(x, y, SPEC_COL_W, 1, col);
}

// Grundbild: Randpunkte, alle Zeilen erloschen, Zaehler zurueckgesetzt.
void drawSpectrumFrame() {
  // Punktreihen am linken und rechten Rand des Analyzers.
  const uint16_t dot = M5Cardputer.Display.color565(45, 60, 75);
  for (int r = 0; r < SPEC_ROWS; r++) {
    int y = SPEC_BOTTOM_Y - r * SPEC_ROW_PITCH;
    M5Cardputer.Display.drawPixel(SPEC_DOT_L, y, dot);
    M5Cardputer.Display.drawPixel(SPEC_DOT_R, y, dot);
  }

  for (int b = 0; b < SPEC_BANDS; b++) {
    for (int r = 0; r < SPEC_ROWS; r++) specDrawRow(b, r, false);
    specLevel[b] = 0.0f;
    specPrevRows[b] = 0;
  }
}

// Kern des Analyzers: Fenster anwenden, FFT rechnen, Bins zu Baendern
// zusammenfassen, in dB umrechnen und die Saeulen nachziehen.
void updateSpectrum() {
  if (visMode != VIS_SPECTRUM) return;
  if (!specBuffersReady) return;

  static unsigned long lastFrame = 0;
  unsigned long now = millis();
  if (now - lastFrame < SPEC_FRAME_MS) return;
  lastFrame = now;

  // Nur rechnen, wenn der Audio-Task einen vollen Block geliefert hat.
  if (specReady) {
    specInitTables();

    for (int i = 0; i < FFT_N; i++) {
      fftRe[i] = (float)specSamples[i] * fftWin[i];
      fftIm[i] = 0.0f;
    }
    specReady = false;   // Puffer wieder freigeben

    fftRadix2();

    // Vollausschlag: Sinus mit Amplitude 32768 ergibt nach Hannfenster
    // rund FFT_N * 8192 im Spitzenbin -> das ist unsere 0-dB-Marke.
    const float ref = (float)FFT_N * 8192.0f;

    for (int b = 0; b < SPEC_BANDS; b++) {
      int from = specBandBin[b];
      int to   = specBandBin[b + 1] - 1;
      if (to < from) to = from;

      // Lautester Bin des Bandes bestimmt die Saeule - sieht lebendiger
      // aus als der Mittelwert.
      float peak = 0.0f;
      for (int k = from; k <= to && k < FFT_N / 2; k++) {
        float m = sqrtf(fftRe[k] * fftRe[k] + fftIm[k] * fftIm[k]);
        if (m > peak) peak = m;
      }

      float rows = 0.0f;
      if (peak > 0.0f) {
        float db = 20.0f * log10f(peak / ref) + SPEC_GAIN_DB;
        if (db > 0.0f) db = 0.0f;
        if (db > SPEC_DB_FLOOR) {
          rows = ((db - SPEC_DB_FLOOR) / -SPEC_DB_FLOOR) * (SPEC_ROWS - 1);
        }
      }

      // Neuer Wert setzt sich nur nach oben durch, sonst gilt der Abfall.
      if (rows > specLevel[b]) specLevel[b] = rows;
    }
  }

  // Abfallen und nur die geaenderten Zeilen nachziehen.
  for (int b = 0; b < SPEC_BANDS; b++) {
    specLevel[b] -= SPEC_DECAY;
    if (specLevel[b] < 0.0f) specLevel[b] = 0.0f;

    int rows = (int)specLevel[b];
    int prev = specPrevRows[b];
    if (rows == prev) continue;

    if (rows > prev) {
      for (int r = prev + 1; r <= rows; r++) specDrawRow(b, r, true);
    } else {
      for (int r = prev; r > rows; r--) specDrawRow(b, r, false);
    }
    specPrevRows[b] = rows;
  }
}

// Abgriff der fertig dekodierten Samples. Laeuft im Audio-Task, darf also
// nur kopieren. WICHTIG: continueI2S kommt als false herein - ohne das
// Setzen auf true verwirft die Library die Samples und es bleibt still.
//
// outBuff enthaelt Stereopaare, validSamples zaehlt die Paare. Hier wird
// auf Mono gemittelt, bis FFT_N Werte beisammen sind.
void audio_process_i2s(int16_t* outBuff, uint16_t validSamples,
                       uint8_t bitsPerSample, uint8_t channels,
                       bool *continueI2S) {
  *continueI2S = true;

  if (visMode != VIS_SPECTRUM) return;
  if (!specBuffersReady) return;   // Puffer gerade nicht vorhanden
  if (specReady) return;   // letzter Block noch nicht ausgewertet

  int fill = specFill;
  for (uint16_t i = 0; i < validSamples && fill < FFT_N; i++) {
    specSamples[fill++] = (int16_t)(((int32_t)outBuff[2 * i] + outBuff[2 * i + 1]) / 2);
  }

  if (fill >= FFT_N) {
    specFill = 0;
    specReady = true;
  } else {
    specFill = fill;
  }
}

// Statische Teile der gerade aktiven Darstellung zeichnen.
void drawVisFrame() {
  if (visMode == VIS_VU || visMode == VIS_VU_PEAK) {
    drawVuFrame();
    vuReset();
  }
  else if (visMode == VIS_SPECTRUM) {
    specFill = 0;
    specReady = false;
    drawSpectrumFrame();
  }
}

// F schaltet durch: aus - VU - VU mit Peak - Spektrum - aus
// Gibt die Analyzerpuffer frei. Reihenfolge ist wichtig: erst das Flag
// loeschen, kurz warten, damit ein gerade laufender Rueckruf durch ist,
// dann erst freigeben.
static void specFreeBuffers() {
  specBuffersReady = false;
  delay(5);

  free(specSamples); specSamples = NULL;
  free(fftRe);       fftRe = NULL;
  free(fftIm);       fftIm = NULL;
  free(fftWin);      fftWin = NULL;

  specFill = 0;
  specReady = false;
}

// Legt sie an. false, wenn der Speicher nicht reicht.
static bool specAllocBuffers() {
  if (specBuffersReady) return true;

  specSamples = (int16_t*)malloc(FFT_N * sizeof(int16_t));
  fftRe       = (float*)  malloc(FFT_N * sizeof(float));
  fftIm       = (float*)  malloc(FFT_N * sizeof(float));
  fftWin      = (float*)  malloc(FFT_N * sizeof(float));

  if (!specSamples || !fftRe || !fftIm || !fftWin) {
    specFreeBuffers();
    return false;
  }

  specTablesReady = false;   // Hannfenster und Bandgrenzen neu berechnen
  specFill = 0;
  specReady = false;
  specBuffersReady = true;
  return true;
}

// F schaltet durch. Beim Betreten des Spektrums werden die Puffer angelegt,
// beim Verlassen wieder freigegeben - siehe Kommentar bei den Zeigern.
void toggleVuMode() {
  VisMode next = (VisMode)((visMode + 1) % VIS_MODES);

  if (visMode == VIS_SPECTRUM && next != VIS_SPECTRUM) {
    visMode = next;            // erst umschalten, dann freigeben
    specFreeBuffers();
  }
  else if (next == VIS_SPECTRUM) {
    if (!specAllocBuffers()) next = VIS_OFF;   // kein Platz: Anzeige aus
    visMode = next;
  }
  else {
    visMode = next;
  }

  clearContentArea();
  drawVisFrame();
}

// Batteriesymbol oben rechts, ohne Beschriftung - der Ladestand steht nur
// noch in der Fuellung des Symbols.
// updateInterval = 0 erzwingt sofortiges Zeichnen.
void updateBatteryDisplay(unsigned long updateInterval) {
  static unsigned long lastUpdate = 0;

  if (millis() - lastUpdate < updateInterval) return;
  lastUpdate = millis();

  int batteryLevel = M5.Power.getBatteryLevel();

  const uint16_t colFrame = UI_SCALE_GRID;
  const uint16_t colFill  = UI_SCALE_ON;

  // Symbol buendig an den Seitenrand. Der Pluspol zaehlt zur Breite mit.
  const int battTotal = BATT_W + BATT_GAP + BATT_TIP_W;
  const int battX = 240 - UI_MARGIN - battTotal;

  M5Cardputer.Display.fillRect(battX, 0, battTotal, HDR_BATT_H, TFT_BLACK);

  // Rahmen aus vier Linien. Oben und unten enden sie BATT_R vor der Ecke,
  // links und rechts fangen sie BATT_R spaeter an - dadurch fehlen genau die
  // vier Eckpunkte und das Gehaeuse wirkt gerundet.
  M5Cardputer.Display.fillRect(
    battX + BATT_R, BATT_Y, BATT_W - 2 * BATT_R, 1, colFrame);
  M5Cardputer.Display.fillRect(
    battX + BATT_R, BATT_Y + BATT_H - 1, BATT_W - 2 * BATT_R, 1, colFrame);
  M5Cardputer.Display.fillRect(
    battX, BATT_Y + BATT_R, 1, BATT_H - 2 * BATT_R, colFrame);
  M5Cardputer.Display.fillRect(
    battX + BATT_W - 1, BATT_Y + BATT_R, 1, BATT_H - 2 * BATT_R, colFrame);

  // Ab BATT_R = 2 klafft zwischen den Linien eine Luecke. Ein Punkt diagonal
  // in jede Ecke schliesst sie und macht aus der Schraege eine Rundung.
  if (BATT_R > 1) {
    const int cx = battX + BATT_W - 2;
    const int cy = BATT_Y + BATT_H - 2;
    M5Cardputer.Display.fillRect(battX + 1, BATT_Y + 1, 1, 1, colFrame);
    M5Cardputer.Display.fillRect(cx,        BATT_Y + 1, 1, 1, colFrame);
    M5Cardputer.Display.fillRect(battX + 1, cy,         1, 1, colFrame);
    M5Cardputer.Display.fillRect(cx,        cy,         1, 1, colFrame);
  }

  // Pluspol, mittig zur Gehaeusehoehe.
  M5Cardputer.Display.fillRect(
    battX + BATT_W + BATT_GAP,
    BATT_Y + (BATT_H - BATT_TIP_H) / 2,
    BATT_TIP_W, BATT_TIP_H, colFrame);

  // Fuellung. Ein Rest Ladung soll sichtbar bleiben, deshalb mindestens
  // ein Pixel, solange der Ladestand ueber null liegt.
  const int fillMax = BATT_W - 2 * BATT_PAD;
  int fillW = (batteryLevel * fillMax + 50) / 100;
  if (batteryLevel > 0 && fillW < 1) fillW = 1;
  if (fillW > fillMax) fillW = fillMax;

  M5Cardputer.Display.fillRect(
    battX + BATT_PAD, BATT_Y + BATT_PAD,
    fillW, BATT_H - 2 * BATT_PAD, colFill);
}

// Notliste aus dem Programmspeicher uebernehmen.
void loadDefaultStations() {
  numStations = std::min(sizeof(defaultStations)/sizeof(defaultStations[0]), static_cast<size_t>(MAX_STATIONS));
  memcpy(stations, defaultStations, sizeof(RadioStation) * numStations);
}

// Platz eines Senders anhand der URL, sonst -1. Die URL ist das verlaessliche
// Merkmal - denselben Sender gibt es unter verschiedenen Namen.
static int stationIndexByUrl(const char* url) {
  for (int i = 0; i < numStations; i++) {
    if (strcmp(stations[i].url, url) == 0) return i;
  }
  return -1;
}

// Schreibt die ganze Liste zurueck auf die SD-Karte, im selben Format, in
// dem sie gelesen wird: je Zeile "Name,URL". "w" schneidet die Datei ab,
// die Datei ist danach also genau der aktuelle Stand.
static bool saveStationList() {
  if (!SD.begin()) return false;

  File f = SD.open("/station_list.txt", "w");
  if (!f) return false;

  for (int i = 0; i < numStations; i++) {
    f.print(stations[i].name);
    f.print(',');
    f.println(stations[i].url);
  }

  f.close();
  return true;
}

// Loescht einen Platz und rueckt den Rest nach. Der laufende Sender wird
// dabei nicht angehalten - nur der Zeiger auf die Liste wird nachgezogen.
static bool removeStationAt(int idx) {
  if (idx < 0 || idx >= numStations) return false;

  for (int i = idx; i < numStations - 1; i++) stations[i] = stations[i + 1];
  numStations--;

  if (curStation > idx) curStation--;
  if (curStation >= numStations) curStation = numStations > 0 ? numStations - 1 : 0;

  return saveStationList();
}

// Liest /station_list.txt von der SD-Karte, je Zeile "Name,URL".
// Der Name ist irrefuehrend: es wird nichts zusammengefuehrt, die Datei
// ersetzt die Liste vollstaendig. Faellt auf die Notliste zurueck, wenn
// keine Karte steckt, die Datei fehlt oder keine brauchbare Zeile enthaelt.
void mergeRadioStations() {
  if (!SD.begin()) {
    M5Cardputer.Display.drawString("/station_list.txt ", 20, 30);
    M5Cardputer.Display.drawString("Not found on SD card.", 20, 50);
    delay(4000);
    loadDefaultStations();
    M5Cardputer.Display.fillScreen(BLACK);
    return;
  }

  File file = SD.open("/station_list.txt");
  if (!file) {
    loadDefaultStations();
    return;
  }

  numStations = 0;

  String line;
  while (file.available() && numStations < MAX_STATIONS) {
    line = file.readStringUntil('\n');
    int commaIndex = line.indexOf(',');

    // Erstes Komma trennt Name und URL. Zeilen ohne Komma werden verworfen.
    if (commaIndex > 0) {
      String name = line.substring(0, commaIndex);
      String url = line.substring(commaIndex + 1);

      name.trim();
      url.trim();

      if (name.length() > 0 && url.length() > 0) {
        strncpy(stations[numStations].name, name.c_str(), MAX_NAME_LENGTH - 1);
        strncpy(stations[numStations].url, url.c_str(), MAX_URL_LENGTH - 1);
        stations[numStations].name[MAX_NAME_LENGTH - 1] = '\0';
        stations[numStations].url[MAX_URL_LENGTH - 1] = '\0';
        numStations++;
      }
    }
  }

  file.close();
  if (numStations == 0) {
    loadDefaultStations();
  }

}

// Sendername und Streamtitel im Kopfbereich neu setzen. Die Lautstaerke-
// skala darueber bleibt stehen, deshalb wird nur ab HDR_NAME_Y geraeumt.
void showStation() {
  if (uiOverlayActive()) return;   // nicht in ein offenes Fenster malen

  M5Cardputer.Display.fillRect(0, HDR_NAME_Y, 240, HDR_NAME_H, TFT_BLACK);
  M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5Cardputer.Display.drawString(
    extStationActive ? extStationName : stations[curStation].name,
    HDR_TEXT_X, HDR_NAME_Y);

  M5Cardputer.Display.fillRect(0, HDR_TITLE_Y, 240, HDR_TITLE_H, TFT_BLACK);

  if (currentStreamTitle[0] != '\0') {
    M5Cardputer.Display.setFont(UI_FONT_SMALL);
    M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5Cardputer.Display.drawString(currentStreamTitle, HDR_TEXT_X, HDR_TITLE_Y);
    M5Cardputer.Display.setFont(UI_FONT);
  }

  showVolume();
}

// Rueckruf der Library bei ID3-Daten. Schreibt in dieselbe Zeile wie der
// laufende Streamtitel, die beiden koennen sich also ueberschreiben.
// Laeuft im Audio-Task, zeichnet deshalb nicht - siehe pendingStation.
void audio_id3data(const char *info) {
  if (!info || !*info) return;

  strncpy(pendingId3, info, sizeof(pendingId3) - 1);
  pendingId3[sizeof(pendingId3) - 1] = '\0';
  pendingId3Changed = true;
}

// Startet den aktuell gewaehlten Sender und setzt die Ueberwachung auf.
// Wird auch von R und vom Senderwechsel benutzt.
void Playfile() {
  audio.stopSong();

  currentStreamTitle[0] = '\0';
  streamTitleChanged = true;

  // Merker des vorigen Senders verwerfen - sonst schriebe ein liegen
  // gebliebener ICY-Name den gerade gesetzten neuen wieder zu.
  pendingStationChanged = false;
  pendingId3Changed = false;

  streamState = STREAM_CONNECTING;
  streamStartMs = millis();

  const char* url = extStationActive ? extStationUrl
                                     : stations[curStation].url;

  bool connected = true;

  if (strstr(url, "http")) {
    connected = audio.connecttohost(url);
  }
  else if (strstr(url, "/mp3")) {
    if (!uiOverlayActive()) {
      M5Cardputer.Display.drawString(T(STR_IDLE_HDR), HDR_TEXT_X, HDR_NAME_Y);
    }
    connected = audio.connecttoFS(SD, url);
  }
  else {
    // Rest aus dem Ursprungsprojekt: liest bei unbrauchbarer URL einen
    // portugiesischen Satz per Sprachausgabe vor.
    connected = audio.connecttospeech(
      "Trabalhe em quanto os outros dormem, e você ficará com sono durante o dia.",
      "pt"
    );
  }

  // Verbindung schon beim Aufbau gescheitert - kein Warten noetig.
  if (!connected) streamState = STREAM_FAILED;

  showStation();
  drawFooter();
}

// Ueberwacht einen laufenden Verbindungsversuch: Sobald der Decoder eine
// Bitrate meldet, fliessen Audiodaten. Bleibt das aus, gilt der Stream
// nach STREAM_TIMEOUT_MS als nicht erreichbar.
void updateStreamStatus() {
  if (streamState != STREAM_CONNECTING) return;

  if (audio.isRunning() && audio.getBitRate() > 0) {
    streamState = STREAM_OK;
    drawFooter();
    return;
  }

  if (millis() - streamStartMs > STREAM_TIMEOUT_MS) {
    streamState = STREAM_FAILED;
    drawFooter();
  }
}

// Lautstaerke intern 0..255, die Library kennt nur 0..21.
void volumeUp() {
  if (curVolume >= 255) return;

  if (curVolume + VOLUME_STEP > 255)
    curVolume = 255;
  else
    curVolume += VOLUME_STEP;

  audio.setVolume(map(curVolume, 0, 255, 0, 21));
  showVolume();
}

void volumeDown() {
  if (curVolume == 0) return;

  if (curVolume <= VOLUME_STEP)
    curVolume = 0;
  else
    curVolume -= VOLUME_STEP;

  audio.setVolume(map(curVolume, 0, 255, 0, 21));
  showVolume();
}

bool isMuted = false;
uint16_t prevVolume = 0;

// Stummschaltung. Achtung: wer waehrend der Stummschaltung die Lautstaerke
// veraendert, verliert die Aenderung beim Aufheben wieder.
void volumeMute() {
  if (!isMuted) {
    prevVolume = curVolume;
    curVolume = 0;
    isMuted = true;
  } else {
    curVolume = prevVolume;
    isMuted = false;
  }
  audio.setVolume(map(curVolume, 0, 255, 0, 21));
  showVolume();
}

// Mini-Skala der Lautstaerke links in der Batteriezeile, Aufbau siehe die
// VOL_-Defines oben. Zeichnet immer alle Punkte neu, aber nur wenn sich die
// Lautstaerke geaendert hat - 183 Pixel sind schnell gesetzt.
void showVolume() {
  if ((int)curVolume == lastVolumeDrawn) return;
  lastVolumeDrawn = curVolume;

  // Beide Toene direkt aus dem Spektrumanalyzer uebernommen: colOn ist sein
  // kaltes Blauweiss der leuchtenden Zeilen, colGrid der gedaempfte Blauton
  // seiner Randpunktreihen. Hell bekommen Ausschlag, untere Linie, Viertel-
  // marken und die beiden Raender, dunkel das Raster dahinter.
  const uint16_t colOn   = UI_SCALE_ON;
  const uint16_t colGrid = UI_SCALE_GRID;

  // Zahl der gefuellten Punktspalten: 0 bei stumm, alle bei Vollausschlag.
  const int filled = ((int)curVolume * VOL_DOTS + 127) / 255;

  const int last    = VOL_DOTS - 1;
  const int quarter = last / 4;
  const int half    = last / 2;
  const int three   = (3 * last) / 4;

  for (int i = 0; i < VOL_DOTS; i++) {
    const int x = VOL_DOT_L + i * VOL_DOT_PITCH;

    // Hoehe der Dauermarke an dieser Spalte: 3 an den Enden, 2 bei den
    // Viertelmarken, sonst 1 - die durchgehende Linie ganz unten.
    int mark = 1;
    if (i == 0 || i == last) mark = VOL_ROWS;
    else if (i == quarter || i == half || i == three) mark = 2;

    for (int r = 0; r < VOL_ROWS; r++) {
      const int y = VOL_BASE_Y - r * VOL_ROW_PITCH;

      // Ausschlag und Dauermarke leuchten gleich hell, alles uebrige bleibt
      // als dunkles Raster stehen - so verschwindet auch ein fallender Pegel.
      uint16_t col = (i < filled || r < mark) ? colOn : colGrid;

      M5Cardputer.Display.drawPixel(x, y, col);
    }
  }
}

// Naechster Sender, laeuft am Ende der Liste wieder auf den ersten.
void stationUp() {
  if (numStations > 0) {
    currentStreamTitle[0] = '\0';
    streamTitleChanged = true;
    extStationActive = false;   // zurueck auf die Liste von SD

    curStation = (curStation + 1) % numStations;
    audio.stopSong();
    Playfile();
    showStation();
  }
  showVolume();
}

// Voriger Sender. Der Summand numStations verhindert negative Werte.
void stationDown() {

  currentStreamTitle[0] = '\0';
  streamTitleChanged = true;

  extStationActive = false;   // zurueck auf die Liste von SD

  if (numStations > 0) {
    curStation = (curStation - 1 + numStations) % numStations;
    audio.stopSong();
    Playfile();
    showStation();
  }
  showVolume();
}

// Komplette Oberflaeche neu aufbauen, etwa nach dem Schliessen des Menues.
// Menueschrift setzen bzw. wieder auf die Oberflaeche zurueckstellen.
// setTextSize() gilt global, das Zuruecksetzen ist deshalb Pflicht.
static void setMenuFont() {
  M5Cardputer.Display.setFont(MENU_FONT);
  M5Cardputer.Display.setTextSize(MENU_TEXT_SIZE);
}

static void restoreUiFont() {
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.setFont(UI_FONT);
}

// ---------------------------------------------------------------------------
// Abruf der Senderliste von radio-browser.info
// ---------------------------------------------------------------------------

// Holt die feste Abfrage und zieht die Sendernamen aus dem Datenstrom.
//
// Gesucht wird das Muster "name":" - der Schluessel kommt in einem Sender-
// objekt genau einmal vor. Ein Zustandsautomat laeuft ueber die Bloecke, das
// Muster darf also ueber eine Blockgrenze hinweg stehen. Danach wird bis zum
// naechsten unmaskierten Anfuehrungszeichen mitgeschrieben.
//
// Zeichen ausserhalb von ASCII werden zu '?' - die Nokia-Schrift kann nur
// 0x20..0x7E, sie kaemen sonst als Luecken heraus.
// Holt die feste Abfrage und zieht Sendernamen und Stream-URL heraus.
//
// Ohne HTTPClient, mit einem rohen WiFiClient. Grund: HTTPClient zerlegt den
// Antwortkopf mit String-Objekten, also vielen kleinen Anforderungen an den
// Heap. Nach einem gelaufenen Stream (die Audio-Bibliothek belegt fuer https
// einen grossen TLS-Kontext) reichte das nicht mehr: die Statuszeile kam
// leer an, HTTPClient meldete -7 "kein HTTP-Server", und der Audio-Task
// stockte hoerbar mit. Hier wird nichts angefordert - ein Puffer auf dem
// Stapel, sonst nur Zeiger.
//
// Der Dienst schickt Content-Length und kein chunked (nachgesehen), der
// Rumpf laesst sich also einfach bis zum Verbindungsende lesen.
//
// Ein Durchlauf, drei Abschnitte: Statuszeile, Kopf, Rumpf. Der Rumpf geht
// durch denselben Musterautomaten wie vorher - gesucht werden "name":" und
// "url_resolved":", in den Objekten steht der Name vor der URL, ein Paar
// gilt erst nach der URL als vollstaendig.
//
// Zeichen ausserhalb von ASCII werden zu '?' - die Nokia-Schrift kann nur
// 0x20..0x7E, sie kaemen sonst als Luecken heraus.
// Tabellen anlegen bzw. freigeben. Nur aus loop() heraus benutzt, kein
// anderer Task fasst sie an.
static void rbFreeTables() {
  free(rbNames); rbNames = NULL;
  free(rbUrls);  rbUrls = NULL;
  rbCount = 0;
}

static bool rbAllocTables() {
  if (rbNames && rbUrls) return true;

  rbNames = (char (*)[RB_NAME_LEN])calloc(RB_MAX_NAMES, RB_NAME_LEN);
  rbUrls  = (char (*)[RB_URL_LEN]) calloc(RB_MAX_NAMES, RB_URL_LEN);

  if (!rbNames || !rbUrls) {
    rbFreeTables();
    return false;
  }
  return true;
}

// Haengt s prozentkodiert an dst an. Alles ausser den unreservierten
// Zeichen wird zu %XX - noetig fuer Leerzeichen ("New York") und fuer die
// Umlaute der Bundeslaender, die als UTF-8-Bytes kodiert werden.
static void rbAppendEncoded(char* dst, size_t cap, const char* s) {
  static const char hex[] = "0123456789ABCDEF";
  size_t n = strlen(dst);

  for (; *s && n + 4 < cap; s++) {
    const unsigned char c = (unsigned char)*s;

    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      dst[n++] = (char)c;
    } else {
      dst[n++] = '%';
      dst[n++] = hex[c >> 4];
      dst[n++] = hex[c & 0x0F];
    }
  }

  dst[n] = '\0';
}

// Baut den Anfragepfad aus Land, Region und Seitenanfang.
static void rbBuildPath(char* dst, size_t cap) {
  const RbCountry& c = rbCountries[rbCountryIdx];

  snprintf(dst, cap, RB_PATH_BASE "&limit=%d&offset=%d&countrycode=%s",
           RB_MAX_NAMES, rbOffset, c.code);

  // rbRegionIdx 0 heisst "Alle" - dann kein state-Filter.
  if (c.regionCount > 0 && rbRegionIdx > 0) {
    strncat(dst, "&stateExact=true&state=", cap - strlen(dst) - 1);
    rbAppendEncoded(dst, cap, c.regions[rbRegionIdx - 1].query);
  }
}

// Ein Abruf gegen einen bestimmten Server. Den zweiten Versuch steuert
// rbFetch() weiter unten.
static void rbFetchOnce(const char* host) {
  rbCount = 0;
  rbSel = 0;
  rbTop = 0;
  rbBytes = 0;
  rbStatus = 0;

  if (!rbNames || !rbUrls) {   // ohne Tabellen kein Abruf
    rbStatus = RB_ERR_NO_TABLE;
    return;
  }

  // rbHeapBefore und rbHeapStopped setzt der Aufrufer, der haelt den
  // Stream an. Hier wird nur noch der Tiefstand mitgeschrieben.
  rbHeapLow = ESP.getFreeHeap();

  const uint32_t t0 = millis();

  if (WiFi.status() != WL_CONNECTED) {
    rbStatus = RB_ERR_NO_WIFI;
    rbMillis = millis() - t0;
    rbHeapAfter = ESP.getFreeHeap();
    return;
  }

  WiFiClient client;

  if (!client.connect(host, RB_PORT, RB_TIMEOUT_MS)) {
    rbStatus = RB_ERR_CONNECT;
    rbMillis = millis() - t0;
    rbHeapAfter = ESP.getFreeHeap();
    return;
  }

  // Anfrage stueckweise, ohne String-Objekte. Der Pfad steht auf dem
  // Stapel, alles andere sind Literale.
  char path[320];
  rbBuildPath(path, sizeof(path));

  client.print("GET ");
  client.print(path);
  client.print(" HTTP/1.1\r\nHost: ");
  client.print(host);                 // nicht mehr fest, siehe rbFetch()
  client.print("\r\nUser-Agent: " RB_AGENT "\r\n"
               "Connection: close\r\n"
               "\r\n");

  // Abschnitt der Antwort.
  enum { PH_STATUS, PH_HEAD, PH_BODY } phase = PH_STATUS;

  char statusLine[24];
  int  statusLen = 0;
  int  emptyLine = 0;     // 1 = die vorige Zeile endete gerade

  // Musterautomat fuer den Rumpf, unveraendert.
  static const char patName[] = "\"name\":\"";
  static const char patUrl[]  = "\"url_resolved\":\"";
  const int patNameLen = sizeof(patName) - 1;
  const int patUrlLen  = sizeof(patUrl) - 1;

  int  matchName = 0;
  int  matchUrl  = 0;
  bool inName    = false;
  bool inUrl     = false;
  bool escaped   = false;
  int  hexLeft   = 0;
  int  len       = 0;
  bool done      = false;

  uint8_t buf[256];

  while (!done) {
    audio.loop();         // Wiedergabe darf nicht abreissen

    if (millis() - t0 > RB_TIMEOUT_MS) {
      if (rbStatus == 0) rbStatus = RB_ERR_TIMEOUT;
      break;
    }

    if (!client.available()) {
      if (!client.connected()) break;   // Server hat zugemacht, fertig
      delay(1);
      continue;
    }

    int n = client.read(buf, sizeof(buf));
    if (n <= 0) continue;

    rbBytes += n;

    for (int i = 0; i < n; i++) {
      const char c = (char)buf[i];

      // --- Statuszeile: "HTTP/1.1 200 OK" ---------------------------------
      if (phase == PH_STATUS) {
        if (c == '\n') {
          statusLine[statusLen] = '\0';

          // Ziffern stehen ab Spalte 9. Kuerzer heisst: keine Statuszeile.
          rbStatus = (statusLen > 12) ? atoi(statusLine + 9) : 0;
          if (rbStatus <= 0) { rbStatus = RB_ERR_NO_STATUS; done = true; break; }

          phase = PH_HEAD;
          emptyLine = 0;
        } else if (c != '\r' && statusLen < (int)sizeof(statusLine) - 1) {
          statusLine[statusLen++] = c;
        }
        continue;
      }

      // --- Kopf ueberspringen bis zur Leerzeile ----------------------------
      if (phase == PH_HEAD) {
        if (c == '\r') continue;
        if (c == '\n') {
          if (emptyLine) {
            phase = PH_BODY;
            if (rbStatus != 200) { done = true; break; }
          }
          emptyLine = 1;
        } else {
          emptyLine = 0;
        }
        continue;
      }

      // --- Rumpf -----------------------------------------------------------
      if (!inName && !inUrl) {
        if (c == patName[matchName]) {
          if (++matchName == patNameLen) {
            inName = true; matchName = 0; matchUrl = 0;
            len = 0; escaped = false; hexLeft = 0;
            continue;
          }
        } else {
          matchName = (c == patName[0]) ? 1 : 0;
        }

        if (c == patUrl[matchUrl]) {
          if (++matchUrl == patUrlLen) {
            inUrl = true; matchName = 0; matchUrl = 0;
            len = 0; escaped = false; hexLeft = 0;
          }
        } else {
          matchUrl = (c == patUrl[0]) ? 1 : 0;
        }
        continue;
      }

      char* dst     = inName ? rbNames[rbCount] : rbUrls[rbCount];
      const int cap = inName ? RB_NAME_LEN : RB_URL_LEN;

      if (hexLeft > 0) { hexLeft--; continue; }

      if (escaped) {
        escaped = false;
        char out = '?';
        if (c == '"' || c == '\\' || c == '/') out = c;
        else if (c == 'n' || c == 't') out = ' ';
        else if (c == 'u') { hexLeft = 4; continue; }
        if (len < cap - 1) dst[len++] = out;
        continue;
      }

      if (c == '\\') { escaped = true; continue; }

      if (c == '"') {
        dst[len] = '\0';

        if (inName) {
          inName = false;
        } else {
          inUrl = false;
          rbCount++;
          if (rbCount >= RB_MAX_NAMES) { done = true; break; }
        }
        continue;
      }

      if (len < cap - 1) dst[len++] = (c >= 0x20 && c < 0x7F) ? c : '?';
    }

    const uint32_t heap = ESP.getFreeHeap();
    if (heap < rbHeapLow) rbHeapLow = heap;
  }

  client.stop();

  rbMillis = millis() - t0;
  rbHeapAfter = ESP.getFreeHeap();
}

// Lohnt ein zweiter Versuch? Nur bei Stoerungen, die von selbst vorbeigehen.
//
// Ausdruecklich NICHT dabei: RB_ERR_NO_WIFI (ohne Netz hilft kein zweiter
// Versuch), RB_ERR_NO_TABLE (der Speicher wird nicht mehr) und Status 200
// mit leerer Liste - das ist keine Stoerung, dort gibt es schlicht nichts.
static bool rbWorthRetry() {
  return rbStatus == RB_ERR_CONNECT
      || rbStatus == RB_ERR_TIMEOUT
      || rbStatus == RB_ERR_NO_STATUS
      || rbStatus >= 500;              // Fehler des Servers, nicht der Anfrage
}

// Abruf mit einem zweiten Versuch auf dem Rundlaufnamen.
//
// Der Stream steht waehrenddessen ohnehin still, der Aufrufer haelt ihn an -
// die Pause kostet also keine Wiedergabe, nur Wartezeit im schlechten Fall.
static void rbFetch() {
  rbFetchOnce(RB_HOST);
  if (!rbWorthRetry()) return;

  delay(RB_RETRY_PAUSE_MS);
  rbFetchOnce(RB_HOST2);
}


// Hinweis waehrend des Abrufs - der dauert je nach Netz eine Weile.
static void rbShowFetching() {
  M5Cardputer.Display.fillScreen(TFT_BLACK);
  M5Cardputer.Display.setFont(FOOTER_FONT);
  M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5Cardputer.Display.drawString("radio-browser.info ...", UI_MARGIN, 60);
}

// Zahl der Zeilen und Beschriftung der gerade gezeigten Ebene.
static int rbPageCount() {
  switch (rbPage) {
    case RB_PAGE_COUNTRY: return RB_COUNTRY_COUNT;
    case RB_PAGE_REGION:  return rbCountries[rbCountryIdx].regionCount + 1;
    default:              return rbCount;
  }
}

static const char* rbPageLabel(int i) {
  switch (rbPage) {
    case RB_PAGE_COUNTRY:
      return rbCountries[i].name;
    case RB_PAGE_REGION:
      return (i == 0) ? T(STR_RB_ALL) : rbCountries[rbCountryIdx].regions[i - 1].label;
    default:
      return rbNames[i];
  }
}

// Kopfzeile: wo bin ich gerade.
static void rbPageTitle(char* dst, size_t cap) {
  const RbCountry& c = rbCountries[rbCountryIdx];

  switch (rbPage) {
    case RB_PAGE_COUNTRY:
      snprintf(dst, cap, "%s", T(STR_RB_TITLE_C));
      break;
    case RB_PAGE_REGION:
      snprintf(dst, cap, "%s - Region", c.name);
      break;
    default:
      if (c.regionCount > 0 && rbRegionIdx > 0) {
        snprintf(dst, cap, T(STR_RB_TITLE_S),
                 c.regions[rbRegionIdx - 1].label, rbOffset + 1);
      } else {
        snprintf(dst, cap, T(STR_RB_TITLE_S), c.name, rbOffset + 1);
      }
      break;
  }
}

// Vollbildliste. Zeichnet alle drei Ebenen, nur Quelle und Fusszeile
// unterscheiden sich.
static void drawRbList() {
  M5Cardputer.Display.fillScreen(TFT_BLACK);
  M5Cardputer.Display.setFont(FOOTER_FONT);

  const uint16_t head = M5Cardputer.Display.color565(120, 165, 210);
  char buf[48];

  rbPageTitle(buf, sizeof(buf));
  M5Cardputer.Display.setTextColor(head, TFT_BLACK);
  M5Cardputer.Display.drawString(buf, UI_MARGIN, 1);

  const int count = rbPageCount();

  if (rbPage == RB_PAGE_STATIONS && count == 0) {
    M5Cardputer.Display.setTextColor(TFT_ORANGE, TFT_BLACK);
    M5Cardputer.Display.drawString(T(STR_RB_EMPTY), UI_MARGIN, 40);

    const char* grund;
    switch (rbStatus) {
      case RB_ERR_NO_WIFI:   grund = T(STR_RB_E_WIFI);                break;
      case RB_ERR_CONNECT:   grund = T(STR_RB_E_CONN);  break;
      case RB_ERR_TIMEOUT:   grund = T(STR_RB_E_TIME);      break;
      case RB_ERR_NO_STATUS: grund = T(STR_RB_E_STATUS); break;
      case RB_ERR_NO_TABLE:  grund = T(STR_RB_E_MEM);    break;
      case 200:              grund = T(STR_RB_E_NONE);      break;
      default:               grund = NULL;                       break;
    }

    if (grund) {
      M5Cardputer.Display.drawString(grund, UI_MARGIN, 54);
    } else {
      snprintf(buf, sizeof(buf), T(STR_RB_HTTP), rbStatus);
      M5Cardputer.Display.drawString(buf, UI_MARGIN, 54);
    }

    snprintf(buf, sizeof(buf), "%lu B, Heap min %lu",
             (unsigned long)rbBytes, (unsigned long)rbHeapLow);
    M5Cardputer.Display.drawString(buf, UI_MARGIN, 68);
  } else {
    for (int i = 0; i < RB_VISIBLE; i++) {
      const int idx = rbTop + i;
      if (idx >= count) break;

      const int y = RB_TOP_Y + i * RB_LINE_H;

      M5Cardputer.Display.setTextColor(
        idx == rbSel ? TFT_WHITE : M5Cardputer.Display.color565(120, 128, 128),
        TFT_BLACK);

      M5Cardputer.Display.drawString(idx == rbSel ? ">" : " ", UI_MARGIN, y);
      M5Cardputer.Display.drawString(rbPageLabel(idx), UI_MARGIN + 8, y);
    }
  }

  // Fusszeilen: Tastenhilfe und, auf der Senderebene, die Messwerte.
  M5Cardputer.Display.setTextColor(head, TFT_BLACK);

  if (rbPage == RB_PAGE_STATIONS) {
    snprintf(buf, sizeof(buf), T(STR_RB_STATS),
             rbCount, (unsigned long)rbMillis, (unsigned long)rbHeapLow);
    M5Cardputer.Display.drawString(buf, UI_MARGIN, 114);
    M5Cardputer.Display.drawString(T(STR_RB_FOOT_PLAY), UI_MARGIN, 124);
  } else {
    M5Cardputer.Display.drawString(T(STR_RB_FOOT_NEXT), UI_MARGIN, 114);
    M5Cardputer.Display.drawString(T(STR_RB_FOOT_BACK), UI_MARGIN, 124);
  }

  M5Cardputer.Display.setFont(UI_FONT);
  M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
}

// Auswahl im Bild halten.
static void rbScrollIntoView() {
  if (rbSel < rbTop) rbTop = rbSel;
  if (rbSel >= rbTop + RB_VISIBLE) rbTop = rbSel - RB_VISIBLE + 1;
}

// Senderseite holen: Stream anhalten (sonst reicht der Heap nicht),
// abrufen, Stream wieder aufnehmen. Siehe Kommentar an rbFetch().
static void rbLoadStations() {
  rbShowFetching();

  rbHeapBefore = ESP.getFreeHeap();
  audio.stopSong();
  delay(50);
  rbHeapStopped = ESP.getFreeHeap();

  rbAllocTables();
  rbFetch();

  Playfile();

  rbPage = RB_PAGE_STATIONS;
  rbSel = 0;
  rbTop = 0;
  drawRbList();
}

// Raeumt die Flaeche des groesstmoeglichen Fensters frei. Beim Wechsel der
// Ebene ist das neue Fenster womoeglich kleiner und wuerde sonst Reste des
// alten stehenlassen. Kopfbereich und Footer bleiben dabei stehen.
static void clearSysMenuArea() {
  const int maxCount = (int)(sizeof(sysWifiItems) / sizeof(sysWifiItems[0]));
  const int h = 2 * SYS_MENU_PAD + maxCount * SYS_MENU_LINE_H;
  const int x = (240 - SYS_MENU_W) / 2;
  const int y = (135 - h) / 2;

  M5Cardputer.Display.fillRect(x - 2, y - 2, SYS_MENU_W + 4, h + 4, TFT_BLACK);
}

// Das schwebende Fenster des Systemmenues, mittig auf der Anzeige. Zeichnet
// jedes Mal das ganze Fenster - bei einer Handvoll Eintraegen ist das
// billiger als das Nachfuehren einzelner Zeilen und kann nichts stehenlassen.
void drawSysMenu() {
  const int count = sysMenuCount();

  const int h = 2 * SYS_MENU_PAD + count * SYS_MENU_LINE_H;
  const int x = (240 - SYS_MENU_W) / 2;
  const int y = (135 - h) / 2;

  const uint16_t frame = M5Cardputer.Display.color565(120, 165, 210);
  const uint16_t dim   = M5Cardputer.Display.color565(45, 60, 75);

  // Zwei Pixel Schwarz rings um den Rahmen setzen das Fenster vom Analyzer
  // ab, der dahinter weiterlaeuft.
  M5Cardputer.Display.fillRect(x - 2, y - 2, SYS_MENU_W + 4, h + 4, TFT_BLACK);
  M5Cardputer.Display.drawRect(x, y, SYS_MENU_W, h, frame);

  setMenuFont();

  for (int i = 0; i < count; i++) {
    const int ly = y + SYS_MENU_PAD + i * SYS_MENU_LINE_H;
    const int ty = ly + (SYS_MENU_LINE_H - M5Cardputer.Display.fontHeight()) / 2;

    if (i == sysMenuIndex) {
      M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
      M5Cardputer.Display.drawString(">", x + SYS_MENU_PAD, ty);
    } else {
      M5Cardputer.Display.setTextColor(dim, TFT_BLACK);
    }

    M5Cardputer.Display.drawString(sysMenuLabel(i),
                                   x + SYS_MENU_PAD + SYS_MENU_MARK_W, ty);
  }

  restoreUiFont();
  M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
}

void openSysMenu() {
  sysMenuActive = true;
  sysMenuPage = SYS_PAGE_MAIN;
  sysMenuIndex = 0;
  sysBuildMain();
  drawSysMenu();
}

// Kurze Rueckmeldung im Stil des Menuefensters, bleibt ms Millisekunden
// stehen. audio.loop() laeuft dabei weiter.
static void sysMessage(const char* l1, const char* l2, uint32_t ms) {
  const int h = 2 * SYS_MENU_PAD + 2 * SYS_MENU_LINE_H;
  const int x = (240 - SYS_MENU_W) / 2;
  const int y = (135 - h) / 2;

  clearSysMenuArea();
  M5Cardputer.Display.drawRect(x, y, SYS_MENU_W, h,
                               M5Cardputer.Display.color565(120, 165, 210));

  setMenuFont();
  M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5Cardputer.Display.drawString(l1, x + SYS_MENU_PAD, y + SYS_MENU_PAD + 2);
  if (l2 && *l2) {
    M5Cardputer.Display.drawString(
      l2, x + SYS_MENU_PAD, y + SYS_MENU_PAD + SYS_MENU_LINE_H + 2);
  }
  restoreUiFont();

  const uint32_t t0 = millis();
  while (millis() - t0 < ms) {
    audio.loop();
    delay(1);
  }
}

// Schliessen: Das Fenster lag quer ueber Kopfbereich und Anzeige, also alles
// neu aufbauen. streamTitleChanged laesst die Laufschrift sauber neu starten
// statt an ihrer alten Stelle weiterzulaufen.
void closeSysMenu() {
  sysMenuActive = false;
  sysMenuPage = SYS_PAGE_MAIN;
  sysMenuIndex = 0;

  restoreUiFont();
  M5Cardputer.Display.fillScreen(TFT_BLACK);
  streamTitleChanged = true;
  redrawUI();
}

// Vor einer WLAN-Aktion: Wiedergabe anhalten und den Bildschirm in denselben
// Zustand bringen wie beim Start - die Schirme aus CardWifiSetup.h rechnen
// mit der Hauptschrift in einfacher Groesse.
static void wifiActionBegin() {
  audio.stopSong();
  streamState = STREAM_CONNECTING;

  // Ab jetzt bricht BtnG0 jede Maske in CardWifiSetup.h ab. Waehrend des
  // Starts bleibt das aus, dort ist der Knopf mit dem WLAN-Reset belegt.
  wifiAbort = false;
  wifiAbortEnabled = true;

  // Die Masken aus CardWifiSetup.h laufen im Skalenfont, 1,5-fach.
  // wifiActionEnd() stellt ueber restoreUiFont() wieder zurueck.
  wifiSetFont();
  M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5Cardputer.Display.fillScreen(TFT_BLACK);
}

// Danach: Menue verlassen, Sender wieder starten, Oberflaeche neu aufbauen.
static void wifiActionEnd() {
  wifiAbortEnabled = false;
  wifiAbort = false;

  sysMenuActive = false;
  sysMenuPage = SYS_PAGE_MAIN;
  sysMenuIndex = 0;

  restoreUiFont();
  M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5Cardputer.Display.fillScreen(TFT_BLACK);

  streamTitleChanged = true;
  lastDrawnWifiPct = -1;   // Feldstaerke hat sich sicher geaendert

  Playfile();
  redrawUI();
}

// Liste verlassen: zurueck ins Menue oder gleich zum Radio.
static void rbBackToMenu() {
  rbListActive = false;
  rbFreeTables();
  M5Cardputer.Display.fillScreen(TFT_BLACK);
  openSysMenu();
}

static void rbBackToRadio() {
  rbListActive = false;
  rbFreeTables();
  closeSysMenu();
}

// Den markierten Sender uebernehmen und abspielen. Er kommt nicht in die
// Liste von SD - er gilt nur, bis mit den Pfeiltasten oder ueber die
// Senderliste wieder ein eigener Sender gewaehlt wird.
static void rbPlaySelected() {
  if (!rbNames || !rbUrls) return;
  if (rbCount == 0 || rbUrls[rbSel][0] == '\0') return;

  strncpy(extStationName, rbNames[rbSel], sizeof(extStationName) - 1);
  extStationName[sizeof(extStationName) - 1] = '\0';

  strncpy(extStationUrl, rbUrls[rbSel], sizeof(extStationUrl) - 1);
  extStationUrl[sizeof(extStationUrl) - 1] = '\0';

  extStationActive = true;

  rbListActive = false;
  rbFreeTables();          // erst kopiert, dann freigeben
  sysMenuActive = false;
  sysMenuPage = SYS_PAGE_MAIN;
  sysMenuIndex = 0;

  restoreUiFont();
  M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5Cardputer.Display.fillScreen(TFT_BLACK);

  streamTitleChanged = true;

  Playfile();
  redrawUI();
}

// ENTER: eine Ebene tiefer, auf der Senderebene abspielen.
static void rbEnter() {
  switch (rbPage) {
    case RB_PAGE_COUNTRY:
      rbCountryIdx = rbSel;
      rbRegionIdx = 0;
      rbOffset = 0;

      if (rbCountries[rbCountryIdx].regionCount > 0) {
        rbPage = RB_PAGE_REGION;
        rbSel = 0;
        rbTop = 0;
        drawRbList();
      } else {
        rbLoadStations();      // Land ohne Regionen: gleich die Sender
      }
      break;

    case RB_PAGE_REGION:
      rbRegionIdx = rbSel;     // 0 = "Alle"
      rbOffset = 0;
      rbLoadStations();
      break;

    default:
      rbPlaySelected();
      break;
  }
}

// ESC / BS: eine Ebene zurueck, vom Land aus zurueck ins Menue.
static void rbBack() {
  switch (rbPage) {
    case RB_PAGE_STATIONS:
      rbFreeTables();          // Tabellen werden hier nicht mehr gebraucht
      if (rbCountries[rbCountryIdx].regionCount > 0) {
        rbPage = RB_PAGE_REGION;
        rbSel = rbRegionIdx;
      } else {
        rbPage = RB_PAGE_COUNTRY;
        rbSel = rbCountryIdx;
      }
      rbTop = 0;
      rbScrollIntoView();
      drawRbList();
      break;

    case RB_PAGE_REGION:
      rbPage = RB_PAGE_COUNTRY;
      rbSel = rbCountryIdx;
      rbTop = 0;
      rbScrollIntoView();
      drawRbList();
      break;

    default:
      rbBackToMenu();
      break;
  }
}

// Links und rechts blaettern - auf jeder Ebene, aber nicht dasselbe:
//
//   Sender  holt die naechste Seite beim Dienst (rbOffset), kostet also
//           einen Netzzugriff und haelt dafuer den Stream kurz an.
//   Land    springt um einen Bildschirm in der festen Tabelle, ohne Netz.
//   Region  ebenso.
//
// Bei 42 Laendern waeren es sonst 41 Tastendruecke von "Argentina" bis
// "United States".
//
// Verschoben wird die Auswahl, nicht der Ausschnitt: rbSel springt um
// RB_VISIBLE und rbScrollIntoView() zieht das Bild hinterher. Am Rand wird
// gekappt, ein Druck mehr schadet also nicht - er setzt die Auswahl auf den
// ersten bzw. letzten Eintrag.
static void rbListJump(int delta) {
  const int count = rbPageCount();
  if (count <= 0) return;

  rbSel += delta;
  if (rbSel < 0) rbSel = 0;
  if (rbSel > count - 1) rbSel = count - 1;

  rbScrollIntoView();
  drawRbList();
}

// Eine Seite ist RB_MAX_NAMES Eintraege; eine nicht volle Seite ist das Ende,
// dann geht es nicht weiter.
static void rbNextPage() {
  if (rbPage != RB_PAGE_STATIONS) { rbListJump(RB_VISIBLE); return; }
  if (rbCount < RB_MAX_NAMES) return;

  rbOffset += RB_MAX_NAMES;
  rbLoadStations();
}

static void rbPrevPage() {
  if (rbPage != RB_PAGE_STATIONS) { rbListJump(-RB_VISIBLE); return; }
  if (rbOffset == 0) return;

  rbOffset -= RB_MAX_NAMES;
  if (rbOffset < 0) rbOffset = 0;
  rbLoadStations();
}

// ---------------------------------------------------------------------------
// Lokale Senderliste als Vollbild: durchblaettern, abspielen, loeschen.
// Gezeichnet wie die Online-Liste, damit sich beide gleich anfuehlen.
// ---------------------------------------------------------------------------
static void drawLocalList() {
  M5Cardputer.Display.fillScreen(TFT_BLACK);
  M5Cardputer.Display.setFont(FOOTER_FONT);

  const uint16_t head = M5Cardputer.Display.color565(120, 165, 210);
  char line[48];

  snprintf(line, sizeof(line), T(STR_LOC_TITLE), numStations, MAX_STATIONS);
  M5Cardputer.Display.setTextColor(head, TFT_BLACK);
  M5Cardputer.Display.drawString(line, UI_MARGIN, 1);

  if (numStations == 0) {
    M5Cardputer.Display.setTextColor(TFT_ORANGE, TFT_BLACK);
    M5Cardputer.Display.drawString(T(STR_LOC_EMPTY), UI_MARGIN, 40);
  } else {
    for (int i = 0; i < RB_VISIBLE; i++) {
      const int idx = localTop + i;
      if (idx >= numStations) break;

      const int y = RB_TOP_Y + i * RB_LINE_H;

      M5Cardputer.Display.setTextColor(
        idx == localSel ? TFT_WHITE
                        : M5Cardputer.Display.color565(120, 128, 128),
        TFT_BLACK);

      M5Cardputer.Display.drawString(idx == localSel ? ">" : " ", UI_MARGIN, y);
      M5Cardputer.Display.drawString(stations[idx].name, UI_MARGIN + 8, y);
    }
  }

  M5Cardputer.Display.setTextColor(head, TFT_BLACK);
  M5Cardputer.Display.drawString("Ok=Play, Back=rmv", UI_MARGIN, 114);
  M5Cardputer.Display.drawString(T(STR_LOC_FOOT), UI_MARGIN, 124);

  M5Cardputer.Display.setFont(UI_FONT);
  M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
}

static void localScrollIntoView() {
  if (localSel < localTop) localTop = localSel;
  if (localSel >= localTop + RB_VISIBLE) localTop = localSel - RB_VISIBLE + 1;
}

static void openLocalList() {
  sysMenuActive = false;
  localListActive = true;

  // Auf dem laufenden Sender aufsetzen, sofern er aus der Liste kommt.
  localSel = extStationActive ? 0 : curStation;
  if (localSel >= numStations) localSel = 0;
  localTop = 0;
  localScrollIntoView();

  drawLocalList();
}

static void localBackToMenu() {
  localListActive = false;
  M5Cardputer.Display.fillScreen(TFT_BLACK);
  openSysMenu();
}

static void localBackToRadio() {
  localListActive = false;
  closeSysMenu();
}

// Den markierten Sender der lokalen Liste abspielen.
static void localPlaySelected() {
  if (numStations == 0) return;

  curStation = localSel;
  extStationActive = false;      // wieder ein Sender aus der eigenen Liste

  currentStreamTitle[0] = '\0';
  streamTitleChanged = true;

  localListActive = false;
  sysMenuActive = false;
  sysMenuPage = SYS_PAGE_MAIN;
  sysMenuIndex = 0;

  restoreUiFont();
  M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5Cardputer.Display.fillScreen(TFT_BLACK);

  Playfile();
  redrawUI();
}

// Den markierten Sender aus Liste und Datei werfen.
static void localRemoveSelected() {
  if (numStations == 0) return;

  const bool written = removeStationAt(localSel);

  if (localSel >= numStations) localSel = numStations > 0 ? numStations - 1 : 0;
  if (localTop > localSel) localTop = localSel;
  localScrollIntoView();

  if (!written) {
    // Aus dem Speicher ist er raus, auf der Karte steht er noch.
    sysMessage(T(STR_MSG_SD_CLOSED), T(STR_MSG_UNTIL_BOOT), 1800);
  }

  drawLocalList();
}

// Den laufenden Online-Sender in die lokale Liste uebernehmen.
static void sysSaveOnlineStation() {
  char l1[24];
  char l2[24];
  l2[0] = '\0';

  const int schon = extStationActive ? stationIndexByUrl(extStationUrl) : -1;

  if (!extStationActive) {
    snprintf(l1, sizeof(l1), "%s", T(STR_MSG_NOT_ONLINE));
  }
  else if (schon >= 0) {
    snprintf(l1, sizeof(l1), "%s", T(STR_MSG_ALREADY));
    snprintf(l2, sizeof(l2), T(STR_MSG_SLOT), schon + 1);
  }
  else if (numStations >= MAX_STATIONS) {
    snprintf(l1, sizeof(l1), "%s", T(STR_MSG_FULL));
    snprintf(l2, sizeof(l2), T(STR_MSG_ALL_USED), MAX_STATIONS);
  }
  else {
    strncpy(stations[numStations].name, extStationName, MAX_NAME_LENGTH - 1);
    stations[numStations].name[MAX_NAME_LENGTH - 1] = '\0';

    strncpy(stations[numStations].url, extStationUrl, MAX_URL_LENGTH - 1);
    stations[numStations].url[MAX_URL_LENGTH - 1] = '\0';

    numStations++;

    if (saveStationList()) {
      snprintf(l1, sizeof(l1), "%s", T(STR_MSG_SAVED));
      snprintf(l2, sizeof(l2), T(STR_MSG_SLOT_OF), numStations, MAX_STATIONS);
    } else {
      numStations--;                 // nichts geschrieben, also zurueck
      snprintf(l1, sizeof(l1), "%s", T(STR_MSG_SD_CLOSED));
      snprintf(l2, sizeof(l2), "%s", T(STR_MSG_NOT_SAVED));
    }
  }

  sysMessage(l1, l2, 1800);

  // Der Eintrag kann jetzt weg sein oder dazugekommen - Menue neu bauen.
  sysBuildMain();
  if (sysMenuIndex >= sysMainCount) sysMenuIndex = sysMainCount - 1;
  clearSysMenuArea();
  drawSysMenu();
}

// Was ENTER auf dem gewaehlten Eintrag ausloest.
static void sysMenuSelect() {

  if (sysMenuPage == SYS_PAGE_MAIN) {
    switch (sysMainIds[sysMenuIndex]) {
      case IT_WIFI:                        // WiFi -> Untermenue
        sysMenuPage = SYS_PAGE_WIFI;
        sysMenuIndex = 0;
        clearSysMenuArea();
        drawSysMenu();
        break;

      case IT_ONLINE:                      // Sender online: Land zuerst
        sysMenuActive = false;
        rbListActive = true;
        rbPage = RB_PAGE_COUNTRY;
        rbCountryIdx = 0;
        rbRegionIdx = 0;
        rbOffset = 0;
        rbSel = 0;
        rbTop = 0;
        drawRbList();
        break;

      case IT_LOCAL:                       // Lokale Liste
        openLocalList();
        break;

      case IT_SAVE:                        // laufenden Online-Sender sichern
        sysSaveOnlineStation();
        break;

      case IT_LANG:                        // Sprache -> Untermenue
        sysMenuPage = SYS_PAGE_LANG;
        sysMenuIndex = uiLang;             // die laufende Sprache steht vor
        clearSysMenuArea();
        drawSysMenu();
        break;

      case IT_EXIT:
        closeSysMenu();
        break;
    }
    return;
  }

  // Sprachmenue. Nach der Wahl geht es zurueck ins Hauptmenue, das dann
  // gleich in der neuen Sprache dasteht.
  if (sysMenuPage == SYS_PAGE_LANG) {
    if (sysMenuIndex < LANG_COUNT) {
      uiLang = (uint8_t)sysMenuIndex;
      langSave();
    }

    sysMenuPage = SYS_PAGE_MAIN;
    sysMenuIndex = 0;
    sysBuildMain();                        // Beschriftungen neu holen
    clearSysMenuArea();
    drawSysMenu();
    return;
  }

  // WLAN-Untermenue. Alle Aktionen halten das Radio an, laufen mit den
  // Schirmen aus CardWifiSetup.h und kehren danach zum Radio zurueck.
  switch (sysMenuIndex) {
    case 0:                                // WiFi-Info
      wifiActionBegin();
      displayWiFiInfo();
      wifiActionEnd();
      break;

    case 1:                                // Scan+Connect
      wifiActionBegin();
      scanAndConnectWiFi();
      wifiActionEnd();
      break;

    case 2:                                // Gespeicherte
      wifiActionBegin();
      connectSavedWiFi();
      wifiActionEnd();
      break;

    case 3:                                // Reset
      wifiActionBegin();
      resetAndReconnectWiFi();
      wifiActionEnd();
      break;

    case 4:                                // Back
      sysMenuPage = SYS_PAGE_MAIN;
      sysMenuIndex = 0;
      clearSysMenuArea();
      drawSysMenu();
      break;
  }
}

void redrawUI() {
  showStation();
  lastVolumeDrawn = -1;   // erzwingt das Neuzeichnen der Lautstaerkeskala
  showVolume();
  updateBatteryDisplay(0);
  drawFooter();

  drawVisFrame();
}

void setup() {
  auto cfg = M5.config();
  auto spk_cfg = M5Cardputer.Speaker.config();
    spk_cfg.sample_rate = 44100;
    spk_cfg.task_pinned_core = APP_CPU_NUM;
    M5Cardputer.Speaker.config(spk_cfg);

  M5Cardputer.begin(cfg, true);

#if DEBUG_SERIAL
  // cdc_on_boot=1 bei diesem Board: Serial ist die USB-Schnittstelle, die
  // Baudrate ist dabei ohne Bedeutung. Kein Warten auf den Monitor - das
  // Radio soll auch ohne angeschlossenen Rechner starten.
  Serial.begin(115200);
  Serial.println();
  Serial.println("[boot] WebRadio Cardputer Adv");
#endif

  M5Cardputer.Speaker.begin();
  M5Cardputer.Speaker.setVolume(255);

  M5Cardputer.Display.setBrightness(brightnessLevels[currentBrightnessIndex]);

  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.setFont(UI_FONT);

  // Sprache aus dem NVS holen, bevor der erste Text erscheint.
  langLoad();

  // Zeilenhoehe des Menues: feste Oberflaechenzeile plus 2 px Luft.
  MENU_LINE_H = UI_LINE_H + 2;

  // Blockiert, bis eine Verbindung steht. BtnG0 setzt dabei das WLAN zurueck.
  // Die Masken darin stellen auf den Skalenfont in 1,5-facher Groesse um;
  // das gilt global und muss danach zurueck, sonst zeichnet die ganze
  // Oberflaeche zu gross.
  connectToWiFi();
  restoreUiFont();

  audio.setPinout(I2S_BCK, I2S_WS, I2S_DOUT);
  audio.setVolume(map(curVolume, 0, 255, 0, 21));
  audio.setBalance(0);

  M5Cardputer.Display.fillScreen(BLACK);

  audio.stopSong();
  mergeRadioStations();
  Playfile();
  clearContentArea();
  redrawUI();
}

#if DEBUG_SERIAL
// Heap-Bericht im Abstand von DEBUG_HEAP_MS. Steht ganz oben in loop(),
// damit er auch laeuft, wenn eine der Vollbildlisten die Schleife uebernimmt
// und frueh zurueckspringt.
//
// Die blockierenden Masken aus CardWifiSetup.h erreicht er nicht - die haben
// ihre eigene Schleife und rufen loop() nicht auf.
static void debugHeapReport() {
  static uint32_t last = 0;
  if (millis() - last < DEBUG_HEAP_MS) return;
  last = millis();

  Serial.printf("[heap] frei %lu  min %lu  groesster Block %lu\n",
                (unsigned long)ESP.getFreeHeap(),
                (unsigned long)ESP.getMinFreeHeap(),
                (unsigned long)ESP.getMaxAllocHeap());
}
#endif

void loop() {
  audio.loop();       // muss oft laufen, sonst reisst die Wiedergabe ab
  M5Cardputer.update();

#if DEBUG_SERIAL
  debugHeapReport();
#endif

  // Vollbildliste der lokalen Sender. Nimmt alle Tasten fuer sich.
  if (localListActive) {
    delay(1);

    if (M5Cardputer.BtnA.wasPressed()) {
      localBackToRadio();
      return;
    }

    if (M5Cardputer.Keyboard.isChange() &&
        millis() - lastButtonPress > DEBOUNCE_DELAY) {

      if (M5Cardputer.Keyboard.isKeyPressed(';')) {          // hoch
        if (localSel > 0) { localSel--; localScrollIntoView(); drawLocalList(); }
      }
      else if (M5Cardputer.Keyboard.isKeyPressed('.')) {     // runter
        if (localSel < numStations - 1) {
          localSel++; localScrollIntoView(); drawLocalList();
        }
      }
      else if (M5Cardputer.Keyboard.isKeyPressed(KEY_ENTER)) {
        localPlaySelected();
      }
      else if (M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE)) {
        localRemoveSelected();             // BS loescht, ESC geht zurueck
      }
      else if (M5Cardputer.Keyboard.isKeyPressed('`')) {
        localBackToMenu();
      }

      lastButtonPress = millis();
    }

    return;
  }

  // Vollbildliste der online geholten Sender. Nimmt ebenfalls alle Tasten.
  if (rbListActive) {
    delay(1);

    if (M5Cardputer.BtnA.wasPressed()) {
      rbBackToRadio();
      return;
    }

    if (M5Cardputer.Keyboard.isChange() &&
        millis() - lastButtonPress > DEBOUNCE_DELAY) {

      const int count = rbPageCount();

      if (M5Cardputer.Keyboard.isKeyPressed(';')) {          // hoch
        if (rbSel > 0) { rbSel--; rbScrollIntoView(); drawRbList(); }
      }
      else if (M5Cardputer.Keyboard.isKeyPressed('.')) {     // runter
        if (rbSel < count - 1) { rbSel++; rbScrollIntoView(); drawRbList(); }
      }
      else if (M5Cardputer.Keyboard.isKeyPressed(KEY_ENTER)) {
        rbEnter();
      }
      else if (M5Cardputer.Keyboard.isKeyPressed('/')) {     // rechts
        rbNextPage();
      }
      else if (M5Cardputer.Keyboard.isKeyPressed(',')) {     // links
        rbPrevPage();
      }
      else if (M5Cardputer.Keyboard.isKeyPressed('`') ||
               M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE)) {
        rbBack();
      }

      lastButtonPress = millis();
    }

    return;
  }

  // Systemmenue: schwebendes Fenster, nimmt alle Tasten fuer sich. Der Rest
  // der Schleife wird uebersprungen, damit nichts ins Fenster hineinzeichnet.
  if (sysMenuActive) {
    delay(1);

    // BtnG0 fuehrt aus jeder Ebene sofort zum Radio zurueck.
    if (M5Cardputer.BtnA.wasPressed()) {
      closeSysMenu();
      return;
    }

    if (M5Cardputer.Keyboard.isChange() &&
        millis() - lastButtonPress > DEBOUNCE_DELAY) {

      const int count = sysMenuCount();

      if (M5Cardputer.Keyboard.isKeyPressed(';')) {          // hoch
        sysMenuIndex = (sysMenuIndex - 1 + count) % count;
        drawSysMenu();
      }
      else if (M5Cardputer.Keyboard.isKeyPressed('.')) {     // runter
        sysMenuIndex = (sysMenuIndex + 1) % count;
        drawSysMenu();
      }
      else if (M5Cardputer.Keyboard.isKeyPressed(KEY_ENTER)) {
        sysMenuSelect();
      }
      // ESC liefert auf dem Cardputer das aufgedruckte Zeichen ` , die
      // Taste rechts daneben BS. Beide gehen eine Ebene zurueck und
      // schliessen das Fenster, wenn schon das Hauptmenue sichtbar ist.
      else if (M5Cardputer.Keyboard.isKeyPressed('`') ||
               M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE)) {
        if (sysMenuPage == SYS_PAGE_MAIN) {
          closeSysMenu();
        } else {
          sysMenuPage = SYS_PAGE_MAIN;
          sysMenuIndex = 0;
          clearSysMenuArea();
          drawSysMenu();
        }
      }

      lastButtonPress = millis();
    }

    return;
  }

  // BtnG0 oeffnet das Systemmenue.
  if (M5Cardputer.BtnA.wasPressed()) {
    openSysMenu();
    return;
  }

  // Bei offener Senderliste uebernimmt das Menue die gesamte Anzeige und
  // alle Tasten. Der Rest der Schleife wird uebersprungen.
  if (stationMenuActive) {
    delay(1);
    if (M5Cardputer.Keyboard.isChange() &&
      millis() - lastButtonPress > DEBOUNCE_DELAY) {

      if (M5Cardputer.Keyboard.isKeyPressed('.')) {        // runter
        menuIndex = (menuIndex + 1) % numStations;
        drawStationMenu();
      }
      else if (M5Cardputer.Keyboard.isKeyPressed(';')) {   // hoch
        menuIndex = (menuIndex - 1 + numStations) % numStations;
        drawStationMenu();
      }
      else if (M5Cardputer.Keyboard.isKeyPressed(KEY_ENTER)) {
        curStation = menuIndex;
        extStationActive = false;   // zurueck auf die Liste von SD

        currentStreamTitle[0] = '\0';
        streamTitleChanged = true;

        audio.stopSong();
        Playfile();

        stationMenuActive = false;

        M5Cardputer.Display.fillRect(0, 0, 240, 14, TFT_BLACK);
        clearContentArea();

        redrawUI();

      }

      else if (M5Cardputer.Keyboard.isKeyPressed('l')) {   // Menue verlassen
        stationMenuActive = false;

        M5Cardputer.Display.fillRect(0, 0, 240, 14, TFT_BLACK);
        clearContentArea();

        redrawUI();

      }

      lastButtonPress = millis();

    }

    return;
  }

  updateBatteryDisplay(5000);

  if (M5Cardputer.Keyboard.isChange() && (millis() - lastButtonPress > DEBOUNCE_DELAY)) {

    if (M5Cardputer.Keyboard.isKeyPressed(';')) volumeUp();        // hoch
    else if (M5Cardputer.Keyboard.isKeyPressed('.')) volumeDown(); // runter
    else if (M5Cardputer.Keyboard.isKeyPressed('m')) volumeMute();
    else if (M5Cardputer.Keyboard.isKeyPressed('/')) stationUp();  // rechts
    else if (M5Cardputer.Keyboard.isKeyPressed(',')) stationDown(); // links
    else if (M5Cardputer.Keyboard.isKeyPressed('r')) {
      Playfile();
    }
    else if (M5Cardputer.Keyboard.isKeyPressed('f')) {
      toggleVuMode();
    }
    else if (M5Cardputer.Keyboard.isKeyPressed('b')) {
      toggleBrightness();
    }
    else if (M5Cardputer.Keyboard.isKeyPressed('l')) {
      openStationMenu();
    }


    lastButtonPress = millis();

   }

  // Alle folgenden Aufrufe bremsen sich selbst und zeichnen nur bei Bedarf.
  updateStreamStatus();
  drawWifi();
  drawBitrate();
  updateVuMeter();
  updateSpectrum();

  drawPendingHeader();   // was die Rueckrufe hinterlegt haben
  drawStreamTitle();

  delay(1);
}

#if DEBUG_SERIAL
// Rueckruf der Library fuer ihre eigenen Meldungen. Ohne diese Funktion
// bleibt das schwache Symbol audio_info NULL und die Bibliothek schweigt.
void audio_info(const char *info) {
    Serial.print("[audio] ");
    Serial.println(info ? info : "");
}
#endif

// Rueckruf der Library: Sendername aus dem ICY-Kopf. Laeuft im Audio-Task,
// zeichnet deshalb nicht - siehe die Erklaerung bei pendingStation.
void audio_showstation(const char *showstation) {
    if (!showstation || !*showstation) return;

    strncpy(pendingStation, showstation, sizeof(pendingStation) - 1);
    pendingStation[sizeof(pendingStation) - 1] = '\0';
    pendingStationChanged = true;
}

// Rueckruf der Library: neuer Titel. Wird nur uebernommen, gezeichnet wird
// spaeter in drawStreamTitle().
void audio_showstreamtitle(const char *info) {
  if (!info || !*info) return;

  strncpy(currentStreamTitle, info, sizeof(currentStreamTitle) - 1);
  currentStreamTitle[sizeof(currentStreamTitle) - 1] = '\0';
  streamTitleChanged = true;
}

// Kern des Durchlaufs. Erwartet, dass die halbe Schrift bereits gesetzt ist -
// die vielen Ausstiege machen es sonst unuebersichtlich, sie zurueckzustellen.
static void scrollStreamTitle() {
  static int lastX = -10000;
  static int xOffset = 0;
  static bool scrolling = false;
  static bool scrollDone = false;
  static unsigned long lastUpdate = 0;

  const int screenW = M5Cardputer.Display.width();
  const int startScrollX = 80;
  const int yText = HDR_TITLE_Y;
  const int hText = HDR_TITLE_H;
  const int scrollSpeed = 20;   // ms je Pixel

  if (!currentStreamTitle[0]) {
    M5Cardputer.Display.fillRect(0, yText, screenW, hText, TFT_BLACK);
    return;
  }

  int textWidth = M5Cardputer.Display.textWidth(currentStreamTitle);

  // Neuer Titel: Durchlauf von vorn beginnen.
  if (streamTitleChanged) {
    xOffset = startScrollX;
    scrolling = false;
    scrollDone = false;
    streamTitleChanged = false;
    lastX = -10000;

    M5Cardputer.Display.fillRect(0, yText, screenW, hText, TFT_BLACK);
  }

  // Kurze Titel stehen einfach da. Der linke Anschlag geht von der Breite ab,
  // sonst gilt ein Titel als passend, der rechts hinausragt.
  if (textWidth <= screenW - HDR_TEXT_X) {
    if (lastX != HDR_TEXT_X) {
      lastX = HDR_TEXT_X;
      M5Cardputer.Display.fillRect(0, yText, screenW, hText, TFT_BLACK);
      M5Cardputer.Display.drawString(currentStreamTitle, HDR_TEXT_X, yText);
    }
    return;
  }

  // Durchlauf beendet: Titel bleibt am linken Anschlag stehen.
  if (scrollDone) {
    if (lastX != HDR_TEXT_X) {
      lastX = HDR_TEXT_X;
      M5Cardputer.Display.fillRect(0, yText, screenW, hText, TFT_BLACK);
      M5Cardputer.Display.drawString(currentStreamTitle, HDR_TEXT_X, yText);
    }
    return;
  }

  if (!scrolling) {
    scrolling = true;
    xOffset = startScrollX;
    lastUpdate = millis();
  }

  if (millis() - lastUpdate < scrollSpeed) return;
  lastUpdate = millis();

  xOffset--;

  if (xOffset < -textWidth) {
    scrollDone = true;
    scrolling = false;
    return;
  }

  if (xOffset != lastX) {
    lastX = xOffset;
    M5Cardputer.Display.fillRect(0, yText, screenW, hText, TFT_BLACK);
    M5Cardputer.Display.drawString(currentStreamTitle, xOffset, yText);
  }

}

// Zeichnet, was die Rueckrufe der Audio-Bibliothek hinterlegt haben.
//
// Laeuft in loop() und damit im selben Task wie alle uebrigen Zeichen-
// funktionen. Das ist der ganze Zweck der Uebung: Nur ein Task fasst M5GFX
// an. Siehe die Erklaerung bei pendingStation.
static void drawPendingHeader() {
  if (!pendingStationChanged && !pendingId3Changed) return;
  if (uiOverlayActive()) return;   // Merker bleibt stehen, kommt nachher dran

  if (pendingStationChanged) {
    pendingStationChanged = false;

    M5Cardputer.Display.setFont(UI_FONT);
    M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5Cardputer.Display.fillRect(0, HDR_NAME_Y, 240, HDR_NAME_H, TFT_BLACK);
    M5Cardputer.Display.drawString(pendingStation, HDR_TEXT_X, HDR_NAME_Y);
  }

  if (pendingId3Changed) {
    pendingId3Changed = false;

    M5Cardputer.Display.setFont(UI_FONT_SMALL);
    M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5Cardputer.Display.drawString(pendingId3, HDR_TEXT_X, HDR_TITLE_Y);
    M5Cardputer.Display.setFont(UI_FONT);
  }
}

// Streamtitel in Zeile HDR_TITLE_Y, in halber Schrift. Passt er nicht auf den
// Bildschirm, laeuft er einmal von rechts durch und bleibt danach links
// stehen. Zeichnet nebenbei die rote Trennlinie nach.
void drawStreamTitle() {
  if (uiOverlayActive()) return;

  M5Cardputer.Display.fillRect(
    0, HDR_RULE_Y, M5Cardputer.Display.width(), 1, TFT_RED);

  // Die halbe Schrift muss auch beim Ausmessen mit textWidth() stehen,
  // sonst passt die Laenge des Durchlaufs nicht zum Gezeichneten.
  M5Cardputer.Display.setFont(UI_FONT_SMALL);
  scrollStreamTitle();
  M5Cardputer.Display.setFont(UI_FONT);
}

// Stellt die Footerschrift ein und liefert die Oberkante der Footerzeile.
// Sie haengt unten: FOOTER_TEXT_DY Pixel Luft unter der Schrift.
static int footerTextY() {
  M5Cardputer.Display.setFont(FOOTER_FONT);
  return M5Cardputer.Display.height() - FOOTER_TEXT_DY
       - M5Cardputer.Display.fontHeight();
}

// Feldstaerke des WLAN als Prozentwert 1..100, 0 heisst "keine Verbindung".
// Umgerechnet aus dem RSSI ueber den brauchbaren Bereich -100 dBm (nichts
// mehr zu holen) bis -50 dBm (Vollausschlag) - die uebliche Faustformel.
static int wifiPercent() {
  if (WiFi.status() != WL_CONNECTED) return 0;

  int pct = 2 * ((int)WiFi.RSSI() + 100);

  if (pct < 1)   pct = 1;
  if (pct > 100) pct = 100;
  return pct;
}

// WLAN-Symbol: zwei Boegen und der Punkt darunter, von Hand gesetzt - bei
// elf mal acht Pixeln bringt gerechnete Kreisgeometrie nur Matsch. Ein Bit
// je Pixel, hoechstwertiges Bit ist die linke Spalte.
static void drawWifiIcon(int x0, int y0, uint16_t col) {
  static const uint16_t rows[WIFI_ICON_H] = {
    0x1FC,   // ..#######..   aeusserer Bogen
    0x202,   // .#.......#.
    0x401,   // #.........#
    0x070,   // ....###....   innerer Bogen
    0x088,   // ...#...#...
    0x000,   // ...........
    0x020,   // .....#.....   Punkt
    0x000    // ...........
  };

  for (int y = 0; y < WIFI_ICON_H; y++) {
    for (int x = 0; x < WIFI_ICON_W; x++) {
      if (rows[y] & (1 << (WIFI_ICON_W - 1 - x)))
        M5Cardputer.Display.drawPixel(x0 + x, y0 + y, col);
    }
  }
}

// Mittleres Footer-Feld: WLAN-Symbol und Feldstaerke in Prozent. Holt den
// Wert hoechstens alle WIFI_UPDATE_MS und zeichnet nur bei Aenderung - das
// RSSI zappelt sonst und der Footer flackert.
void drawWifi() {
  if (streamState == STREAM_FAILED) return;   // dann steht dort die Meldung

  static unsigned long lastPoll = 0;

  if (lastDrawnWifiPct >= 0 && millis() - lastPoll < WIFI_UPDATE_MS) return;
  lastPoll = millis();

  const int pct = wifiPercent();
  if (pct == lastDrawnWifiPct) return;
  lastDrawnWifiPct = pct;

  const int y  = M5Cardputer.Display.height() - FOOTER_HEIGHT;
  const int ty = footerTextY();

  M5Cardputer.Display.fillRect(
    FOOTER_WIFI_X, y, FOOTER_RATE_X - FOOTER_WIFI_X, FOOTER_HEIGHT, TFT_BLACK);

  char buf[8];
  if (pct > 0) snprintf(buf, sizeof(buf), "%d%%", pct);
  else         snprintf(buf, sizeof(buf), "--");

  // Symbol und Wert als ein Block mittig ueber dem Feld ausrichten.
  const int blockW = WIFI_ICON_W + WIFI_GAP + M5Cardputer.Display.textWidth(buf);
  const int x0     = (FOOTER_WIFI_X + FOOTER_RATE_X - blockW) / 2;

  const uint16_t col = (pct >= WIFI_PCT_GREEN)  ? TFT_GREEN
                     : (pct >= WIFI_PCT_YELLOW) ? TFT_YELLOW
                                                : TFT_RED;

  drawWifiIcon(x0, ty, col);

  M5Cardputer.Display.setTextColor(FOOTER_TEXT_COLOR, TFT_BLACK);
  M5Cardputer.Display.drawString(buf, x0 + WIFI_ICON_W + WIFI_GAP, ty);

  M5Cardputer.Display.setFont(UI_FONT);
}

// Rechtes Footer-Feld: Bitrate des laufenden Streams. Zeichnet nur neu,
// wenn sich der Wert geaendert hat, sonst flackert es bei VBR-Streams.
void drawBitrate() {
  if (streamState == STREAM_FAILED) return;

  int kbit = (streamState == STREAM_OK) ? (audio.getBitRate() / 1000) : 0;

  if (kbit == lastDrawnKbit) return;
  lastDrawnKbit = kbit;

  const int y  = M5Cardputer.Display.height() - FOOTER_HEIGHT;
  const int ty = footerTextY();

  M5Cardputer.Display.fillRect(
    FOOTER_RATE_X, y, M5Cardputer.Display.width() - FOOTER_RATE_X,
    FOOTER_HEIGHT, TFT_BLACK);

  if (kbit > 0) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d kbit/s", kbit);

    M5Cardputer.Display.setTextColor(FOOTER_TEXT_COLOR, TFT_BLACK);
    M5Cardputer.Display.drawRightString(
      buf, M5Cardputer.Display.width() - UI_MARGIN, ty);
  }

  M5Cardputer.Display.setFont(UI_FONT);
}

// Footer. Im Normalfall Kennung, WLAN und Bitrate, bei totem Stream
// stattdessen eine orange Meldung ueber die volle Breite.
void drawFooter() {
  if (uiOverlayActive()) return;   // nicht in ein offenes Fenster malen

  int y  = M5Cardputer.Display.height() - FOOTER_HEIGHT;
  int ty = footerTextY();

  M5Cardputer.Display.fillRect(0, y, 240, FOOTER_HEIGHT, TFT_BLACK);

  // Bereich ist leer, beide Felder muessen sich neu zeichnen.
  lastDrawnKbit = -1;
  lastDrawnWifiPct = -1;

  if (streamState == STREAM_FAILED) {
    M5Cardputer.Display.setTextColor(TFT_ORANGE, TFT_BLACK);
    M5Cardputer.Display.drawCentreString("Stream unavailable", 120, ty);
  } else {
    M5Cardputer.Display.setTextColor(FOOTER_TEXT_COLOR, TFT_BLACK);
    M5Cardputer.Display.drawString(FOOTER_NAME, UI_MARGIN, ty);
    drawWifi();
    drawBitrate();
  }

  M5Cardputer.Display.setFont(UI_FONT);
  M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
}

// B schaltet die Helligkeitsstufen der Reihe nach durch, Stufe 0 = aus.
void toggleBrightness() {
  currentBrightnessIndex++;
  if (currentBrightnessIndex >= 5) {
    currentBrightnessIndex = 0;
  }

  uint8_t brightness = brightnessLevels[currentBrightnessIndex];
  M5Cardputer.Display.setBrightness(brightness);

}

// Senderliste als Vollbild. Der sichtbare Ausschnitt folgt der Auswahl,
// die markierte Zeile wird invertiert dargestellt.
void drawStationMenu() {
  M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);

  M5Cardputer.Display.drawCentreString("Select station:", 120, 0);
  M5Cardputer.Display.fillRect(0, 13, 240, 1, TFT_RED);

  // Auswahl moeglichst mittig halten, an den Listenenden anschlagen.
  int start = menuIndex - MENU_VISIBLE / 2;
  if (start < 0) start = 0;
  if (start > (int)numStations - MENU_VISIBLE)
    start = max(0, (int)numStations - MENU_VISIBLE);

  const int listTop = MENU_TOP;
  const int listHeight = MENU_VISIBLE * MENU_LINE_H + 30;
  M5Cardputer.Display.fillRect(0, listTop, 240, listHeight, TFT_BLACK);

  for (int i = 0; i < MENU_VISIBLE; i++) {
    int idx = start + i;
    if (idx >= numStations) break;

    int y = MENU_TOP + i * MENU_LINE_H + 8;

    if (idx == menuIndex) {
      M5Cardputer.Display.fillRect(0, y, 240, MENU_LINE_H, TFT_WHITE);
      M5Cardputer.Display.setTextColor(TFT_BLACK, TFT_WHITE);
    } else {
      M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    }

    int textY = y + (MENU_LINE_H - M5Cardputer.Display.fontHeight()) / 2;
    M5Cardputer.Display.drawString(stations[idx].name, UI_MARGIN, textY);
  }
}

// Menue oeffnen: Bildschirm leeren, Auswahl auf den laufenden Sender setzen.
void openStationMenu() {
  stationMenuActive = true;
  menuIndex = curStation;

  M5Cardputer.Display.fillRect(0, 0, 240, 135, TFT_BLACK);

  drawStationMenu();
}
