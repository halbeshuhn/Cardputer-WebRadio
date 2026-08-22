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

#include <SD.h>              // vor M5Cardputer.h, sonst kennt M5GFX
#include "M5Cardputer.h"
#include "NokiaFC.h"         // Nokia-Pixelschrift, aus nokiafc22.ttf erzeugt
#include "NotoSans.h"        // Streamtitel, aus NotoSans.ttf erzeugt
#include "LogoDummy.h"       // Ersatzlogo, roh, aus tools/make_logo_dummy.py

// Nur fuer die Diagnose hinter DEBUG_SERIAL gebraucht. Ohne #if, weil
// DEBUG_SERIAL erst weiter unten gesetzt wird - hier oben waere es noch 0.
#include <esp_heap_caps.h>   // heap_caps_check_integrity_all()
#include <esp_system.h>      // esp_reset_reason()
#include <esp_partition.h>   // esp_partition_t

// esp_ota_ops.h liegt nicht im Suchpfad des Sketches, die Funktion selbst ist
// aber gelinkt (Komponente app_update). Deshalb hier von Hand angemeldet.
extern "C" const esp_partition_t* esp_ota_get_running_partition(void);
#include "Lang.h"            // Texttabelle deutsch/englisch, T() und uiLang
#include "CardWifiSetup.h"   // WLAN-Einrichtung und -Speicher, eigene Datei
// Streamdecoder. AudioCompat.h bildet die Schnittstelle von ESP32-audioI2S
// auf ESP8266Audio ab; fuer den alten Stand hier wieder <Audio.h> einsetzen.
#include "AudioCompat.h"

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

// ---------------------------------------------------------------------------
// Dauertopf-Versuch (18.8.2026, Eckis Entwurf)
//
// Einmal beim Start eine TLS-Verbindung aufbauen und den Client **fuer immer
// leben lassen**. mbedTLS legt beim Handschlag seine zwei Puffer an (in_buf
// und out_buf); solange niemand stop() ruft, bleiben sie belegt. Sie liegen
// dann unten im noch unversehrten Heap statt irgendwo in der Mitte.
//
// Wichtig: "verbinden und wieder schliessen" bringt gar nichts -
// NetworkClientSecure::stop() ruft mbedtls_ssl_free() (ssl_client.cpp:380)
// und gibt beide Puffer sofort zurueck. Der Client MUSS leben bleiben.
//
// Dass der Server die Verbindung nach einer Weile von sich aus zumacht,
// stoert nicht: die Puffer haengen am SSL-Kontext, nicht am Socket.
//
// Nebenzweck: das ist die erste Gelegenheit, den Asymmetrie-Schalter am
// Geraet nachzumessen. Mit unseren eigenen Bibliotheken (IN 16384, OUT 2048)
// sollten beim Handschlag rund 19 KB fallen, mit den originalen (16384 +
// 16384) waeren es rund 33 KB.
//
// Auf 0 setzen, dann faellt alles davon weg.
#define TLS_DAUERTOPF  1

// Seit dem 19.8.2026 unbedingt: der Logopfad holt Bilder ueber TLS, wenn der
// Server darauf besteht. Frueher hing der Einschluss am Dauertopf.
#include <WiFiClientSecure.h>
#define DEBUG_HEAP_MS  1500   // Abstand der Heap-Zeilen
#define DEBUG_CHECK_MS  250   // Abstand der Heappruefung, siehe debugHeapCheck()

// ---------------------------------------------------------------------------
// Eingangspuffer der Audio-Bibliothek, in Bytes.
//
// Die Bibliothek nimmt von sich aus 16000 und haelt davon 2048 als Reserve
// zurueck - 13.951 nutzbare, so meldet sie es beim Start als
// "inputBufferSize". Das sind 0,87 Sekunden Vorrat bei 128 kbit/s, 0,44 bei
// 256 und 0,35 bei 320. Ohne PSRAM liegt er im internen RAM.
//
// Kleiner heisst weniger Vorrat gegen Netzschwankungen, aber mehr Heap fuer
// die Decoder - und darum geht es hier. Der AAC-Decoder fordert je
// Stereo-Rahmen 22.006 Bytes am Stueck an (sizeof(element) in neaacdec.cpp,
// aus den Debug-Informationen der Firmware ausgelesen). Beim Absturz am
// 9.8.2026 war der groesste freie Block 16.372 Bytes gross, es fehlten also
// 5.634. Mit 8000 statt 16000 werden 8.000 Bytes frei, mehr als der
// Fehlbetrag. Ob daraus auch ein zusammenhaengender Block dieser Groesse
// wird, ist die eigentliche Frage - der Heap zerfaellt im Betrieb.
//
// 0 laesst die Vorgabe der Bibliothek stehen, das ist der Weg zurueck.
// Gesetzt werden muss der Wert, bevor der Puffer angelegt wird - das
// geschieht beim ersten Verbinden, also vor Playfile() in setup().
// ---------------------------------------------------------------------------
// 64.000 statt der urspruenglichen 12.000, seit dem 18.8.2026. Anlass war
// Eckis Autotest am Handy-Hotspot: 12.000 sind 300 ms bei 320 kBit, jede
// Funkdelle darueber hinaus gab ein Loch. 64.000 sind **1,6 s bei 320 kBit**
// und 4,0 s bei 128.
//
// Nur moeglich, weil der Puffer jetzt dynamisch ist (AudioCompat.h): als
// statisches Feld haette er dem PNG-Dekoder den Platz genommen.
//
// Warum 64.000 vertretbar sind, obwohl davon im Betrieb nur rund 11,7 KB
// groesster Block uebrig bleiben: bei laufendem Ton fordert **niemand** mehr
// als 2.048 Bytes am Stueck an. Nachgezaehlt am 18.8.2026 -
//   Decoder      0      (acCodecMem[30720] ist ein statisches Feld)
//   Spektrum     2.048  (groesster von vier Puffern, nur bei visMode Spektrum)
//   WLAN-Pakete  ~1.600 (32 dynamische Empfangspuffer)
//   lwIP-Fenster 5.760  aber in Ketten zu je MSS 1.436, nichts am Stueck
// Die teuren Posten - PNG-Dekoder 43.768, Online-Tabellen, TLS-Handschlag -
// laufen alle bei abgebautem Audio.
//
// Die Grenze liegt kurz darueber: bei 72.000 blieben nur noch rund 3,7 KB,
// und dann kommen die 1,6-KB-Paketpuffer ins Gedraenge.
//
// **56.000 statt 64.000, seit dem 19.8.2026.** Nicht aus Vorsicht, sondern
// weil 64.000 nicht mehr zu haben sind: der groesste Block faellt im Betrieb
// einmalig von 86.004 auf 63.476 und bleibt dort - 524 Bytes unter der
// Wunschgroesse. Jede Anforderung landete deshalb in der Rueckfallkette und
// endete bei 48.000 oder 36.000. 56.000 sind 1,4 s bei 320 kBit und 3,5 s bei
// 128, und sie passen unter die Decke.
//
// Woher die fehlenden rund 24 KB kommen, ist ungeklaert: einmal fiel die
// Decke nach einem http-Sender, im anderen Lauf nach einem https-Sender.
// Kommen sie zurueck, darf diese Zahl wieder hoch.
#define AUDIO_INBUF_BYTES 56000

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
#define HDR_RULE_Y     37   // rote Trennlinie

// Der Streamtitel stand bis v3.1.1 als zweite Kopfzeile ueber der Linie und
// lief als Laufschrift durch. Jetzt steht er unter der Linie, in der leeren
// Anzeige (VIS_OFF), umgebrochen und stehend. Das kostet nichts an Platz,
// den sonst niemand nutzt, und macht das Lesen leichter: die Zeile wandert
// nicht mehr, und die Schrift ist die des VU-Skalenlineals - Font0 in
// einfacher Groesse, also halb so gross wie die Menueschrift.
//
// Was zwischen Linie und Footer nicht mehr hinpasst, faellt weg. Bei sieben
// Zeilen zu 38 Zeichen sind das 266 Zeichen; currentStreamTitle fasst 127.
#define TITLE_Y        42   // Oberkante der ersten Zeile, 5 px unter der Linie
#define TITLE_LINE_H   19   // Tinte 18 px seit den Akzenten, dazu 1 px Luft
#define TITLE_LINES     4   // 42 + 3*19 = 99, die letzte Zeile endet bei 117

// Der Ton hat Vorrang: unterhalb dieses Fuellstands wird nicht gezeichnet.
// Der Titel darf ruhig ein paar Sekunden spaeter erscheinen, die Wiedergabe
// darf nicht stocken.
#define TITLE_MIN_FILL_PCT 60
#define TITLE_FONT     (&NotoSans)

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

// Der Ladestand als Zahl links daneben. Font0, drei Ziffern zu 6 px; die
// Lautstaerkeskala endet bei x=184, die Zahl faengt fruehestens bei 191 an.
#define BATT_PCT_GAP    2    // Luft zwischen Zahl und Gehaeuse
#define BATT_PCT_W     18    // Platz fuer drei Ziffern
#define BATT_PCT_Y      4    // Oberkante, ein Pixel unter der Mitte

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
// is_https=false stand hier bis zum 14.8.2026 und ist weg - endgueltig.
//
// Der urspruengliche Grund war echt: mbedTLS legte zwei Puffer zu je 16 KB
// an, wo 13 bis 23 KB frei waren. Danach kam ein Jahr Speicherarbeit in vier
// Tagen - SBR-Ausbau, eigene ESP-IDF-Bibliotheken, asymmetrische TLS-Puffer
// (IN 16384 / OUT 2048), Logos und Listen nur bei stillem Ton.
//
// Seit dem 19.8.2026 spricht das Geraet **wirklich** https: die Adresse geht
// unangetastet an AudioCompat.h, und AudioFileSourceICYStream legt bei https
// einen NetworkClientSecure an (library-patch/http-tls). Der Zwischenschritt
// vom 14.8. - jede https-Adresse auf http umschreiben - ist damit
// abgeraeumt, mitsamt der Fusszeile "https not work".
//
// Aussortiert wird hier weiterhin nichts selbst - das wuerde die
// Seitenrechnung ueber offset zerlegen.
//
// Einen codec-Filter gibt es seit v3.1.0 nicht mehr. Bis dahin stand hier
// codec=MP3, weil AAC das Geraet reproduzierbar zum Neustart brachte - nicht
// wegen AAC, sondern wegen des faad2-Decoders der Bibliothek. Seit dem
// Ruecktausch auf Helix spielt AAC; siehe library-patch/aac-helix-swap.md.
// Der Filter kostete nur Sender: in Australien waren 42 % der Treffer AAC,
// in Polen 33 %, in Deutschland 13 % (gemessen am 8.8.2026).
// codec=MP3 stand hier bis v3.0.1 und hat AAC-Sender ausgesperrt, weil der
// damalige faad2-Decoder daran abstuerzte. Seit dem Rücktausch auf Helix
// spielt AAC, der Filter kostete also nur Sender - in Australien 42 Prozent
// der Treffer. Mit is_https ist es seit dem 14.8.2026 genauso ausgegangen.
// Sortiert wird nach Beliebtheit. Alphabetisch war es eine Weile, das ist
// berechenbar - aber Deutschland hat 2678 Sender, und HR1 lag damit auf
// Seite 86. Nach Klickzahl stehen die bekannten Sender eines Landes auf der
// ersten Seite, und das gilt in jedem Land, ohne dass jemand etwas tippen
// muss. Wer einen bestimmten sucht, nimmt die Suchleiste.
#define RB_PATH_BASE "/json/stations/search?hidebroken=true" \
                     "&order=clickcount&reverse=true"

#define RB_AGENT      "M5CardputerWebRadio/1.0"  // verlangt der Dienst
#define RB_TIMEOUT_MS 8000

// Eigene Kennungen fuer die Fehleranzeige. Werte > 0 sind der HTTP-Status
// des Servers, alles andere ist hier drin entstanden.
#define RB_ERR_NO_WIFI   -1
#define RB_ERR_CONNECT   -2
#define RB_ERR_TIMEOUT   -3
#define RB_ERR_NO_STATUS -4
#define RB_ERR_NO_TABLE  -5   // Speicher fuer die Tabellen reichte nicht
// 38 lateinische Zeichen zu 6 px passen mit Rand auf die Breite. In UTF-8
// braucht ein chinesisches Zeichen aber drei Bytes bei 14 px Breite - 17
// davon fuellen die Zeile, also 51 Bytes plus Abschluss. 64 sind gerundet
// und kosten bei elf Zeilen 286 Bytes Heap.
#define RB_NAME_LEN   64
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
struct RbCountry {
  const char* name;   // Anzeige, englisch
  const char* code;   // ISO 3166-1 alpha-2, geht als countrycode an den Dienst
};

// Die Regionsebene ist am 12.8.2026 entfallen. Grund: das Verzeichnis fuehrt
// das Bundesland bei **mehr als der Haelfte** der deutschen Sender gar nicht
// (1443 von 2678 leer) und schreibt es sonst uneinheitlich - "Bayern" neben
// "Bavaria", "Sachsen" neben "Saxony". Wer ueber die Region ging, sah immer
// nur einen Ausschnitt und vermisste bekannte Sender; HR1 fehlte so in
// Hessen. In den USA dasselbe Bild, 39 Prozent ohne Angabe.
//
// Statt eines Filters, der mehr verbirgt als er zeigt, gibt es jetzt die
// Suche in der Senderliste.

// Alle Laender, die radio-browser.info kennt und in denen mindestens ein
// Sender steht - Stand 12.8.2026, alphabetisch. Vorher waren es 42, ausgewaehlt
// nach Bestandszahlen; wer sein Land nicht fand, war ausgesperrt (Meldung #1
// im Repository: Taiwan fehlte). Gefunden wird jetzt ueber das Suchfeld auf
// der Laenderseite, nicht durch Blaettern - bei 241 Eintraegen waeren das
// sonst 22 Seiten.
//
// Die Namen stehen bewusst **englisch** und nicht in Lang.h: ein Land heisst
// in jeder Oberflaeche gleich. Wo der Dienst amtlich ausschreibt, steht hier
// der gebraeuchliche Kurzname - "United Kingdom" statt "The United Kingdom Of
// Great Britain And Northern Ireland". Laengster Name: 27 Zeichen.
//
// name ist die Anzeige, code geht als countrycode an den Dienst (ISO 3166-1
// alpha-2). Nur Deutschland und die USA haben Regionstabellen; bei allen
// anderen steht NULL, dann ueberspringt rbEnter() die Regionsebene.
//
// Achtung: die Bestandszahlen des Dienstes gelten fuer alle Sender, das Geraet
// zeigt nur die spielbaren. Der alte https-Filter (DE 6122->2674) ist seit dem
// 14.8.2026 weg, uebrig bleibt hidebroken - in kleinen Laendern kann die Liste
// trotzdem duenn ausfallen.
static const RbCountry rbCountries[] = {
  { "Afghanistan",                 "AF", },
  { "Aland Islands",               "AX", },
  { "Albania",                     "AL", },
  { "Algeria",                     "DZ", },
  { "American Samoa",              "AS", },
  { "Andorra",                     "AD", },
  { "Angola",                      "AO", },
  { "Anguilla",                    "AI", },
  { "Antarctica",                  "AQ", },
  { "Antigua and Barbuda",         "AG", },
  { "Argentina",                   "AR", },
  { "Armenia",                     "AM", },
  { "Aruba",                       "AW", },
  { "Australia",                   "AU", },
  { "Austria",                     "AT", },
  { "Azerbaijan",                  "AZ", },
  { "Bahamas",                     "BS", },
  { "Bahrain",                     "BH", },
  { "Bangladesh",                  "BD", },
  { "Barbados",                    "BB", },
  { "Belarus",                     "BY", },
  { "Belgium",                     "BE", },
  { "Belize",                      "BZ", },
  { "Benin",                       "BJ", },
  { "Bermuda",                     "BM", },
  { "Bhutan",                      "BT", },
  { "Bolivia",                     "BO", },
  { "Bosnia and Herzegovina",      "BA", },
  { "Botswana",                    "BW", },
  { "Brazil",                      "BR", },
  { "British Indian Ocean Terr.",  "IO", },
  { "British Virgin Islands",      "VG", },
  { "Brunei",                      "BN", },
  { "Bulgaria",                    "BG", },
  { "Burkina Faso",                "BF", },
  { "Burundi",                     "BI", },
  { "Cabo Verde",                  "CV", },
  { "Cambodia",                    "KH", },
  { "Cameroon",                    "CM", },
  { "Canada",                      "CA", },
  { "Caribbean Netherlands",       "BQ", },
  { "Cayman Islands",              "KY", },
  { "Central African Rep.",        "CF", },
  { "Chad",                        "TD", },
  { "Chile",                       "CL", },
  { "China",                       "CN", },
  { "Christmas Island",            "CX", },
  { "Cocos Islands",               "CC", },
  { "Colombia",                    "CO", },
  { "Comoros",                     "KM", },
  { "Congo (Brazzaville)",         "CG", },
  { "Congo (Kinshasa)",            "CD", },
  { "Cook Islands",                "CK", },
  { "Costa Rica",                  "CR", },
  { "Cote d'Ivoire",               "CI", },
  { "Croatia",                     "HR", },
  { "Cuba",                        "CU", },
  { "Curacao",                     "CW", },
  { "Cyprus",                      "CY", },
  { "Czechia",                     "CZ", },
  { "Denmark",                     "DK", },
  { "Djibouti",                    "DJ", },
  { "Dominica",                    "DM", },
  { "Dominican Republic",          "DO", },
  { "Dutch Part Sint Maarten",     "SX", },
  { "Ecuador",                     "EC", },
  { "Egypt",                       "EG", },
  { "El Salvador",                 "SV", },
  { "Equatorial Guinea",           "GQ", },
  { "Eritrea",                     "ER", },
  { "Estonia",                     "EE", },
  { "Eswatini",                    "SZ", },
  { "Ethiopia",                    "ET", },
  { "Falkland Islands",            "FK", },
  { "Faroe Islands",               "FO", },
  { "Fiji",                        "FJ", },
  { "Finland",                     "FI", },
  { "France",                      "FR", },
  { "French Guiana",               "GF", },
  { "French Polynesia",            "PF", },
  { "French Southern Territories", "TF", },
  { "Gabon",                       "GA", },
  { "Gambia",                      "GM", },
  { "Georgia",                     "GE", },
  { "Germany",                   "DE", },
  { "Ghana",                       "GH", },
  { "Gibraltar",                   "GI", },
  { "Greece",                      "GR", },
  { "Greenland",                   "GL", },
  { "Grenada",                     "GD", },
  { "Guadeloupe",                  "GP", },
  { "Guam",                        "GU", },
  { "Guatemala",                   "GT", },
  { "Guernsey",                    "GG", },
  { "Guinea",                      "GN", },
  { "Guinea Bissau",               "GW", },
  { "Guyana",                      "GY", },
  { "Haiti",                       "HT", },
  { "Honduras",                    "HN", },
  { "Hong Kong",                   "HK", },
  { "Hungary",                     "HU", },
  { "Iceland",                     "IS", },
  { "India",                       "IN", },
  { "Indonesia",                   "ID", },
  { "Iran",                        "IR", },
  { "Iraq",                        "IQ", },
  { "Ireland",                     "IE", },
  { "Isle Of Man",                 "IM", },
  { "Israel",                      "IL", },
  { "Italy",                       "IT", },
  { "Jamaica",                     "JM", },
  { "Japan",                       "JP", },
  { "Jersey",                      "JE", },
  { "Jordan",                      "JO", },
  { "Kazakhstan",                  "KZ", },
  { "Kenya",                       "KE", },
  { "Kiribati",                    "KI", },
  { "Kosovo",                      "XK", },
  { "Kuwait",                      "KW", },
  { "Kyrgyzstan",                  "KG", },
  { "Laos",                        "LA", },
  { "Latvia",                      "LV", },
  { "Lebanon",                     "LB", },
  { "Lesotho",                     "LS", },
  { "Liberia",                     "LR", },
  { "Libya",                       "LY", },
  { "Liechtenstein",               "LI", },
  { "Lithuania",                   "LT", },
  { "Luxembourg",                  "LU", },
  { "Macao",                       "MO", },
  { "Madagascar",                  "MG", },
  { "Malawi",                      "MW", },
  { "Malaysia",                    "MY", },
  { "Maldives",                    "MV", },
  { "Mali",                        "ML", },
  { "Malta",                       "MT", },
  { "Marshall Islands",            "MH", },
  { "Martinique",                  "MQ", },
  { "Mauritania",                  "MR", },
  { "Mauritius",                   "MU", },
  { "Mayotte",                     "YT", },
  { "Mexico",                      "MX", },
  { "Micronesia",                  "FM", },
  { "Moldova",                     "MD", },
  { "Monaco",                      "MC", },
  { "Mongolia",                    "MN", },
  { "Montenegro",                  "ME", },
  { "Montserrat",                  "MS", },
  { "Morocco",                     "MA", },
  { "Mozambique",                  "MZ", },
  { "Myanmar",                     "MM", },
  { "Namibia",                     "NA", },
  { "Nauru",                       "NR", },
  { "Nepal",                       "NP", },
  { "Netherlands",                 "NL", },
  { "New Caledonia",               "NC", },
  { "New Zealand",                 "NZ", },
  { "Nicaragua",                   "NI", },
  { "Niger",                       "NE", },
  { "Nigeria",                     "NG", },
  { "Niue",                        "NU", },
  { "North Korea",                 "KP", },
  { "North Macedonia",             "MK", },
  { "Norway",                      "NO", },
  { "Oman",                        "OM", },
  { "Pakistan",                    "PK", },
  { "Palau",                       "PW", },
  { "Palestine",                   "PS", },
  { "Panama",                      "PA", },
  { "Papua New Guinea",            "PG", },
  { "Paraguay",                    "PY", },
  { "Peru",                        "PE", },
  { "Philippines",                 "PH", },
  { "Poland",                      "PL", },
  { "Portugal",                    "PT", },
  { "Puerto Rico",                 "PR", },
  { "Qatar",                       "QA", },
  { "Reunion",                     "RE", },
  { "Romania",                     "RO", },
  { "Russia",                      "RU", },
  { "Rwanda",                      "RW", },
  { "Saint Helena",                "SH", },
  { "Saint Kitts and Nevis",       "KN", },
  { "Saint Lucia",                 "LC", },
  { "Saint Martin",                "MF", },
  { "Saint Pierre & Miquelon",     "PM", },
  { "Saint Vincent",               "VC", },
  { "San Marino",                  "SM", },
  { "Sao Tome and Principe",       "ST", },
  { "Saudi Arabia",                "SA", },
  { "Senegal",                     "SN", },
  { "Serbia",                      "RS", },
  { "Seychelles",                  "SC", },
  { "Sierra Leone",                "SL", },
  { "Singapore",                   "SG", },
  { "Slovakia",                    "SK", },
  { "Slovenia",                    "SI", },
  { "Solomon Islands",             "SB", },
  { "Somalia",                     "SO", },
  { "South Africa",                "ZA", },
  { "South Korea",                 "KR", },
  { "South Sudan",                 "SS", },
  { "Spain",                       "ES", },
  { "Sri Lanka",                   "LK", },
  { "Sudan",                       "SD", },
  { "Suriname",                    "SR", },
  { "Svalbard and Jan Mayen",      "SJ", },
  { "Sweden",                      "SE", },
  { "Switzerland",                 "CH", },
  { "Syria",                       "SY", },
  { "Taiwan",                      "TW", },
  { "Tajikistan",                  "TJ", },
  { "Tanzania",                    "TZ", },
  { "Thailand",                    "TH", },
  { "Timor-Leste",                 "TL", },
  { "Togo",                        "TG", },
  { "Tonga",                       "TO", },
  { "Trinidad and Tobago",         "TT", },
  { "Tunisia",                     "TN", },
  { "Turkey",                      "TR", },
  { "Turkmenistan",                "TM", },
  { "Turks and Caicos",            "TC", },
  { "Tuvalu",                      "TV", },
  { "Uganda",                      "UG", },
  { "Ukraine",                     "UA", },
  { "United Arab Emirates",        "AE", },
  { "United Kingdom",              "GB", },
  { "United States",             "US", },
  { "Uruguay",                     "UY", },
  { "US Minor Outlying Islands",   "UM", },
  { "US Virgin Islands",           "VI", },
  { "Uzbekistan",                  "UZ", },
  { "Vanuatu",                     "VU", },
  { "Vatican City",                "VA", },
  { "Venezuela",                   "VE", },
  { "Vietnam",                     "VN", },
  { "Wallis and Futuna",           "WF", },
  { "Yemen",                       "YE", },
  { "Zambia",                      "ZM", },
  { "Zimbabwe",                    "ZW", },
};

#define RB_COUNTRY_COUNT ((int)(sizeof(rbCountries) / sizeof(rbCountries[0])))

// Die drei Ebenen des Browsers.
enum RbPage { RB_PAGE_COUNTRY, RB_PAGE_STATIONS };

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

// ---------------------------------------------------------------------------
// Atem
//
// Eine Kurve, zwei Nutzer: das gewaehlte EQ-Band und die Lautstaerkeskala
// waehrend des Regelns. Ein Zyklus dauert UI_BREATH_MS, die Stufe kommt aus
// der Uhr und nicht aus einem Zaehler - so bleibt der Takt gleich, auch wenn
// ein Bild ausfaellt, weil der Decoder dazwischenfunkt.
//
// Kosinus in 32 Stufen, 0..255. Weich an beiden Enden; ein Dreieck haette an
// den Umkehrpunkten sichtbare Knicke.
// ---------------------------------------------------------------------------
#define UI_BREATH_MS 1200

static const uint8_t uiBreath[32] = {
    0,   2,  10,  21,  37,  57,  79, 103,
  127, 152, 176, 198, 218, 234, 245, 253,
  255, 253, 245, 234, 218, 198, 176, 152,
  128, 103,  79,  57,  37,  21,  10,   2
};

static inline uint8_t uiLerp(uint8_t a, uint8_t b, uint8_t t) {
  return (uint8_t)((int)a + (((int)b - (int)a) * (int)t) / 255);
}

static uint16_t uiMixColor(const uint8_t a[3], const uint8_t b[3], uint8_t t) {
  return M5Cardputer.Display.color565(uiLerp(a[0], b[0], t),
                                      uiLerp(a[1], b[1], t),
                                      uiLerp(a[2], b[2], t));
}

static inline uint8_t uiBreathLevel() {
  return uiBreath[(millis() % UI_BREATH_MS) * 32 / UI_BREATH_MS];
}

// ---------------------------------------------------------------------------
// Eine Zeile, die ausläuft
//
// Fuer Text, der breiter ist als sein Platz. Statt ihn abzuschneiden: er
// steht UI_LAUF_STILL_MS still, laeuft dann nach links aus, bis nur noch
// UI_LAUF_REST_CH Zeichen im Fenster stehen, blendet aus und faengt von vorn
// an. Passt der Text ohnehin, bleibt er einfach stehen.
//
// Zwei Nutzer: der Sendername in der Kopfzeile und die Adresse auf dem
// Info-Schirm. Der Zustand steckt in UiLauf, das Zeichnen bleibt beim
// jeweiligen Nutzer - die beiden malen mit verschiedenen Schriften an
// verschiedenen Stellen.
//
// Gezeichnet wird ohne Loeschen davor. Das hat frueher geflimmert; der Text
// deckt sich selbst, geraeumt wird nur, was rechts von ihm uebrigbleibt.
// Nach links begrenzt setClipRect() - sonst liefe der Text aus seinem Feld.
// ---------------------------------------------------------------------------
#define UI_LAUF_STILL_MS 3000  // Standzeit am Anfang
#define UI_LAUF_STEP_MS    25  // Takt des Laufs
#define UI_LAUF_STEP_PX     1  // also 40 px je Sekunde
#define UI_LAUF_REST_CH     6  // so viele Zeichen bleiben am Ende stehen
#define UI_LAUF_OUT_MS    250  // Ausblenden, wie die uebrigen Blenden

enum { UI_LAUF_STILL, UI_LAUF_ROLL, UI_LAUF_OUT };

// Die drei Schritte stehen als Methoden in der Struktur, nicht daneben: der
// Arduino-Vorlauf zieht Prototypen freier Funktionen an den Dateianfang, und
// dort waere UiLauf noch unbekannt.
struct UiLauf {
  uint8_t  phase    = UI_LAUF_STILL;
  uint32_t since    = 0;
  uint32_t lastStep = 0;
  int      scroll   = 0;
  int      weit     = 0;   // Strecke bis zum Ausblenden, 0 = steht still

  // Strecke messen und von vorn anfangen. Der Aufrufer hat die Schrift schon
  // gesetzt - der Sendername kann in der efont stehen, die Adresse nie.
  void reset(const char* text, int maxW) {
    phase    = UI_LAUF_STILL;
    since    = millis();
    lastStep = since;
    scroll   = 0;
    weit     = 0;

    if (!text || !text[0]) return;

    const int voll = M5Cardputer.Display.textWidth(text);
    if (voll <= maxW) return;

    // Die letzten Zeichen bleiben als Rest stehen. Bei Mehrbytezeichen faellt
    // er etwas laenger aus - er soll nur ungefaehr so breit sein.
    const size_t len = strlen(text);
    const char* rest = (len > UI_LAUF_REST_CH) ? text + len - UI_LAUF_REST_CH
                                               : text;
    const int w = voll - M5Cardputer.Display.textWidth(rest);
    if (w > 0) weit = w;
  }

  // Helligkeit, die zur laufenden Phase gehoert. Ohne sie wuerde ein
  // Neuzeichnen von aussen die Zeile mitten im Ausblenden auf voll setzen.
  uint8_t helligkeit() const {
    if (phase != UI_LAUF_OUT) return 255;
    const uint32_t dt = millis() - since;
    if (dt >= UI_LAUF_OUT_MS) return 255;
    return (uint8_t)(255 - dt * 255 / UI_LAUF_OUT_MS);
  }

  // Einen Schritt weiter. true heisst: neu zeichnen.
  bool tick(uint8_t& hell) {
    hell = 255;
    if (weit <= 0) return false;

    const uint32_t jetzt = millis();

    if (phase == UI_LAUF_STILL) {
      if (jetzt - since < UI_LAUF_STILL_MS) return false;
      phase    = UI_LAUF_ROLL;
      lastStep = jetzt;
      return false;
    }

    if (phase == UI_LAUF_ROLL) {
      if (jetzt - lastStep < UI_LAUF_STEP_MS) return false;
      lastStep = jetzt;

      scroll += UI_LAUF_STEP_PX;
      if (scroll >= weit) {
        scroll = weit;
        phase  = UI_LAUF_OUT;
        since  = jetzt;
      }
      return true;
    }

    const uint32_t dt = jetzt - since;
    if (dt >= UI_LAUF_OUT_MS) {
      scroll = 0;
      phase  = UI_LAUF_STILL;
      since  = jetzt;
      return true;
    }
    hell = (uint8_t)(255 - dt * 255 / UI_LAUF_OUT_MS);
    return true;
  }
};

// ---------------------------------------------------------------------------
// Die Lautstaerkeskala beim Regeln
//
// Waehrend geregelt wird und VOL_HALL_MS danach atmet der Ausschlag, und an
// seinen beiden vordersten Spalten stehen zwei helle Striche als Marke. Ruhig
// bleiben dabei die Randpixel links und rechts und die durchgehende Linie
// ganz unten - sie tragen die Skala, sie sollen nicht mitzappeln.
//
// Die Striche entstehen, indem die oberen beiden Punkte einer Spalte
// verbunden werden. Diese Verbindung haengt an der Nachhallstaerke, nicht am
// Atem: sonst zerfiele der Strich im dunkelsten Moment wieder in zwei Punkte.
// Nur seine Farbe atmet.
// ---------------------------------------------------------------------------
#define VOL_FADE_MS   250   // Nachlauf des Ausschlags, wie im EQ
#define VOL_HALL_MS  3000   // so lange atmet es nach dem letzten Druck weiter
#define VOL_REPEAT_DELAY_MS 500
#define VOL_REPEAT_RATE_MS   80

static const uint8_t VOL_C_ON [3] = { 120, 165, 210 };   // = UI_SCALE_ON
static const uint8_t VOL_C_MID[3] = { 180, 205, 235 };   // Atem des Balkens
static const uint8_t VOL_C_TOP[3] = { 255, 255, 255 };   // Atem der Marke

static float    volShown = -1.0f;    // angezeigter Ausschlag, laeuft nach
static float    volFrom  = 0.0f;
static uint32_t volMoveStart  = 0;
static uint32_t volLastChange = 0;   // letzter Tastendruck
static uint8_t  volBreathLast = 255;
static int      volMarkX[2] = { -1, -1 };   // wo die Striche zuletzt standen
static char     volHeldKey = 0;
static uint32_t volHeldNext = 0;

// Staerke des Nachhalls, 255 waehrend des Regelns, dann in VOL_FADE_MS heraus.
static uint8_t volAmp() {
  if (!volLastChange) return 0;
  const uint32_t seit = millis() - volLastChange;
  if (seit <= VOL_HALL_MS) return 255;
  if (seit <= VOL_HALL_MS + VOL_FADE_MS)
    return (uint8_t)(255 - (seit - VOL_HALL_MS) * 255 / VOL_FADE_MS);
  return 0;
}

// Helligkeitsstufen, die B der Reihe nach durchschaltet. Stufe 0 ist aus.
uint8_t brightnessLevels[5] = {0, 32, 64, 128, 255};
uint8_t currentBrightnessIndex = 4;

// Unterkante des Kopfbereichs. Alles darunter gehoert der Anzeige.
static int header_height = HDR_RULE_Y + 1;

Audio audio;

// Zustand des Systemmenues (schwebendes Fenster, BtnG0).
// Neue Eintraege einfach in die passende Liste - Fenster und Steuerung
// ziehen mit, nur die Behandlung in sysMenuSelect() muss dazu.
enum SysMenuPage { SYS_PAGE_MAIN, SYS_PAGE_WIFI, SYS_PAGE_LANG, SYS_PAGE_SLEEP };

// Das Hauptmenue wird bei jedem Oeffnen zusammengestellt: "Sender sichern"
// erscheint nur, wenn gerade ein Sender aus der Online-Liste laeuft.
enum SysItem { IT_WIFI, IT_ONLINE, IT_LOCAL, IT_SAVE, IT_SLEEP, IT_LANG,
               IT_CHARGE, IT_EXIT };

// Wie viele Zeilen das Fenster hoechstens zeigt. Die Hoehe rechnet sich als
// 2 * SYS_MENU_PAD + Zeilen * SYS_MENU_LINE_H, also 12 + 20n; bei 135 px
// Schirmhoehe sind sechs Zeilen (132 px) das Letzte, was hineingeht. Mit
// "Sender sichern" **und** "Sleeptime" sind es sieben Eintraege - deshalb
// wird ab hier gescrollt statt gewachsen.
// Vier volle Zeilen und eine halbe. Die angeschnittene Zeile ist Absicht:
// sie sagt dem Auge, dass es weitergeht - deutlicher als das Dreieck am
// Rand, das man uebersehen kann. 2*6 + 4*20 + 10 = 102 px, damit bleiben
// bei 135 px Schirm 33 px Luft.
#define SYS_MENU_ROWS    4
#define SYS_MENU_H       (2 * SYS_MENU_PAD + SYS_MENU_ROWS * SYS_MENU_LINE_H \
                          + SYS_MENU_LINE_H / 2)
#define SYS_MENU_INNER   (SYS_MENU_H - 2 * SYS_MENU_PAD)

// Gescrollt wird in Pixeln, nicht in Zeilen: 1 px alle 25 ms, derselbe Takt
// wie die auslaufenden Zeilen. sysMenuOff ist der gezeigte Stand, sysMenuZiel
// der angesteuerte - die Bewegung dazwischen macht updateSysMenu().
// Schneller als die Laufzeilen: dort liest man mit, hier will man ankommen.
//
// Die Schrittweite darf nicht groesser sein als der Streifen, den
// drawSysMenu() ueber und unter jeder Zeichenzelle raeumt (je 2 px bei
// Zeilenhoehe 20 und Schrift 16) - sonst bleiben Reste der vorigen Lage
// stehen, und genau das sah man als Flimmern. Schneller wird es deshalb
// ueber den Takt, nicht ueber die Weite: 2 px alle 8 ms sind 250 px/s.
#define SYS_MENU_STEP_PX 2
#define SYS_MENU_STEP_MS 8

static int      sysMenuOff  = 0;
static int      sysMenuZiel = 0;
static uint32_t sysMenuStep = 0;

// Sieben, nicht sechs: WLAN, Online, Lokal, Sichern, Sprache, Testlauf,
// Beenden. "Sichern" gibt es nur bei laufendem Online-Sender, "Testlauf" nur
// mit DEBUG_SERIAL - beide zusammen kamen bis zum 12.8.2026 nie vor, deshalb
// ist der Ueberlauf so lange niemandem aufgefallen. Er schrieb hinter das
// Feld, und dort beginnt das audio-Objekt: sein erstes Wort ist die
// Sprungtabelle, danach stuerzte der naechste audio.loop() ab.
#define SYS_MAIN_MAX 8
static SysItem sysMainIds[SYS_MAIN_MAX];
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

// ---------------------------------------------------------------------------
// Sleeptimer
//
// Drei Zustaende: aus, laeuft, eingeschlafen. Beim Einschlafen wird der
// Stream **angehalten** - das spart Netz und Strom, im Schlaf laeuft sonst
// die ganze Nacht ein Sender ins Leere.
//
// Dazu gehoert zwingend, dass der Streamwaechter stillgelegt wird: er sieht
// sonst einen Sender, der keine Daten mehr liefert, erklaert ihn nach
// STREAM_LOST_MS fuer tot und baut ihn nach 5, 10, 20, dann alle 40 Sekunden
// neu auf. Das Radio wuerde sich also selbst wieder aufwecken.
//
// Beim Wecken laeuft es rueckwaerts: Helligkeit zurueck, Playfile(), und
// damit ist auch der Waechter wieder in Betrieb - der Sender braucht dann
// wie nach jedem Wechsel ein paar Sekunden.
//
// Gemerkt wird die Helligkeitsstufe. Die Lautstaerke bleibt unangetastet:
// ohne Stream gibt es nichts stummzuschalten, und wer vor dem Einschlafen
// von Hand stumm war, ist es nach dem Wecken immer noch.
// ---------------------------------------------------------------------------
enum SleepState { SLEEP_OFF, SLEEP_RUNNING, SLEEP_ASLEEP };

SleepState sleepState   = SLEEP_OFF;
uint32_t   sleepDeadline = 0;     // millis(), nur bei SLEEP_RUNNING gueltig
uint8_t    sleepPrevBright = 0;   // Helligkeitsstufe vor dem Einschlafen
int        lastDrawnSleepMin = -1;  // was in der Fusszeile steht, -1 = nichts

// Der Einstellschirm: Stunden 0..12, Minuten in zwei Stellen. Drei Felder,
// zwischen denen links/rechts wandert - die Stunden haben einen gemeinsamen
// Strich unter beiden Ziffern, die Minuten je einen eigenen.
uint8_t sleepEditH   = 0;
uint8_t sleepEditM10 = 3;         // Vorgabe 00:30
uint8_t sleepEditM1  = 0;
int8_t  sleepEditField = 0;       // 0 = Stunden, 1 = Minutenzehner, 2 = Einer

// Sender aus dem Netz, ausserhalb der Liste von SD-Karte. Ist das gesetzt,
// spielt Playfile() diesen statt stations[curStation]. Jeder Senderwechsel
// mit den Pfeiltasten oder ueber die Senderliste hebt es wieder auf.
bool extStationActive = false;
char extStationName[MAX_NAME_LENGTH] = "";
char extStationUrl[RB_URL_LEN]       = "";   // Netz-URLs sind laenger als 100
char extStationIcon[RB_URL_LEN]      = "";   // Adresse des Senderlogos, kann leer sein

// Ergebnis und Messwerte des letzten Abrufs bei radio-browser.info.
// Vollbildliste der lokalen Sender (BtnG0 -> Lokale Liste).
bool localListActive = false;
int  localSel = 0;
int  localTop = 0;

bool     rbListActive = false;   // Browser offen, egal auf welcher Ebene
RbPage   rbPage       = RB_PAGE_COUNTRY;
int      rbCountryIdx = 0;

// rbRaw zaehlt, wie viele Datensaetze der Dienst geschickt hat - rbCount nur
// die, die uebrig blieben. Der Unterschied sind die uebersprungenen Doppel.
// Am Rohwert haengt das Blaettern: kamen weniger als angefragt, ist die Liste
// zu Ende. Vorher stand dort rbCount, und eine Seite mit Doppeln sah aus wie
// das Listenende - bei Taiwan war nach der ersten Seite Schluss.
int      rbRaw   = 0;

// Seitenverwaltung. Gezaehlt wird in Seiten, nicht in Datensaetzen - wie viele
// Datensaetze eine Seite gekostet hat, haengt vom HLS-Filter ab und ist von
// Seite zu Seite verschieden. rbPageOff merkt sich deshalb den Anfang jeder
// besuchten Seite; nur so kommt man exakt wieder zurueck.
#define RB_MAX_PAGES 300               // 300 * 11 = 3300 Sender, reicht ueberall
uint16_t rbPageOff[RB_MAX_PAGES] = { 0 };
int      rbPageNo     = 0;             // aktuelle Seite, 0-basiert
int      rbPageUsed   = 0;             // Datensaetze, die diese Seite kostete
int      rbTotalPages = -1;            // bekannt, sobald das Ende erreicht war

// Suchfeld der Laenderseite. Bei 241 Laendern waere Blaettern nicht zumutbar -
// 22 Seiten, ehe jemand sein Land findet. Getippt wird in die untersten zwei
// Zeilen, die dort sonst die Tastenhilfe tragen; die Liste zeigt nur noch,
// was passt. Gesucht wird ohne Ruecksicht auf Gross- und Kleinschreibung und
// irgendwo im Namen, damit "king" auch "United Kingdom" findet.
#define RB_FILTER_LEN 18
char     rbFilter[RB_FILTER_LEN] = "";

// Suchbegriff der Senderliste. Anders als bei den Laendern wird hier nicht
// oertlich gefiltert - der Begriff geht als &name= an den Dienst, denn im
// Geraet liegen immer nur elf Namen. Die Leiste erscheint erst mit dem ersten
// Buchstaben und verschwindet wieder, wenn der letzte geloescht ist; ihr
// Platz bleibt aber frei, damit die Liste nicht springt.
char     rbStFilter[RB_FILTER_LEN] = "";
uint8_t  rbFiltIdx[RB_COUNTRY_COUNT];   // Plaetze der passenden Laender
int      rbFiltCount = 0;
bool     rbCaretOn   = false;
uint32_t rbCaretMs   = 0;
int      rbOffset     = 0;       // Seitenanfang der Senderliste
// Namens- und URL-Tabelle, zusammen 4,4 KB. Wie die Analyzerpuffer nur
// vorhanden, solange sie gebraucht werden - also waehrend die Online-Liste
// offen ist. Zeiger auf Felder, die Zugriffe rbNames[i] bleiben dieselben.
char (*rbNames)[RB_NAME_LEN] = NULL;
char (*rbUrls)[RB_URL_LEN]   = NULL;
char (*rbIcons)[RB_URL_LEN]  = NULL;   // Logoadressen, Feld "favicon"
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
  return sysMenuActive || rbListActive || localListActive;
}

uint32_t rbHeapBefore = 0;   // freier Heap bei laufendem Stream
uint32_t rbHeapStopped = 0;  // ... nachdem der Stream angehalten wurde
uint32_t rbHeapAfter  = 0;
uint32_t rbHeapLow    = 0;   // kleinster freier Heap waehrend des Abrufs

// Hauptmenue neu zusammenstellen. extStationActive entscheidet, ob es den
// Eintrag zum Sichern gibt.
static void sysBuildMain() {
  int n = 0;

  // Der Zaehler wird bei jedem Eintrag geprueft. Lieber ein fehlender
  // Menuepunkt als ein Schreibzugriff hinter das Feld.
  #define SYS_ADD(id) do { if (n < SYS_MAIN_MAX) sysMainIds[n++] = (id); } while (0)

  SYS_ADD(IT_WIFI);
  SYS_ADD(IT_ONLINE);
  SYS_ADD(IT_LOCAL);
  if (extStationActive) SYS_ADD(IT_SAVE);
  SYS_ADD(IT_SLEEP);
  SYS_ADD(IT_LANG);
  // Der Testlauf stand hier bis zum 12.8.2026 - nur in Diagnosefassungen,
  // und genau er hat den Ueberlauf von sysMainIds ausgeloest. Ecki braucht
  // ihn nicht, also ist er aus dem Menue heraus. Der Code dahinter bleibt,
  // erreichbar ist er nicht mehr.
  SYS_ADD(IT_CHARGE);
  SYS_ADD(IT_EXIT);
  sysMainCount = n;

  #undef SYS_ADD
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
    // Derselbe Eintrag, zwei Beschriftungen: laeuft oder schlaeft der Timer,
    // ist er der Ausschalter.
    case IT_SLEEP:  return (sleepState == SLEEP_OFF) ? T(STR_MENU_SLEEP)
                                                     : T(STR_MENU_SLEEP_STOP);
    case IT_LANG:   return T(STR_MENU_LANG);
    case IT_CHARGE: return T(STR_MENU_CHARGE);
    default:        return T(STR_MENU_EXIT);
  }
}

// Streamtitel aus den ICY-Metadaten des Senders.
// Vier Zeilen zu etwa 17 Zeichen sind in UTF-8 bis zu 204 Bytes - deshalb
// 256 statt der frueheren 128, sonst waere ein chinesischer Titel nach der
// zweiten Zeile zu Ende.
char currentStreamTitle[256] = "";
bool streamTitleChanged = false;   // ein neuer Titel ist eingetroffen
bool titlePending = false;         // gezeichnet wird, sobald Luft dafuer ist

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

// Ein laufender Stream gilt als verloren, wenn so lange keine Daten mehr
// ankommen. Grosszuegiger als STREAM_TIMEOUT_MS: ein kurzer Aussetzer soll
// nicht gleich einen Neuaufbau ausloesen.
#define STREAM_LOST_MS      8000
#define STREAM_RETRY_MS     5000    // erster Wiederholversuch
#define STREAM_RETRY_MAX_MS 40000   // Obergrenze des wachsenden Abstands

enum StreamState { STREAM_CONNECTING, STREAM_OK, STREAM_FAILED };
StreamState streamState = STREAM_OK;
unsigned long streamStartMs = 0;
unsigned long streamAliveMs = 0;   // zuletzt Daten gesehen

// ---------------------------------------------------------------------------
// Testlauf
//
// Schaltet die lokale Liste im Takt durch und schreibt bei jedem Wechsel eine
// Zeile in die serielle Ausgabe. Zweck ist die Frage, die sich am 11.8.2026
// nicht beantworten liess: Geht bei jedem Senderwechsel Speicher verloren,
// oder zerfaellt der Heap nur? Bei 20 s je Sender sind das rund 180 Wechsel
// in der Stunde - ein Verlust von wenigen Kilobyte je Wechsel faellt dann
// unuebersehbar auf.
//
// Der haerteste Fall steckt in der Liste selbst: MP3 und AAC+ im Wechsel
// bedeuten jedes Mal anderer Decoder, andere Abtastrate, neuer I2S-Kanal.
// ---------------------------------------------------------------------------

unsigned long streamRetryAt = 0;   // naechster Wiederholversuch
uint8_t       streamRetries = 0;   // fuer den wachsenden Abstand

// ---------------------------------------------------------------------------
// Scheitert ein Stream, kann das zwei ganz verschiedene Ursachen haben: der
// Sender ist tot, oder der Speicher hat nicht gereicht. Im Footer soll das
// auseinandergehalten werden, sonst sucht man den Fehler beim Sender.
//
// Unterschieden wird am groessten zusammenhaengenden Block im Augenblick des
// Fehlschlags. Der AAC-Decoder (Helix) legt beim Oeffnen drei Bloecke an:
// PSInfoBase_t 19.172, ProgConfigElement_t mal 16 = 1.312 und AACDecInfo_t
// 96 Bytes, zusammen 20.580. Der groesste Einzelblock ist der erste - liegt
// darunter nichts Zusammenhaengendes mehr frei, kann AAC nicht starten.
//
// Die Bibliothek meldet den Grund nicht zurueck, sie liefert nur false.
// Deshalb wird hier gemessen statt gefragt. Die Zahlen stammen aus den
// Debug-Informationen der uebersetzten Firmware, nicht aus einer Schaetzung.
// ---------------------------------------------------------------------------
#define AAC_BLOCK_NEEDED 19172   // groesster Einzelblock des AAC-Decoders

bool streamFailedLowMem = false;  // Grund des letzten Fehlschlags
bool streamFailedPlaylist = false; // ... die URL war eine Wiedergabeliste
bool streamFailedHls = false;      // ... und zwar eine, die HLS liefert
// ... oder der Sender lief und hoerte auf zu liefern. Die Sammelsperre in
// AudioCompat.h hat AC_SAMMEL_TOT_MS lang auf Nachschub gewartet und
// aufgegeben - das ist etwas anderes als ein Sender, der nie antwortet.
bool streamFailedStalled = false;

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
#define VU_L_Y       59     // Oberkante linke Zeile
#define VU_SCALE_Y   74     // Skala in der Mitte
#define VU_R_Y       89     // Oberkante rechte Zeile

#define VU_DB_FLOOR  -30.0f // linkes Ende der Skala in dB
#define VU_GREEN_MAX      7 // Segmente 0..7 gruen
#define VU_YELLOW_MAX     9 // 8..9 gelb, darueber rot
#define VU_OFF_DIV       10 // Helligkeitsteiler der erloschenen LEDs

#define VU_FRAME_MS      40 // Bildrate der Balken
#define VU_PEAK_HOLD_MS 1500 // Haltezeit des Peak-Segments
#define VU_PEAK_FALL_MS   90 // danach faellt es ein Segment pro ... ms

// Reihenfolge bestimmt, was F durchschaltet.
enum VisMode { VIS_OFF, VIS_VU, VIS_VU_PEAK, VIS_SPECTRUM, VIS_EQ, VIS_INFO };
#define VIS_MODES 6
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
#define SPEC_BOTTOM_Y  102  // y der Grundlinie
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

// Streamtitel unter der roten Linie, umgebrochen und stehend. Nur in der
// leeren Anzeige - sobald VU oder Spektrum laufen, gehoert die Flaeche denen.
//
// Umbrochen wird am letzten Leerzeichen, das noch in die Zeile passt; ein
// einzelnes ueberlanges Wort wird hart getrennt. Font0 ist eine Festbreiten-
// schrift, die Zeichenbreite steht also nach einer einzigen Messung fest.
// ---------------------------------------------------------------------------
// Schriftwahl fuer fremde Zeichen
//
// Die Nokia-Pixelschrift kann ASCII, die Noto-Schrift geht bis 0xFF - fuer
// Griechisch, Kyrillisch oder CJK reicht beides nicht. M5GFX bringt dafuer
// efont mit, vier Auspraegungen, die sich in den Han-Zeichen unterscheiden.
// Welche gilt, entscheidet das gewaehlte Land: wer in Taiwan sucht, bekommt
// die traditionellen Formen, wer in Japan sucht, die japanischen.
//
// Sie liegen im Flash, nicht im RAM, und LovyanGFX baut jedes Zeichen auf dem
// Stapel - der Heap bleibt unberuehrt.
static const lgfx::IFont* uiUniFont = &fonts::efontCN_14;

static void uiSetUniFontForCountry(const char* code) {
  if (!code) return;

  if      (!strcmp(code, "TW") || !strcmp(code, "HK") || !strcmp(code, "MO"))
    uiUniFont = &fonts::efontTW_14;
  else if (!strcmp(code, "JP"))
    uiUniFont = &fonts::efontJA_14;
  else if (!strcmp(code, "KR") || !strcmp(code, "KP"))
    uiUniFont = &fonts::efontKR_14;
  else
    uiUniFont = &fonts::efontCN_14;
}

// Steht im Text ein Zeichen jenseits von Latin-1? Dann kann Noto es nicht.
static bool uiNeedsUnicode(const char* s) {
  for (const uint8_t* p = (const uint8_t*)s; *p; ) {
    if (*p < 0x80) { p++; continue; }
    if ((*p & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
      if ((((uint16_t)(*p & 0x1F)) << 6 | (p[1] & 0x3F)) > 0xFF) return true;
      p += 2;
      continue;
    }
    return true;                 // drei Bytes oder mehr: immer jenseits
  }
  return false;
}

// Ein abgeschnittener Name darf nicht mitten in einem UTF-8-Zeichen enden -
// die Schrift zeichnete sonst Unsinn. Die angefangene Folge faellt weg.
static void uiTrimUtf8(char* s) {
  int n = (int)strlen(s);
  while (n > 0) {
    const uint8_t c = (uint8_t)s[n - 1];
    if (c < 0x80) return;                       // sauberes Ende
    if ((c & 0xC0) == 0xC0) { s[n - 1] = 0; return; }   // Anfangsbyte: weg
    s[--n] = 0;                                 // Folgebyte: weiter zurueck
  }
}

// HTML-Zahlenverweise aufloesen: &#30334; wird zu 百, &#x767E; ebenso, dazu
// die fuenf benannten Verweise, die in Titeln vorkommen.
//
// Noetig, weil ICY-Metadaten keine Kodierung angeben und manche Sender sich
// damit behelfen, alles jenseits von ASCII als Verweis zu schreiben - der
// taiwanesische Sender 1766 etwa. Am 12.8.2026 an seinem Strom nachgesehen.
// Das Ergebnis ist nie laenger als die Vorlage, es wird an Ort und Stelle
// gearbeitet.
static void uiDecodeEntities(char* s) {
  char* w = s;

  for (char* r = s; *r; ) {
    if (*r != '&') { *w++ = *r++; continue; }

    // benannte Verweise
    struct { const char* txt; char zeichen; } fest[] = {
      { "&amp;", '&' }, { "&quot;", '"' }, { "&apos;", '\'' },
      { "&lt;", '<' },  { "&gt;", '>' },
    };
    bool getroffen = false;
    for (auto& f : fest) {
      const size_t n = strlen(f.txt);
      if (!strncmp(r, f.txt, n)) { *w++ = f.zeichen; r += n; getroffen = true; break; }
    }
    if (getroffen) continue;

    if (r[1] != '#') { *w++ = *r++; continue; }

    char* z = r + 2;
    uint32_t cp = 0;
    bool hex = (*z == 'x' || *z == 'X');
    if (hex) z++;

    const char* anfang = z;   // nur zum Vergleich
    while (*z && *z != ';') {
      uint32_t v;
      if      (*z >= '0' && *z <= '9') v = *z - '0';
      else if (hex && *z >= 'a' && *z <= 'f') v = *z - 'a' + 10;
      else if (hex && *z >= 'A' && *z <= 'F') v = *z - 'A' + 10;
      else break;
      cp = cp * (hex ? 16 : 10) + v;
      if (cp > 0x10FFFF) break;
      z++;
    }

    if (z == anfang || *z != ';' || cp == 0) { *w++ = *r++; continue; }

    if (cp < 0x80) {
      *w++ = (char)cp;
    } else if (cp < 0x800) {
      *w++ = (char)(0xC0 | (cp >> 6));
      *w++ = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      *w++ = (char)(0xE0 | (cp >> 12));
      *w++ = (char)(0x80 | ((cp >> 6) & 0x3F));
      *w++ = (char)(0x80 | (cp & 0x3F));
    } else {
      *w++ = '?';                 // jenseits der Grundebene, keine Schrift dafuer
    }
    r = z + 1;
  }
  *w = '\0';
}

// Ist der Text gueltiges UTF-8? ICY-Kopfzeilen nennen ihre Kodierung nicht,
// und taiwanesische Sender schicken oft Big5. Der Sender AM756 etwa meldet
// sich mit bc ea b4 f2 b0 a8 a4 bd a5 78 - in Big5 sein Name, als UTF-8
// gelesen nur Kaestchen, und das 0x78 am Ende eines Paares erscheint als
// einzelnes "x". Genau so stand es am 12.8.2026 auf dem Schirm.
//
// Solche Namen werden verworfen: der Name aus dem Verzeichnis steht schon da
// und ist richtig. Lieber der gute alte als ein falscher neuer.
static bool uiValidUtf8(const char* s) {
  for (const uint8_t* p = (const uint8_t*)s; *p; ) {
    if (*p < 0x80) { p++; continue; }

    int folge;
    if      ((*p & 0xE0) == 0xC0) folge = 1;
    else if ((*p & 0xF0) == 0xE0) folge = 2;
    else if ((*p & 0xF8) == 0xF0) folge = 3;
    else return false;                       // 0x80..0xBF oder 0xF8+ als Anfang

    for (int i = 1; i <= folge; i++) {
      if ((p[i] & 0xC0) != 0x80) return false;
    }
    p += folge + 1;
  }
  return true;
}

// Laenge eines UTF-8-Zeichens in Bytes. Nie mitten hineinschneiden.
static int uiCharLen(const char* p) {
  const uint8_t c = (uint8_t)*p;
  if (c < 0x80) return 1;
  if ((c & 0xE0) == 0xC0) return 2;
  if ((c & 0xF0) == 0xE0) return 3;
  if ((c & 0xF8) == 0xF0) return 4;
  return 1;
}

// Titel in fremder Schrift. CJK kennt keine Leerzeichen, deshalb wird hier
// zeichenweise umbrochen - an einem Leerzeichen aber bevorzugt, damit
// gemischte Titel nicht mitten im lateinischen Wort brechen.
static void drawTitleBlockUni() {
  M5Cardputer.Display.setFont(uiUniFont);
  M5Cardputer.Display.setTextColor(FOOTER_TEXT_COLOR, TFT_BLACK);

  const int maxW = M5Cardputer.Display.width() - 2 * UI_MARGIN;
  const char* p  = currentStreamTitle;

  char row[160];

  for (int line = 0; line < TITLE_LINES && *p; line++) {
    int  used  = 0;          // Bytes in row
    int  lastSpace = -1;     // Bytes bis zum letzten Leerzeichen
    const char* q = p;

    while (*q) {
      const int cl = uiCharLen(q);
      if (used + cl >= (int)sizeof(row)) break;

      memcpy(row + used, q, cl);
      row[used + cl] = '\0';

      if (M5Cardputer.Display.textWidth(row) > maxW) { row[used] = '\0'; break; }

      if (*q == ' ') lastSpace = used;
      used += cl;
      q    += cl;
    }

    if (*q && lastSpace > 0) {         // lieber am Wort trennen
      row[lastSpace] = '\0';
      used = lastSpace;
    }
    if (used == 0) break;              // ein Zeichen passt nicht: aufhoeren

    M5Cardputer.Display.drawString(row, HDR_TEXT_X, TITLE_Y + line * TITLE_LINE_H);

    p += used;
    while (*p == ' ') p++;
  }

  M5Cardputer.Display.setFont(UI_FONT);
  M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
}

void drawTitleBlock() {
  if (uiOverlayActive()) return;
  if (visMode != VIS_OFF) return;

  // Ton vor Anzeige. Ist der Vorpuffer schwach gefuellt, wird jetzt nicht
  // gezeichnet - der Merker holt es nach, sobald wieder Luft ist.
  if (audio.isRunning() && audio.bufferFillPct() < TITLE_MIN_FILL_PCT) {
    titlePending = true;
    return;
  }
  titlePending = false;

  M5Cardputer.Display.fillRect(
    0, TITLE_Y, M5Cardputer.Display.width(),
    M5Cardputer.Display.height() - FOOTER_HEIGHT - TITLE_Y, TFT_BLACK);

  if (currentStreamTitle[0] == '\0') return;

  // Fremde Schrift? Dann den eigenen Weg, ohne Latin-1 und mit Umbruch nach
  // UTF-8-Zeichen. Europaeische Titel laufen weiter ueber Noto - die Schrift
  // ist die schoenere, und an ihrem Umbruch stimmen die Masze.
  if (uiNeedsUnicode(currentStreamTitle)) { drawTitleBlockUni(); return; }

  // UTF-8 auf Latin-1 bringen. Die Schrift geht bis 0xFF, der Titel kommt
  // aber als UTF-8: ein "ue" steht dort als zwei Bytes. Ohne diesen Schritt
  // faende die Schrift keines davon, und der Umbruch zaehlte ein Zeichen als
  // zwei. Was sich nicht auf ein Byte abbilden laesst, wird zu '?'.
  char t[128];
  int n = 0;
  for (const uint8_t* p = (const uint8_t*)currentStreamTitle;
       *p && n < (int)sizeof(t) - 1; ) {
    uint8_t c = *p;
    if (c < 0x80) {
      t[n++] = (char)c; p++;
    } else if ((c & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
      uint16_t cp = ((c & 0x1F) << 6) | (p[1] & 0x3F);
      t[n++] = (cp <= 0xFF) ? (char)cp : '?';
      p += 2;
    } else {
      t[n++] = '?';
      p++;
      while ((*p & 0xC0) == 0x80) p++;   // Folgebytes ueberspringen
    }
  }
  t[n] = '\0';

  M5Cardputer.Display.setFont(TITLE_FONT);
  M5Cardputer.Display.setTextColor(FOOTER_TEXT_COLOR, TFT_BLACK);

  const int maxW = M5Cardputer.Display.width() - 2 * UI_MARGIN;

  // Wortweise umbrechen. Die erste Fassung mass fuer jedes angehaengte
  // Zeichen die ganze Zeile - bei 120 Zeichen mehrere tausend Abfragen,
  // mitten in der Hauptschleife. Jetzt wird je Wort einmal gemessen.
  char row[128];
  int start = 0;

  for (int line = 0; line < TITLE_LINES && start < n; line++) {
    int fit = -1;        // letztes Wortende, das noch passt
    int probe = start;

    while (probe < n) {
      int w = probe;
      while (w < n && t[w] != ' ') w++;      // Ende des naechsten Wortes

      int take = w - start;
      if (take > (int)sizeof(row) - 1) take = sizeof(row) - 1;
      memcpy(row, t + start, take);
      row[take] = '\0';

      if (M5Cardputer.Display.textWidth(row) > maxW) break;

      fit = w;
      probe = w;
      while (probe < n && t[probe] == ' ') probe++;
      if (probe == w) break;                 // kein Fortschritt mehr
    }

    int take;
    if (fit > start) {
      take = fit - start;                    // sauber am Wortende getrennt
    } else {
      // Ein einzelnes Wort ist breiter als die Zeile: hart trennen. Hier
      // zeichenweise, aber nur fuer dieses eine Wort.
      take = 1;
      while (start + take < n) {
        memcpy(row, t + start, take + 1);
        row[take + 1] = '\0';
        if (M5Cardputer.Display.textWidth(row) > maxW) break;
        take++;
      }
    }

    memcpy(row, t + start, take);
    row[take] = '\0';
    M5Cardputer.Display.drawString(row, HDR_TEXT_X,
                                   TITLE_Y + line * TITLE_LINE_H);

    start += take;
    while (start < n && t[start] == ' ') start++;
  }

  M5Cardputer.Display.setFont(UI_FONT);
}

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

  // Liegt eine Vollbildmaske darueber, wird nicht an feste Positionen
  // gezeichnet - sonst steht der Balken mitten in der Liste. Am 19.8.2026
  // beim Druck auf `o` aufgefallen: von den fuenf Darstellungen war diese
  // die einzige ohne die Abfrage (drawTitleBlock, EQ und Info-Schirm hatten
  // sie laengst).
  if (uiOverlayActive()) return;

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
  if (uiOverlayActive()) return;   // dasselbe wie beim VU-Meter

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

// ---------------------------------------------------------------------------
// 5-Band-Equalizer
//
// Das Aussehen ist bewusst das des Spektrumanalyzers: dieselben Reihen im
// selben Abstand, dieselben Farben. 13 Reihen, die mittlere ist 0 dB - eine
// Reihe ist damit genau 1 dB. Die Rechnung selbst steht in AudioCompat.h,
// hier stehen nur Anzeige, Bedienung und das Speichern.
//
// Die Maszahlen sind mit tools/screen_eq.py entworfen und dort nachgemessen.
// Wer eine davon aendert, aendert sie hier - das Skript liest sie von hier.
#define EQ_BANDS          5
#define EQ_ROWS          13   // Reihe 0 ist -6 dB, Reihe 6 ist 0, Reihe 12 +6
#define EQ_ROW_PITCH      4   // wie SPEC_ROW_PITCH
#define EQ_BOTTOM_Y     100   // unterste Reihe
#define EQ_COL_W         28   // breiter als die 18 des Spektrums
#define EQ_COL_GAP       10   // dort stehen die Punktreihen
#define EQ_X0            50   // so faellt die rechte Punktreihe auf x=235 -
                              // dieselbe Kante wie das Batteriesymbol. Das
                              // Raster ist schmaler als frueher (30/12/31);
                              // der gewonnene Platz steht links.
#define EQ_FREQ_Y       105   // Beschriftung unter den Saeulen
#define EQ_FRAME_MS      40   // Bildrate des Reglerlaufs
#define EQ_SAVE_IDLE_MS 2000  // so lange Ruhe, dann wird geschrieben
#define EQ_NVS_KEY "eq_gains"

// Eigener Schluessel, nicht der alte "eq_pre" aus dem Probelauf vom
// 11.8.2026: dort standen 0..12 fuer 0 bis 12 dB Daempfung. Hier ist der
// Bereich ein anderer, und ein liegengebliebener alter Wert wuerde still
// falsch gelesen.
#define EQ_PRE_NVS_KEY "eq_pre2"

// Die Zahlen stehen ein zweites Mal in acEqFreq[] in AudioCompat.h - dort
// rechnet die Maschine damit, hier steht nur die Beschriftung. Wer eine
// aendert, muss die andere mitziehen.
//
// Aussen ist es keine Mitte, sondern die Ecke eines Kuhschwanzes: 150Hz hebt
// alles darunter an, 8kHz alles darueber.
// Ohne Einheit: bei 28 px Saeulenbreite lief "3.5kHz" in die Nachbarspalte.
static const char* const eqFreqLabel[EQ_BANDS] =
    { "150", "350", "1k", "3.5k", "8k" };

// Links neben dem ersten Band liegt die Vordaempfung. Sie ist keine Marke
// mehr, sondern eine eigene Saeule im Stil des EQ: halb so breit, mit einer
// Punktlinie als linkem und rechtem Rand. Der Wert steht weiter in der
// Fusszeile. Erreichbar mit Pfeil links ueber die 150 Hz hinaus, verstellbar
// in 1-dB-Schritten wie ein Band. Die Grenzen stehen in AudioCompat.h
// (AC_EQ_PRE_MIN_DB, AC_EQ_PRE_MAX_DB), begrenzt wird dort.
//
// Die 31 Stufen passen nicht auf 13 Reihen. Der Regler laeuft deshalb im
// halben Reihenraster - er darf zwischen den Linien stehen, in beide
// Richtungen. Das gibt 25 Lagen statt 13.
#define EQ_SEL_PRE (-1)
#define EQ_PRE_DOT_L 4                                    // linke Punktlinie
#define EQ_PRE_W     (EQ_COL_W / 2)                       // halbe Saeule
#define EQ_PRE_X     (EQ_PRE_DOT_L + EQ_COL_GAP / 2)      // Abstand wie im EQ
#define EQ_PRE_DOT_R (EQ_PRE_X + EQ_PRE_W + EQ_COL_GAP / 2)

// Dauer eines Uebergangs: Wechsel der Auswahl und Wandern eines Reglers.
#define EQ_FADE_MS 250

// Tastenwiederholung wie unter Windows: erst eine Pause, dann zuegig.
#define EQ_REPEAT_DELAY_MS 500
#define EQ_REPEAT_RATE_MS   80

static int8_t   eqGainDb[EQ_BANDS] = { 0, 0, 0, 0, 0 };
static float    eqShownRow[EQ_BANDS];   // angezeigte Lage, laeuft dem Wert nach
static float    eqFromRow[EQ_BANDS];    // Lage beim Anstoss des Uebergangs
static uint32_t eqMoveStart[EQ_BANDS];  // wann er angestossen wurde
static float    eqPreShownY = 0;        // dasselbe fuer die Vordaempfung
static float    eqPreFromY  = 0;
static uint32_t eqPreMoveStart = 0;
static int      eqPrevPreKnobY = -1;    // zuletzt gemalte Reglerzeile
static uint8_t  eqBreathLast = 255;     // zuletzt gemalte Atemstufe
static int8_t   eqSelFrom = EQ_SEL_PRE - 1;  // Band, das gerade ausblendet
static uint32_t eqSelFadeStart = 0;     // 0 = kein Wechsel im Gange
static char     eqHeldKey = 0;          // gehaltene Pfeiltaste
static uint32_t eqHeldNext = 0;         // wann sie das naechste Mal zaehlt
static int8_t   eqSel = 0;              // EQ_SEL_PRE oder 0..EQ_BANDS-1
static bool     eqUnsaved = false;
static uint32_t eqLastChange = 0;
static uint32_t eqLastFrame = 0;

static int eqRowY(int row)       { return EQ_BOTTOM_Y - row * EQ_ROW_PITCH; }
static int eqColX(int band)      { return EQ_X0 + band * (EQ_COL_W + EQ_COL_GAP); }
static int eqTargetRow(int band) { return eqGainDb[band] + EQ_ROWS / 2; }

// Beschriftung eines Bandes. Das gewaehlte steht weiss, die uebrigen grau.
//
// ---------------------------------------------------------------------------
// Der Atem des gewaehlten Bandes
//
// Regler, aufgehellter Bereich und Beschriftung laufen in 1,2 s einmal von
// ihrer Ruhefarbe zur vollen Helligkeit und zurueck. Nach oben, nie nach
// unten: das gewaehlte Band wird dabei nie dunkler als ein nicht gewaehltes.
//
// Der erste Versuch flimmerte, und der Grund stand hier lange als Warnung:
// jedes Neuzeichnen loeschte erst und malte dann. Genau das entfaellt jetzt.
// Die Balkenreihen ueberschreiben sich ohnehin selbst, und Font0 malt bei
// gesetzter Hintergrundfarbe jede Zeichenzelle voll aus - der Text raeumt
// seinen Untergrund selbst weg. Die Beschriftung aendert beim Atmen nur die
// Farbe, nicht die Breite, also bleibt kein Rest stehen.
// ---------------------------------------------------------------------------
static const uint8_t EQ_C_HALF[3] = {  46,  70,  94 };   // Ruhe des Bereichs
static const uint8_t EQ_C_LIT [3] = { 120, 165, 210 };   // Ruhe des Reglers
static const uint8_t EQ_C_SEL [3] = { 235, 245, 255 };   // volle Helligkeit
static const uint8_t EQ_C_LBL [3] = { 123, 126, 123 };   // = TFT_DARKGREY
static const uint8_t EQ_C_FOOT[3] = { 128, 128, 128 };   // = FOOTER_TEXT_COLOR
static const uint8_t EQ_C_WHT [3] = { 255, 255, 255 };

// Wie stark ein Band gerade "gewaehlt" wirkt, 0..255. Das ist der Atem, aber
// beim Wechsel ueberblendet: das verlassene Band faellt in EQ_FADE_MS auf
// null, das neue steigt in derselben Zeit auf. Ohne diese Blende sprang die
// Auszeichnung hart um. Gilt fuer Bandsaeule, Beschriftung, Pre-Saeule und
// den Pre-Wert in der Fusszeile - alle fragen hier.
static uint8_t eqSelMix(int b) {
  const uint8_t breath = uiBreathLevel();

  if (eqSelFadeStart) {
    const uint32_t dt = millis() - eqSelFadeStart;
    if (dt < EQ_FADE_MS) {
      const uint32_t f = dt * 255 / EQ_FADE_MS;
      if (b == eqSel)     return (uint8_t)(breath * f / 255);
      if (b == eqSelFrom) return (uint8_t)(breath * (255 - f) / 255);
      return 0;
    }
  }
  return (b == eqSel) ? breath : 0;
}

// Lage des Pre-Reglers in Pixeln, gerundet auf das halbe Reihenraster.
static int eqPreY(int db) {
  const int halb   = EQ_ROW_PITCH / 2;
  const int spanne = (EQ_ROWS - 1) * EQ_ROW_PITCH;
  const int d = (AC_EQ_PRE_MAX_DB - db) * spanne
              / (AC_EQ_PRE_MAX_DB - AC_EQ_PRE_MIN_DB);
  return EQ_BOTTOM_Y - ((d + halb / 2) / halb) * halb;
}

static int footerTextY();   // steht weiter unten, wird hier schon gebraucht

static void eqDrawFreqLabel(int b) {
  const uint16_t col = uiMixColor(EQ_C_LBL, EQ_C_WHT, eqSelMix(b));

  const int x = eqColX(b);
  M5Cardputer.Display.setFont(FOOTER_FONT);
  M5Cardputer.Display.setTextColor(col, TFT_BLACK);
  M5Cardputer.Display.drawCentreString(eqFreqLabel[b], x + EQ_COL_W / 2,
                                       EQ_FREQ_Y);
  M5Cardputer.Display.setFont(UI_FONT);
  M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
}

// Eine Saeule neu zeichnen. Nicht loeschen und dann malen - das waere genau
// das Flimmern, das das Pulsieren gekostet hat. Jede der 13 Reihen bekommt
// ihre Farbe neu, und geloescht wird nur die eine Zeile, auf der der Regler
// zuletzt stand, falls sie zwischen den Reihen lag.
static int eqPrevKnobY[EQ_BANDS] = { -1, -1, -1, -1, -1 };

static void eqDrawColumn(int b) {
  const uint16_t dim  = M5Cardputer.Display.color565(15, 22, 30);
  const uint16_t mark = M5Cardputer.Display.color565(180, 75, 65);
  // Die hellen Farben stehen als EQ_C_* oben - eqSelMix() mischt sie.

  const int x     = eqColX(b);
  const int mid   = EQ_ROWS / 2;
  const int midY  = eqRowY(mid);
  const int knobY = (int)lroundf((float)EQ_BOTTOM_Y
                                 - eqShownRow[b] * (float)EQ_ROW_PITCH);

  // Alte Reglerzeile weg, wenn sie nicht auf einer Reihe lag - die Reihen
  // malen sich gleich ohnehin neu.
  const int prev = eqPrevKnobY[b];
  if (prev >= 0 && prev != knobY && ((EQ_BOTTOM_Y - prev) % EQ_ROW_PITCH) != 0) {
    M5Cardputer.Display.fillRect(x, prev, EQ_COL_W, 1, TFT_BLACK);
  }

  const int loY = (knobY < midY) ? knobY : midY;
  const int hiY = (knobY < midY) ? midY  : knobY;

  // Das gewaehlte Band atmet - Bereich und Regler wandern gemeinsam zur
  // vollen Helligkeit und zurueck.
  const uint8_t t = eqSelMix(b);
  const uint16_t halfCol = uiMixColor(EQ_C_HALF, EQ_C_LIT, t);
  const uint16_t knobCol = uiMixColor(EQ_C_LIT,  EQ_C_SEL, t);

  for (int r = 0; r < EQ_ROWS; r++) {
    const int y = eqRowY(r);
    uint16_t col = dim;
    if (r == mid)                        col = mark;
    else if (y >= loY && y <= hiY)       col = halfCol;
    M5Cardputer.Display.fillRect(x, y, EQ_COL_W, 1, col);
  }

  // Zuletzt der Regler: er verdeckt den roten Marker, wenn er darauf steht.
  M5Cardputer.Display.fillRect(x, knobY, EQ_COL_W, 1, knobCol);
  eqPrevKnobY[b] = knobY;
}

// Die Saeule der Vordaempfung. Aufbau wie ein Band: Reihen im Raster, der
// rote Marker auf 0 dB, der aufgehellte Bereich zwischen Marker und Regler.
// Zwei Unterschiede: sie ist halb so breit, und ihr Regler darf zwischen den
// Reihen stehen - anders bekaeme man 31 Stufen nicht auf 13 Linien.
static void eqDrawPre() {
  const uint16_t dim  = M5Cardputer.Display.color565(15, 22, 30);
  const uint16_t dot  = M5Cardputer.Display.color565(45, 60, 75);
  const uint16_t mark = M5Cardputer.Display.color565(180, 75, 65);

  const uint8_t  t       = eqSelMix(EQ_SEL_PRE);
  const uint16_t halfCol = uiMixColor(EQ_C_HALF, EQ_C_LIT, t);
  const uint16_t knobCol = uiMixColor(EQ_C_LIT,  EQ_C_SEL, t);

  const int markY = eqPreY(0);
  const int knobY = (int)lroundf(eqPreShownY);
  const int loY   = (knobY < markY) ? knobY : markY;
  const int hiY   = (knobY < markY) ? markY : knobY;

  for (int r = 0; r < EQ_ROWS; r++) {
    const int y = eqRowY(r);
    M5Cardputer.Display.fillRect(EQ_PRE_X, y, EQ_PRE_W, 1,
                                 (y >= loY && y <= hiY) ? halfCol : dim);
    M5Cardputer.Display.drawPixel(EQ_PRE_DOT_L, y, dot);
    M5Cardputer.Display.drawPixel(EQ_PRE_DOT_R, y, dot);
  }

  // Stand der Regler zwischen zwei Reihen, raeumen die Reihen ihn nicht weg.
  const int prev = eqPrevPreKnobY;
  if (prev >= 0 && prev != knobY
      && ((EQ_BOTTOM_Y - prev) % EQ_ROW_PITCH) != 0) {
    M5Cardputer.Display.fillRect(EQ_PRE_X, prev, EQ_PRE_W, 1, TFT_BLACK);
  }

  M5Cardputer.Display.fillRect(EQ_PRE_X, markY, EQ_PRE_W, 1, mark);
  M5Cardputer.Display.fillRect(EQ_PRE_X, knobY, EQ_PRE_W, 1, knobCol);
  eqPrevPreKnobY = knobY;
}

// Der Pre-Wert in der Fusszeile, in derselben Blende wie die Saeule. Ohne
// Loeschen davor: die Breite aendert sich beim Atmen nicht, und Font0 malt
// deckend. Wechselt der Wert, raeumt drawFooter() ohnehin.
static void eqDrawPreFooter() {
  char pre[16];
  snprintf(pre, sizeof(pre), "Pre %d dB", (int)audio.eqPreDb());

  M5Cardputer.Display.setFont(FOOTER_FONT);
  M5Cardputer.Display.setTextColor(
      uiMixColor(EQ_C_FOOT, EQ_C_WHT, eqSelMix(EQ_SEL_PRE)), TFT_BLACK);
  M5Cardputer.Display.drawString(pre, UI_MARGIN, footerTextY());
  M5Cardputer.Display.setFont(UI_FONT);
  M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
}

// Punktreihen in den Luecken und aussen. Sie tragen die Skala durch das Bild:
// eine Reihe, ein Dezibel. Die Zahlen +6/0/-6 standen einmal links daneben -
// sie sind weg, der Platz gehoert jetzt der Vordaempfung.
static void eqDrawGrid() {
  const uint16_t dot = M5Cardputer.Display.color565(45, 60, 75);

  int xs[EQ_BANDS + 1];
  xs[0] = EQ_X0 - EQ_COL_GAP / 2;
  for (int b = 0; b < EQ_BANDS; b++) xs[b + 1] = eqColX(b) + EQ_COL_W + EQ_COL_GAP / 2;

  for (int r = 0; r < EQ_ROWS; r++) {
    const int y = eqRowY(r);
    for (int i = 0; i <= EQ_BANDS; i++) M5Cardputer.Display.drawPixel(xs[i], y, dot);
  }
}

void drawEqFrame() {
  eqDrawGrid();
  for (int b = 0; b < EQ_BANDS; b++) {
    eqShownRow[b] = (float)eqTargetRow(b);   // beim Betreten kein Nachlaufen
    eqFromRow[b]  = eqShownRow[b];
    eqMoveStart[b] = 0;
    eqDrawColumn(b);
    eqDrawFreqLabel(b);
  }
  eqPreShownY = (float)eqPreY((int)audio.eqPreDb());
  eqPreFromY  = eqPreShownY;
  eqPreMoveStart = 0;
  eqPrevPreKnobY = -1;
  eqSelFadeStart = 0;
  eqDrawPre();
}

// Laeuft aus loop(). Der Regler folgt seinem Ziel, statt zu springen; wer
// schnell tippt, verschiebt nur das Ziel und verliert keinen Anschlag.
void updateEq() {
  if (visMode != VIS_EQ) return;
  if (uiOverlayActive()) return;
  if (millis() - eqLastFrame < EQ_FRAME_MS) return;
  eqLastFrame = millis();

  const uint32_t jetzt = millis();

  // Ein Uebergang dauert EQ_FADE_MS und laeuft am Ende aus (ease-out): der
  // Regler startet zuegig und legt sich weich an sein Ziel. Frueher war es
  // ein fester Bruchteil je Bild - der kam nie ganz an und hing an der
  // Bildrate.
  bool gemalt[EQ_BANDS] = { false };

  for (int b = 0; b < EQ_BANDS; b++) {
    const float target = (float)eqTargetRow(b);
    if (eqShownRow[b] == target) continue;

    const uint32_t dt = jetzt - eqMoveStart[b];
    if (dt >= EQ_FADE_MS) {
      eqShownRow[b] = target;
    } else {
      const float f = (float)dt / (float)EQ_FADE_MS;
      eqShownRow[b] = eqFromRow[b] + (target - eqFromRow[b]) * f * (2.0f - f);
    }
    eqDrawColumn(b);
    gemalt[b] = true;
  }

  // Dasselbe fuer die Vordaempfung.
  bool preGemalt = false;
  const float preZiel = (float)eqPreY((int)audio.eqPreDb());
  if (eqPreShownY != preZiel) {
    const uint32_t dt = jetzt - eqPreMoveStart;
    if (dt >= EQ_FADE_MS) {
      eqPreShownY = preZiel;
    } else {
      const float f = (float)dt / (float)EQ_FADE_MS;
      eqPreShownY = eqPreFromY + (preZiel - eqPreFromY) * f * (2.0f - f);
    }
    eqDrawPre();
    preGemalt = true;
  }

  // Atem und Auswahlblende. Nur wenn sich die Stufe geaendert hat - sonst
  // schickt jedes Bild dieselben Pixel ueber den Bus. Waehrend einer Blende
  // muessen beide Seiten mit, das verlassene Band ebenso wie das neue.
  const bool blende = eqSelFadeStart && (jetzt - eqSelFadeStart) < EQ_FADE_MS;
  const uint8_t lvl = uiBreathLevel();

  if (blende || lvl != eqBreathLast) {
    eqBreathLast = lvl;

    const int8_t betroffen[2] = { eqSel, blende ? eqSelFrom : eqSel };
    for (int i = 0; i < 2; i++) {
      const int8_t b = betroffen[i];
      if (i == 1 && betroffen[1] == betroffen[0]) break;

      if (b == EQ_SEL_PRE) {
        if (!preGemalt) eqDrawPre();
        eqDrawPreFooter();
      } else if (b >= 0 && b < EQ_BANDS) {
        if (!gemalt[b]) eqDrawColumn(b);
        eqDrawFreqLabel(b);
      }
    }
  }

  if (!blende && eqSelFadeStart) eqSelFadeStart = 0;
}

static void eqSave() {
  uint8_t raw[EQ_BANDS];
  for (int b = 0; b < EQ_BANDS; b++) raw[b] = (uint8_t)(eqGainDb[b] + 6);

  Preferences p;
  p.begin(NVS_NAMESPACE, false);
  p.putBytes(EQ_NVS_KEY, raw, sizeof(raw));
  // Um AC_EQ_PRE_MIN_DB verschoben ablegen, damit nichts Vorzeichenbehaftetes
  // im Flash steht - dieselbe Rechnung wie bei den Baendern.
  p.putUChar(EQ_PRE_NVS_KEY,
             (uint8_t)((int)audio.eqPreDb() - AC_EQ_PRE_MIN_DB));
  p.end();
  eqUnsaved = false;
}

// Beim Start einmal aufrufen. Die gespeicherte Kurve ist sofort wirksam -
// ein Equalizer ist eine Einstellung, kein Effekt zum Einschalten.
void eqLoad() {
  uint8_t raw[EQ_BANDS];

  Preferences p;
  p.begin(NVS_NAMESPACE, true);
  const size_t n = p.getBytes(EQ_NVS_KEY, raw, sizeof(raw));
  p.end();

  if (n == sizeof(raw)) {
    for (int b = 0; b < EQ_BANDS; b++) {
      const int v = (int)raw[b] - 6;
      eqGainDb[b] = (v >= -6 && v <= 6) ? (int8_t)v : 0;
    }
  }

  // Die Vordaempfung liegt in einem eigenen Schluessel. Steht dort nichts,
  // bleibt die Vorgabe aus AudioCompat.h stehen.
  {
    Preferences q;
    q.begin(NVS_NAMESPACE, true);
    const int pre = (int)q.getUChar(EQ_PRE_NVS_KEY, 255);
    q.end();
    if (pre <= AC_EQ_PRE_MAX_DB - AC_EQ_PRE_MIN_DB)
      audio.eqSetPreDb(pre + AC_EQ_PRE_MIN_DB);
  }

  for (int b = 0; b < EQ_BANDS; b++) {
    eqShownRow[b] = (float)eqTargetRow(b);
    audio.eqSetGain(b, eqGainDb[b]);
  }
}

// Nicht bei jedem Tastendruck schreiben: ein NVS-Schreibvorgang haelt die
// Schleife an, und die fuettert den Vorpuffer.
void eqSaveIfIdle() {
  if (!eqUnsaved) return;
  if (millis() - eqLastChange < EQ_SAVE_IDLE_MS) return;
  eqSave();
}

// --- Bedienung. An den Enden bleibt alles stehen, nichts bricht um. -------

static void eqAdjust(int delta) {
  // Hoch macht lauter, auch hier: der Pfeil nach oben nimmt Daempfung weg.
  if (eqSel == EQ_SEL_PRE) {
    const int was = (int)audio.eqPreDb();
    audio.eqSetPreDb(was - delta);   // begrenzt selbst auf -6 .. +24 dB
    if ((int)audio.eqPreDb() == was) return;   // stand schon am Anschlag

    eqPreFromY = eqPreShownY;        // von hier laeuft der Regler los
    eqPreMoveStart = millis();
    eqUnsaved = true;
    eqLastChange = millis();
    drawFooter();                    // dort steht der Wert
    eqDrawPreFooter();               // gleich in der Farbe der Blende
    return;
  }

  int v = eqGainDb[eqSel] + delta;
  if (v >  6) v =  6;
  if (v < -6) v = -6;
  if (v == eqGainDb[eqSel]) return;

  eqGainDb[eqSel] = (int8_t)v;
  audio.eqSetGain(eqSel, v);      // der Ton folgt sofort, das Bild laeuft nach
  eqFromRow[eqSel] = eqShownRow[eqSel];
  eqMoveStart[eqSel] = millis();
  eqUnsaved = true;
  eqLastChange = millis();
}

static void eqSelect(int delta) {
  const int s = (int)eqSel + delta;
  if (s < EQ_SEL_PRE || s >= EQ_BANDS) return;   // an den Enden stehenbleiben

  eqSelFrom = eqSel;
  eqSel = (int8_t)s;
  eqSelFadeStart = millis();
  if (!eqSelFadeStart) eqSelFadeStart = 1;   // 0 heisst "keine Blende"

  // Das Weitere macht updateEq() Bild fuer Bild - beide Seiten blenden dort
  // gemeinsam ueber.
}

// Gedrueckt gehaltene Pfeiltaste, wie unter Windows: eine Pause, dann Schritt
// um Schritt. Die Tastatur meldet nur "gedrueckt", keine Wiederholung - die
// Uhr macht sie hier. Der erste Schritt kommt weiter aus der Flanke in
// handleKeyboard(), hier faengt es erst nach EQ_REPEAT_DELAY_MS an.
static void eqAdjust(int delta);
static void eqSelect(int delta);

void eqHandleRepeat() {
  if (visMode != VIS_EQ) return;
  if (uiOverlayActive()) return;

  char k = 0;
  if      (M5Cardputer.Keyboard.isKeyPressed(';')) k = ';';
  else if (M5Cardputer.Keyboard.isKeyPressed('.')) k = '.';
  else if (M5Cardputer.Keyboard.isKeyPressed('/')) k = '/';
  else if (M5Cardputer.Keyboard.isKeyPressed(',')) k = ',';

  if (k == 0) { eqHeldKey = 0; return; }

  if (k != eqHeldKey) {                    // eben erst gedrueckt
    eqHeldKey  = k;
    eqHeldNext = millis() + EQ_REPEAT_DELAY_MS;
    return;
  }

  if ((int32_t)(millis() - eqHeldNext) < 0) return;
  eqHeldNext = millis() + EQ_REPEAT_RATE_MS;

  switch (k) {
    case ';': eqAdjust(+1); break;
    case '.': eqAdjust(-1); break;
    case '/': eqSelect(+1); break;
    case ',': eqSelect(-1); break;
  }
}

static void eqZeroAll() {
  bool changed = false;
  for (int b = 0; b < EQ_BANDS; b++) {
    if (eqGainDb[b] == 0) continue;
    eqGainDb[b] = 0;
    audio.eqSetGain(b, 0);
    changed = true;
  }
  if (changed) { eqUnsaved = true; eqLastChange = millis(); }
}

// ---------------------------------------------------------------------------
// Wo das Logo eines Senders liegt
//
// Der Dateiname kommt aus der Adresse, nicht aus dem Namen: Namen aendern
// sich, enthalten Schraegstriche und Zeichen jenseits von ASCII. Ein
// FNV-1a-Hash ueber die URL ergibt acht Hexziffern, die auf jedem
// Dateisystem ohne Nachdenken funktionieren.
//
// Roh abgelegt, 76 x 76 als RGB565: so braucht das Zeichnen keinen Decoder.
// ---------------------------------------------------------------------------
#define LOGO_DIR "/logos"

static uint32_t logoHash(const char* s) {
  uint32_t h = 2166136261u;                 // FNV-1a, 32 Bit
  for (; s && *s; s++) {
    h ^= (uint8_t)*s;
    h *= 16777619u;
  }
  return h;
}

static void logoPathFor(const char* url, char* dst, size_t cap, const char* endung) {
  snprintf(dst, cap, LOGO_DIR "/%08lX.%s", (unsigned long)logoHash(url), endung);
}

// Einmal beim Start: Ordner anlegen und das Ersatzbild hineinlegen, damit es
// austauschbar ist. Fehlt die Karte, ist das kein Grund zur Aufregung - dann
// zeichnet der Info-Schirm das Bild aus dem Flash.
static void logoPrepareSd() {
  if (!SD.begin()) return;
  if (!SD.exists(LOGO_DIR)) SD.mkdir(LOGO_DIR);

  const char* dummy = LOGO_DIR "/dummy.565";
  if (SD.exists(dummy)) return;

  File f = SD.open(dummy, FILE_WRITE);
  if (!f) return;
  uint16_t zeile[LOGO_DUMMY_W];
  for (int r = 0; r < LOGO_DUMMY_H; r++) {
    memcpy_P(zeile, &logoDummy[r * LOGO_DUMMY_W], sizeof(zeile));
    // In der Datei steht das hoechstwertige Byte zuerst - so, wie es auch
    // ein Umrechner auf dem Rechner schreiben wuerde.
    for (int i = 0; i < LOGO_DUMMY_W; i++) {
      zeile[i] = (uint16_t)((zeile[i] << 8) | (zeile[i] >> 8));
    }
    f.write((uint8_t*)zeile, sizeof(zeile));
  }
  f.close();
}

// ---------------------------------------------------------------------------
// Senderinfo: Logo links, vier Zeilen rechts
//
// Der Anzeigebereich zwischen roter Linie und Fusszeile ist 81 px hoch
// (header_height bis 135 - FOOTER_HEIGHT). Das Logofeld ist ein festes
// Quadrat von 76 px mit rund 10 px Luft oben und unten, links am
// UI_MARGIN. Rechts daneben bleiben 163 px fuer den Text - vier Zeilen zu
// TITLE_LINE_H, mittig im Band.
//
// Das Logo kommt **roh** von der SD-Karte: 76 x 76 Bildpunkte als RGB565,
// 11.552 Bytes, zeilenweise gelesen. Kein Decoder, kein grosser Block, kein
// Fehlschlag - egal wie voll der Heap gerade ist. Wer keins hat, bekommt das
// Ersatzbild aus dem Flash.
// ---------------------------------------------------------------------------
#define INFO_LOGO      76
#define INFO_GAP        8    // zwischen Logo und Text
#define INFO_LINES      4

// Was die beiden teuren Schritte am Stueck brauchen, am 14./15.8.2026 am
// Geraet gemessen:
//   TLS-Verbindung (mbedTLS)  rund 55 KB
//   PNG-Decoder (lgfx_pngle)  rund 47 KB
// Die dritte Zeile zeigt den groessten freien Block. Sie wird gelb, sobald
// **keiner von beiden** mehr hineinpasst - dann kann kein Logo mehr geholt
// und keins mehr dekodiert werden. Rot gibt es nicht: es ist kein Fehler,
// nur eine Einschraenkung.
// Mindestgroesse des groessten Blocks, bevor ein Logo geholt wird.
//
// Die Zahl hat schon dreimal gewechselt: 56.000 (TLS-Handschlag, alte
// Bibliotheken), 12.000 (18.8.2026, als der Logopfad ohne TLS lief), und seit
// dem 19.8.2026 wieder eine mit TLS darin - das Logo wird jetzt ueber https
// geholt, wenn der Server darauf besteht.
//
// 40.000: gemessen ist ein Handschlag mit den eigenen Bibliotheken bei
// **36.864** vom groessten Block (vier Starts, siehe cardputer-eigene-idf-libs),
// dazu etwas Luft. Wer weniger hat, bekaeme das Bild ohnehin nicht dekodiert -
// INFO_NEED_PNG steht mit 46.000 noch darueber und entscheidet in der Praxis.
#define INFO_NEED_HEAP 40000
// Groesster Block, den der PNG-Dekoder am Stueck braucht. **Aus dem
// Quelltext von M5GFX, nicht geschaetzt:** lgfx_pngle.c legt pngle_t in einem
// einzigen malloc an, darin
//     lgfx_tinfl_decompressor inflator;   // 11.000 Bytes
//     uint8_t lz_buf[TINFL_LZ_DICT_SIZE]; // 32.768 Bytes
// zusammen 43.768 plus Kleinkram, dazu je Bild ein Zeilenpuffer.
// 46.000 ist das mit etwas Luft.
//
// Wichtig: der Sprite in logoZu565() (11.552 Bytes) zaehlt hier **nicht**
// dazu. Das sind zwei getrennte Anforderungen, die aus zwei verschiedenen
// Bloecken kommen duerfen - sie zu addieren war mein Denkfehler und hat am
// 18.8.2026 jede Umrechnung abgewiesen, obwohl reichlich Platz war.
//
// Schlaegt es trotzdem fehl, ist das ungefaehrlich: LGFXBase::draw_png prueft
// den Zeiger und liefert false (LGFXBase.cpp:3428).
#define INFO_NEED_PNG  46000

static void drawInfoText();   // steht weiter unten

static int infoBandY() { return header_height; }
static int infoBandH() { return 135 - header_height - FOOTER_HEIGHT; }

// Ein Bild von der Karte auf den Schirm schieben, Zeile fuer Zeile. Der
// Puffer ist eine Zeile gross: 76 mal zwei Bytes.
static bool drawRawLogo(const char* pfad, int x, int y) {
  File f = SD.open(pfad);
  if (!f) return false;

  if (f.size() != (size_t)(INFO_LOGO * INFO_LOGO * 2)) { f.close(); return false; }

  uint16_t zeile[INFO_LOGO];
  for (int r = 0; r < INFO_LOGO; r++) {
    if (f.read((uint8_t*)zeile, sizeof(zeile)) != (int)sizeof(zeile)) {
      f.close();
      return false;
    }
    // Auf der Karte steht das hoechstwertige Byte zuerst, der ESP32 ist
    // umgekehrt herum - also drehen, sonst kommen falsche Farben heraus.
    for (int i = 0; i < INFO_LOGO; i++) {
      zeile[i] = (uint16_t)((zeile[i] << 8) | (zeile[i] >> 8));
    }
    M5Cardputer.Display.pushImage(x, y + r, INFO_LOGO, 1, zeile);
  }
  f.close();
  return true;
}

// Das Ersatzbild aus dem Flash. Kann nicht scheitern.
static void drawDummyLogo(int x, int y) {
  static uint16_t zeile[LOGO_DUMMY_W];
  for (int r = 0; r < LOGO_DUMMY_H; r++) {
    memcpy_P(zeile, &logoDummy[r * LOGO_DUMMY_W], sizeof(zeile));
    M5Cardputer.Display.pushImage(x, y + r, LOGO_DUMMY_W, 1, zeile);
  }
}

// Rechnername aus der Adresse ziehen: zwischen "//" und dem naechsten "/".
static void infoHost(const char* url, char* dst, size_t cap) {
  dst[0] = '\0';
  if (!url) return;
  const char* p = strstr(url, "//");
  p = p ? p + 2 : url;
  size_t n = 0;
  while (p[n] && p[n] != '/' && n + 1 < cap) { dst[n] = p[n]; n++; }
  dst[n] = '\0';
}

// Masse eines JPEG: den Markern folgen, bis ein SOF kommt. Anders als beim
// PNG stehen sie nicht an fester Stelle.
static bool jpgMasse(const char* pfad, uint32_t& w, uint32_t& h) {
  File f = SD.open(pfad);
  if (!f) return false;
  uint8_t k[4];
  if (f.read(k, 2) != 2 || k[0] != 0xFF || k[1] != 0xD8) { f.close(); return false; }

  while (f.available() > 4) {
    if (f.read(k, 1) != 1) break;
    if (k[0] != 0xFF) continue;                 // bis zum naechsten Marker
    if (f.read(k, 1) != 1) break;
    const uint8_t m = k[0];
    if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) continue;  // ohne Rumpf
    if (f.read(k, 2) != 2) break;
    const uint16_t laenge = (uint16_t)((k[0] << 8) | k[1]);
    // SOF0..SOF15, ohne die Huffman- und Rechenmarker dazwischen
    if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
      uint8_t d[5];
      if (f.read(d, 5) != 5) break;
      h = (uint32_t)((d[1] << 8) | d[2]);
      w = (uint32_t)((d[3] << 8) | d[4]);
      f.close();
      return w && h;
    }
    if (laenge < 2) break;
    f.seek(f.position() + laenge - 2);
  }
  f.close();
  return false;
}

// Masse eines PNG: Bytes 16 bis 23, hoechstwertiges zuerst.
static bool pngMasse(const char* pfad, uint32_t& w, uint32_t& h) {
  File f = SD.open(pfad);
  if (!f) return false;
  uint8_t k[24];
  const int n = f.read(k, sizeof(k));
  f.close();
  if (n < (int)sizeof(k)) return false;
  static const uint8_t kennung[8] = { 0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A };
  if (memcmp(k, kennung, 8) != 0) return false;
  w = ((uint32_t)k[16]<<24)|((uint32_t)k[17]<<16)|((uint32_t)k[18]<<8)|k[19];
  h = ((uint32_t)k[20]<<24)|((uint32_t)k[21]<<16)|((uint32_t)k[22]<<8)|k[23];
  return w && h;
}

#if DEBUG_SERIAL
  #define LOGO_SAG(...)  Serial.printf("[logo] " __VA_ARGS__)
#else
  #define LOGO_SAG(...)  do {} while (0)
#endif

// Ein geholtes PNG/JPEG einmalig zu .565 rechnen und ablegen. Danach wird das
// Bild nie wieder dekodiert - der Info-Schirm liest ausschliesslich .565.
//
// Warum ueber eine Sprite-Flaeche und nicht vom Bildschirm zurueckgelesen:
// der Cardputer hat am Display-Bus **kein MISO** (M5GFX.cpp, bus_cfg.pin_miso
// = -1). Panel_LCD::readRect prueft _cfg.readable und liefert sonst
// memset(0) - man bekaeme ein schwarzes Quadrat. Nachgesehen, nicht geraten.
//
// FALSCHFARBEN, die Falle: readRectRGB() castet auf bgr888_t, aber dessen
// Felder stehen in der Reihenfolge r, g, b (colortype.hpp:357) - Byte 0 ist
// also Rot. Damit ist die Formel unten Zeichen fuer Zeichen dieselbe wie in
// tools/logo565.html, und **niederwertiges Byte zuerst** auf die Karte.
// Der Kommentar in drawRawLogo() behauptet das Gegenteil ("hoechstwertiges
// Byte zuerst") - der ist falsch, das Werkzeug ist die Wahrheit.
static bool logoZu565(const char* quelle, const char* ziel, bool istPng) {
  uint32_t w = 0, h = 0;
  if (istPng ? !pngMasse(quelle, w, h) : !jpgMasse(quelle, w, h)) return false;

  const uint32_t lang = (w > h) ? w : h;
  if (!lang) return false;

  LOGO_SAG("umrechnen: Block %lu vor dem Sprite\n",
           (unsigned long)ESP.getMaxAllocHeap());

  M5Canvas cv(&M5Cardputer.Display);
  cv.setColorDepth(16);
  if (!cv.createSprite(INFO_LOGO, INFO_LOGO)) {
    LOGO_SAG("createSprite abgelehnt\n");
    return false;
  }

  LOGO_SAG("umrechnen: Block %lu nach dem Sprite\n",
           (unsigned long)ESP.getMaxAllocHeap());

  // Schwarzer Grund wie das Band im Info-Schirm. Durchsichtigkeit gibt es in
  // .565 nicht, sie muss hier hineingerechnet werden.
  cv.fillScreen(TFT_BLACK);

  const float skal = (float)INFO_LOGO / (float)lang;
  const int bx = (INFO_LOGO - (int)(w * skal)) / 2;
  const int by = (INFO_LOGO - (int)(h * skal)) / 2;

  bool ok;
  if (istPng) {
    ok = cv.drawPngFile(SD, quelle, bx, by, INFO_LOGO, INFO_LOGO, 0, 0, skal, skal);
    cv.releasePngMemory();          // gibt die rund 47 KB sofort wieder her
  } else {
    ok = cv.drawJpgFile(SD, quelle, bx, by, INFO_LOGO, INFO_LOGO, 0, 0, skal, skal);
  }

  if (!ok) {
    cv.deleteSprite();
    LOGO_SAG("Dekoder abgelehnt\n");
    return false;
  }

  SD.remove(ziel);
  File f = SD.open(ziel, FILE_WRITE);
  if (!f) {
    cv.deleteSprite();
    return false;
  }

  uint8_t rgb[INFO_LOGO * 3];
  uint8_t roh[INFO_LOGO * 2];
  bool gut = true;

  for (int r = 0; r < INFO_LOGO && gut; r++) {
    cv.readRectRGB(0, r, INFO_LOGO, 1, rgb);
    for (int i = 0; i < INFO_LOGO; i++) {
      const uint16_t v = (uint16_t)(((rgb[i * 3]     & 0xF8) << 8)
                                  | ((rgb[i * 3 + 1] & 0xFC) << 3)
                                  |  (rgb[i * 3 + 2] >> 3));
      roh[i * 2]     = (uint8_t)(v & 0xFF);      // niederwertiges Byte zuerst
      roh[i * 2 + 1] = (uint8_t)(v >> 8);
    }
    gut = (f.write(roh, sizeof(roh)) == sizeof(roh));
  }

  f.flush();
  f.close();
  cv.deleteSprite();                // 11,5 KB sofort zurueck

  if (!gut) { SD.remove(ziel); return false; }

  LOGO_SAG(".565 abgelegt: %s, Block wieder %lu\n", ziel,
           (unsigned long)ESP.getMaxAllocHeap());
  return true;
}

void drawInfoScreen() {
  const int by = infoBandY(), bh = infoBandH();
  M5Cardputer.Display.fillRect(0, by, 240, bh, TFT_BLACK);

  const int lx = UI_MARGIN;
  const int ly = by + (bh - INFO_LOGO) / 2;

  // Rahmen zuerst - er steht auch dann, wenn das Bild kleiner ausfaellt.
  M5Cardputer.Display.drawRect(lx, ly, INFO_LOGO, INFO_LOGO, TFT_DARKGREY);

  const char* url = extStationActive ? extStationUrl : stations[curStation].url;

  char pfad[80];
  bool gezeichnet = false;

  // **Nur .565.** Kein PNG und kein JPEG erreicht den Info-Schirm mehr - der
  // Dekoder laeuft ausschliesslich einmalig in logoZu565() beim Holen, bei
  // stillem Ton. Liegt kein .565 vor, kommt der Dummy; es wird ausdruecklich
  // NICHT ersatzweise ein Bild dekodiert.
  logoPathFor(url, pfad, sizeof(pfad), "565");

#if DEBUG_SERIAL
  Serial.printf("[logo] Schirm sucht %s (%s) fuer %s\n",
                pfad, SD.exists(pfad) ? "da" : "fehlt", url);
#endif

  infoUrlReset();   // Strecke messen, Lauf von vorn

  gezeichnet = drawRawLogo(pfad, lx, ly);

  if (!gezeichnet && !drawRawLogo(LOGO_DIR "/dummy.565", lx, ly)) {
    drawDummyLogo(lx, ly);                           // das aus dem Flash
  }

  drawInfoText();
}

// ---------------------------------------------------------------------------
// Die Adresszeile laeuft
//
// Statt die Adresse abzuschneiden, steht sie vollstaendig da - mit Schema und
// Pfad. Sie zeigt sich INFO_URL_STILL_MS lang still, laeuft dann nach links
// aus, bis nur noch INFO_URL_REST_CH Zeichen im Fenster stehen, blendet aus
// und faengt von vorn an.
//
// Zwei Dinge sind dabei wichtig. Erstens das Fenster: links neben dem Text
// steht das Logo, deshalb setClipRect() - ohne das liefe die Adresse hinein.
// Zweitens kein Loeschen vor dem Zeichnen; die Zeile deckt sich selbst, nur
// was rechts von ihr uebrigbleibt, wird geraeumt.
// ---------------------------------------------------------------------------
static const uint8_t INFO_C_TEXT[3] = { 128, 128, 128 };  // FOOTER_TEXT_COLOR
static const uint8_t INFO_C_AUS [3] = {   0,   0,   0 };

static UiLauf infoLauf;

static const char* infoUrl() {
  return extStationActive ? extStationUrl : stations[curStation].url;
}

// y der Zeile i, dieselbe Rechnung wie im Textblock.
static int infoLineY(int i) {
  const int bh = infoBandH();
  return infoBandY() + (bh - INFO_LINES * TITLE_LINE_H) / 2 + i * TITLE_LINE_H;
}

// Die Adresszeile in ihrem aktuellen Stand, vollstaendig mit Schema und Pfad.
static void infoDrawUrlLine(uint8_t helligkeit) {
  const char* url = infoUrl();
  if (!url) return;

  const int tx   = UI_MARGIN + INFO_LOGO + INFO_GAP;
  const int maxW = 240 - UI_MARGIN - tx;
  const int zy   = infoLineY(INFO_LINES - 1);

  M5Cardputer.Display.setFont(TITLE_FONT);
  M5Cardputer.Display.setTextColor(
      uiMixColor(INFO_C_AUS, INFO_C_TEXT, helligkeit), TFT_BLACK);
  M5Cardputer.Display.setClipRect(tx, zy, maxW, TITLE_LINE_H);

  const int x = tx - infoLauf.scroll;
  M5Cardputer.Display.drawString(url, x, zy);

  const int end = x + M5Cardputer.Display.textWidth(url);
  if (end < tx + maxW) {
    M5Cardputer.Display.fillRect(end, zy, tx + maxW - end, TITLE_LINE_H,
                                 TFT_BLACK);
  }

  M5Cardputer.Display.clearClipRect();
  M5Cardputer.Display.setFont(UI_FONT);
  M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
}

// Beim Betreten des Schirms und bei jedem Senderwechsel: Strecke neu messen.
static void infoUrlReset() {
  const int tx   = UI_MARGIN + INFO_LOGO + INFO_GAP;
  M5Cardputer.Display.setFont(TITLE_FONT);
  infoLauf.reset(infoUrl(), 240 - UI_MARGIN - tx);
  M5Cardputer.Display.setFont(UI_FONT);
}

// Eine Zahl rechtsbuendig auf feste Stellen, damit die Einheit dahinter
// stehenbleibt statt zu wandern.
//
// Wichtig: die NotoSans ist proportional. Ein Leerzeichen ist 4 px breit,
// eine Ziffer 8 - ein einzelnes Leerzeichen je fehlender Stelle wuerde also
// nur die Haelfte ausgleichen, und die Einheit rutschte trotzdem. Deshalb
// **zwei** Leerzeichen je fehlender Ziffer. Alle Ziffern sind gleich breit,
// damit stimmt es exakt.
static void infoPad(char* buf, size_t n, unsigned wert, int stellen) {
  char zahl[12];
  snprintf(zahl, sizeof(zahl), "%u", wert);

  size_t k = 0;
  for (int i = (int)strlen(zahl); i < stellen && k + 3 < n; i++) {
    buf[k++] = ' ';
    buf[k++] = ' ';
  }
  snprintf(buf + k, n - k, "%s", zahl);
}

// Nur der Textblock rechts.
static void drawInfoText() {
  const int by = infoBandY(), bh = infoBandH();
  const int lx = UI_MARGIN;

  const uint32_t rate = audio.getSampleRate();
  const uint8_t  kan  = audio.getChannels();
  const int      kbit = (int)(audio.getBitRate() / 1000);

  char z[INFO_LINES][40];

  char feld[16];
  infoPad(feld, sizeof(feld), (unsigned)(kbit > 0 ? kbit : 0), 3);
  snprintf(z[0], sizeof(z[0]), "%s %s kBit", audio.getCodecname(), feld);
  if (rate) {
    snprintf(z[1], sizeof(z[1]), "%u.%u kHz %s", (unsigned)(rate / 1000),
             (unsigned)((rate % 1000) / 100), (kan == 1) ? "Mono" : "Stereo");
  } else {
    snprintf(z[1], sizeof(z[1]), "-");
  }
  const uint32_t block = ESP.getMaxAllocHeap();
  const unsigned fuell = (unsigned)audio.bufferFillPct();

  char pct[16], heap[16];
  infoPad(pct,  sizeof(pct),  fuell, 2);          // 100 % kommt nicht vor
  infoPad(heap, sizeof(heap), (unsigned)(block / 1024), 2);
  snprintf(z[2], sizeof(z[2]), "Buffer %s%%  %sk", pct, heap);

  M5Cardputer.Display.setFont(TITLE_FONT);

  const int tx = lx + INFO_LOGO + INFO_GAP;
  const int maxW = 240 - UI_MARGIN - tx;
  int ty = by + (bh - INFO_LINES * TITLE_LINE_H) / 2;

  // Die letzte Zeile gehoert der laufenden Adresse, siehe infoDrawUrlLine().
  for (int i = 0; i < INFO_LINES - 1; i++) {
    // Die Pufferzeile faerbt sich, wenn der Vorrat knapp wird - alles
    // andere bleibt grau. Frueher hing das am freien Speicher; seit der
    // Info-Schirm kein PNG mehr dekodiert, sagt der Fuellstand mehr.
    const bool knapp = (i == 2) && (fuell <= 6);
    M5Cardputer.Display.setTextColor(knapp ? TFT_YELLOW : FOOTER_TEXT_COLOR,
                                     TFT_BLACK);

    // Zu langes wird abgeschnitten, nicht umgebrochen - vier Zeilen sind
    // vier Zeilen.
    char* t = z[i];
    while (t[0] && M5Cardputer.Display.textWidth(t) > maxW) {
      t[strlen(t) - 1] = '\0';
    }
    const int zy = ty + i * TITLE_LINE_H;
    M5Cardputer.Display.drawString(t, tx, zy);

    // Nicht loeschen und dann malen - das hat beim Auffrischen geflimmert.
    // Der Text deckt seinen Untergrund selbst: auch die NotoSans fuellt beim
    // Zeichnen jede Luecke zwischen den Glyphen (GFXfont::drawChar fuehrt
    // dafuer filled_x mit). Zu raeumen bleibt, was rechts ueberhaengt, wenn
    // die vorige Fassung der Zeile laenger war.
    const int end = tx + M5Cardputer.Display.textWidth(t);
    if (end < 240 - UI_MARGIN) {
      M5Cardputer.Display.fillRect(end, zy, 240 - UI_MARGIN - end,
                                   TITLE_LINE_H, TFT_BLACK);
    }
  }

  M5Cardputer.Display.setFont(UI_FONT);
  M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);

  infoDrawUrlLine(infoLauf.helligkeit());
}


// Die drei unteren Zeilen aendern sich im Betrieb (Puffer, Heap, Bitrate).
// Einmal je Sekunde nachziehen - und nur den Textteil, das Logo bleibt
// stehen. Sonst laege bei jedem Durchgang ein SD-Zugriff im Weg.
// Der Lauf der Adresszeile. Eigener Takt: updateInfoScreen() kommt nur
// einmal je Sekunde, das waere kein Laufen, sondern ein Springen.
void updateInfoUrl() {
  if (visMode != VIS_INFO || uiOverlayActive()) return;
  uint8_t hell;
  if (infoLauf.tick(hell)) infoDrawUrlLine(hell);
}

void updateInfoScreen() {
  if (visMode != VIS_INFO || uiOverlayActive()) return;

  static uint32_t zuletzt = 0;
  if (millis() - zuletzt < 1000) return;
  zuletzt = millis();

  drawInfoText();   // raeumt selbst nach, siehe dort
}

// Statische Teile der gerade aktiven Darstellung zeichnen.
void drawVisFrame() {
  if (visMode == VIS_OFF) {
    drawTitleBlock();
  }
  else if (visMode == VIS_VU || visMode == VIS_VU_PEAK) {
    drawVuFrame();
    vuReset();
  }
  else if (visMode == VIS_SPECTRUM) {
    specFill = 0;
    specReady = false;
    drawSpectrumFrame();
  }
  else if (visMode == VIS_INFO) {
    drawInfoScreen();
  }
  else if (visMode == VIS_EQ) {
    drawEqFrame();
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

// Direkt auf einen bestimmten Schirm schalten. Der Rumpf stammt aus
// toggleVuMode() - beide muessen dasselbe tun, sonst bleibt beim Spektrum ein
// Puffer liegen oder der Equalizer wird nicht gespeichert.
// Der Parameter ist int und nicht VisMode: die Arduino-Werkzeugkette setzt
// ihre Funktionsprototypen **oberhalb** der enum-Deklaration ein, dort ist
// VisMode noch unbekannt ("declared void"). Mit int uebersetzt es sauber.
void visSet(int nextRoh) {
  VisMode next = (VisMode)nextRoh;
  if (next == visMode) return;
  const VisMode vorher = visMode;

  if (visMode == VIS_EQ && next != VIS_EQ) {
    if (eqUnsaved) eqSave();
  }
  if (next == VIS_EQ) eqSel = 0;
  const bool eqFooterChange = (visMode == VIS_EQ) != (next == VIS_EQ);

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
  if (eqFooterChange) drawFooter();

#if DEBUG_SERIAL
  // Damit beim naechsten Mal nicht geraten werden muss, wo Puffer liegen
  // bleiben: jeder Wechsel mit Zustand der Spektrumpuffer und Blockgroesse.
  Serial.printf("[vis] %d -> %d, FFT-Puffer %s, groesster Block %lu\n",
                (int)vorher, (int)visMode,
                specBuffersReady ? "belegt" : "frei",
                (unsigned long)ESP.getMaxAllocHeap());
#endif
}

// V schaltet nur die drei Pegelanzeigen durch: VU, VU mit Peak, Spektrum.
// Info und Equalizer haben ihre eigenen Tasten und liegen nicht im Ring.
void visCycleMeters() {
  switch (visMode) {
    case VIS_VU:       visSet(VIS_VU_PEAK);  break;
    case VIS_VU_PEAK:  visSet(VIS_SPECTRUM); break;
    case VIS_SPECTRUM: visSet(VIS_VU);       break;
    default:           visSet(VIS_VU);       break;   // aus Info/EQ/Aus herein
  }
}

// F schaltet durch. Beim Betreten des Spektrums werden die Puffer angelegt,
// beim Verlassen wieder freigegeben - siehe Kommentar bei den Zeigern.
void toggleVuMode() {
  VisMode next = (VisMode)((visMode + 1) % VIS_MODES);

  // Der Equalizer wird beim Verlassen gespeichert - der uebliche Weg hinaus
  // schreibt also immer, ohne auf die Ruhezeit zu warten.
  if (visMode == VIS_EQ && next != VIS_EQ) {
    if (eqUnsaved) eqSave();
  }
  if (next == VIS_EQ) eqSel = 0;   // beim Betreten ist das erste Band gewaehlt
  const bool eqFooterChange = (visMode == VIS_EQ) != (next == VIS_EQ);

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
  if (eqFooterChange) drawFooter();   // Kennung oder Daempfung
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

  // Der Ladestand als Zahl, ohne Einheit, rechtsbuendig BATT_PCT_GAP vor dem
  // Gehaeuse. Sie sitzt ein Pixel unter der Mitte - so steht sie optisch auf
  // einer Linie mit der Fuellung.
  //
  // Geloescht wird nur links vom Text: die Zahl deckt sich selbst, aber sie
  // wird kuerzer, wenn aus 100 eine 99 wird.
  char pct[8];
  snprintf(pct, sizeof(pct), "%d", batteryLevel);

  M5Cardputer.Display.setFont(FOOTER_FONT);
  M5Cardputer.Display.setTextColor(FOOTER_TEXT_COLOR, TFT_BLACK);

  const int pctR = battX - BATT_PCT_GAP;
  const int pctW = M5Cardputer.Display.textWidth(pct);

  M5Cardputer.Display.drawString(pct, pctR - pctW, BATT_PCT_Y);
  if (pctW < BATT_PCT_W) {
    M5Cardputer.Display.fillRect(pctR - BATT_PCT_W, 0,
                                 BATT_PCT_W - pctW, HDR_BATT_H, TFT_BLACK);
  }

  M5Cardputer.Display.setFont(UI_FONT);
  M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
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
// Der Sendername in der Kopfzeile. Steht an zwei Stellen an: beim vollen
// Neuaufbau und wenn der ICY-Kopf nachtraeglich einen Namen nachreicht.
// Deshalb einmal hier, statt zweimal fast gleich - der zweite Weg hatte die
// Schriftweiche gefehlt, und ein chinesischer Name wurde nachtraeglich mit
// der Nokia-Schrift ueberschrieben: erst richtig, dann Kaestchen.
// Der Sendername laeuft aus, wenn er zu breit ist - siehe UiLauf oben.
static UiLauf hdrLauf;
static char   hdrName[64] = "";

#define HDR_NAME_MAXW (240 - 2 * UI_MARGIN)

static const uint8_t HDR_C_TEXT[3] = { 255, 255, 255 };
static const uint8_t HDR_C_AUS [3] = {   0,   0,   0 };

// Malt den Namen in seinem aktuellen Stand. Kein fillRect davor: der Text
// deckt sich selbst, geraeumt wird nur rechts von ihm.
static void uiDrawHeaderLine(uint8_t helligkeit) {
  if (!hdrName[0]) return;

  const bool uni = uiNeedsUnicode(hdrName);
  // efont ist zwei Pixel niedriger, deshalb der Versatz - sonst springt
  // die Zeile beim Wechsel der Schrift.
  const int y = uni ? HDR_NAME_Y + 1 : HDR_NAME_Y;

  M5Cardputer.Display.setFont(uni ? uiUniFont : UI_FONT);
  M5Cardputer.Display.setTextColor(
      uiMixColor(HDR_C_AUS, HDR_C_TEXT, helligkeit), TFT_BLACK);
  M5Cardputer.Display.setClipRect(HDR_TEXT_X, HDR_NAME_Y,
                                  HDR_NAME_MAXW, HDR_NAME_H);

  const int x = HDR_TEXT_X - hdrLauf.scroll;
  M5Cardputer.Display.drawString(hdrName, x, y);

  const int end = x + M5Cardputer.Display.textWidth(hdrName);
  if (end < HDR_TEXT_X + HDR_NAME_MAXW) {
    M5Cardputer.Display.fillRect(end, HDR_NAME_Y,
                                 HDR_TEXT_X + HDR_NAME_MAXW - end,
                                 HDR_NAME_H, TFT_BLACK);
  }

  M5Cardputer.Display.clearClipRect();
  M5Cardputer.Display.setFont(UI_FONT);
  M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
}

static void uiDrawHeaderName(const char* name) {
  snprintf(hdrName, sizeof(hdrName), "%s", name ? name : "");

  M5Cardputer.Display.fillRect(0, HDR_NAME_Y, 240, HDR_NAME_H, TFT_BLACK);
  M5Cardputer.Display.setFont(uiNeedsUnicode(hdrName) ? uiUniFont : UI_FONT);
  hdrLauf.reset(hdrName, HDR_NAME_MAXW);
  M5Cardputer.Display.setFont(UI_FONT);

  uiDrawHeaderLine(255);
}

// Laeuft aus loop(). Die Kopfzeile steht auf jedem Schirm, deshalb ohne
// Bindung an visMode.
void updateHeaderName() {
  if (uiOverlayActive()) return;
  uint8_t hell;
  if (hdrLauf.tick(hell)) uiDrawHeaderLine(hell);
}

void showStation() {
  if (uiOverlayActive()) return;   // nicht in ein offenes Fenster malen

  uiDrawHeaderName(extStationActive ? extStationName
                                    : stations[curStation].name);

  drawTitleBlock();

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

// ---------------------------------------------------------------------------
// Das Logo eines Senders holen
//
// Laeuft beim Senderwechsel, **nachdem** der Ton angehalten ist und bevor
// der neue Sender verbunden wird. Das ist der einzige Zeitpunkt, an dem der
// Heap sauber ist: am Geraet gemessen 104 KB frei und 73 KB am Stueck.
// Danach ist wieder alles frei, und der Ton beginnt.
//
// Gemessen am 14.8.2026: eine TLS-Verbindung kostet rund 55 KB, der
// PNG-Decoder rund 47. Nacheinander passt beides, gleichzeitig nicht.
//
// Scheitert irgendetwas, passiert genau nichts - der Info-Schirm zeigt dann
// das Ersatzbild. Keine Meldung, kein Abbruch, kein Warten.
//
// Keine Merkdatei fuer Fehlschlaege: ein Server, der heute nicht antwortet,
// kann es morgen wieder tun. Der Preis ist ein vergeblicher Versuch je
// Wechsel - deshalb die kurzen Fristen.
// ---------------------------------------------------------------------------
// Wird der Hinweis diesmal gezeigt? Playfile() setzt ihn aus seinem
// Parameter; der Streamwaechter baut mit false auf und bleibt damit stumm.
static bool logoHinweisErlaubt = false;

// Ein Hinweis, solange geladen wird. Ohne ihn steht das Radio zwei Sekunden
// stumm mit unveraendertem Bild da - das sieht nach Absturz aus.
//
// Hier stand bis zum 16.8.2026 ein Ladebalken mit festen Marken entlang des
// Vorgangs. Ecki hat ihn verworfen: der Text darueber sagt dasselbe, und der
// Balken raeumte das ganze Band. Uebrig bleibt genau dieser Text.
static void logoHinweis() {
  const int by = header_height;
  const int bh = 135 - header_height - FOOTER_HEIGHT;

  M5Cardputer.Display.fillRect(0, by, 240, bh, TFT_BLACK);
  M5Cardputer.Display.setFont(FOOTER_FONT);
  M5Cardputer.Display.setTextColor(FOOTER_TEXT_COLOR, TFT_BLACK);
  M5Cardputer.Display.drawCentreString(T(STR_LOADING_STATION), 120,
                                       by + bh / 2 - 8);
  M5Cardputer.Display.setFont(UI_FONT);
}

#define LOGO_HOLEN_MS   5000    // Verbindung und Antwort, je
//
// **Am 19.8.2026 auf 10 s erhoeht und noch am selben Abend zurueckgenommen.**
// Anlass war das erste Logo ueber TLS (assets.radioapi.me), das mit "HTTP -1"
// nach 5.040 ms scheiterte - genau der Frist. Mit 10 s scheiterte es nach
// 10.009 ms, moremalc.com nach 10.129. **Die Verbindung kommt gar nicht
// zustande; das ist kein Zeitproblem.** Die laengere Frist hat nur die
// Wartezeit vor dem Ton verdoppelt, bei jedem Senderstart aufs Neue.
//
// Und gemerkt wird ein Fehlversuch ausdruecklich **nicht**: ein Logo kann
// spaeter durchaus kommen, der Server kann zurueckkommen, die Adresse kann
// sich aendern. Wer den Fehlschlag festschreibt, sperrt das Bild fuer immer
// aus - Eckis Ansage vom 19.8.2026, und nicht das erste Mal.
#define LOGO_MAX_BYTES  120000  // groesser wird kein Senderlogo gebraucht

// ---------------------------------------------------------------------------
// /logos/index.txt - welcher Sender hat welche Logo-Adresse
//
// Eine Zeile je Sender: Stream-Adresse, Tabulator, Logo-Adresse. Geschrieben
// wird sie, sobald ein Logo geholt wurde. Gebraucht wird sie fuer Sender aus
// der lokalen Liste: dort stehen nur Name und Adresse, die Logo-Adresse kennt
// nur das Verzeichnis. Ohne diese Datei koennte ein gespeicherter Sender sein
// Bild nie nachholen - etwa nachdem /logos geloescht wurde oder wenn er noch
// aus einer aelteren Fassung in der Liste steht.
// ---------------------------------------------------------------------------
#define LOGO_INDEX LOGO_DIR "/index.txt"
#define LOGO_ZEILE (RB_URL_LEN * 2 + 4)

static bool logoIndexLookup(const char* url, char* iconOut, size_t cap) {
  iconOut[0] = '\0';
  if (!SD.exists(LOGO_INDEX)) return false;

  File f = SD.open(LOGO_INDEX);
  if (!f) return false;

  const size_t ulen = strlen(url);
  char zeile[LOGO_ZEILE];

  while (f.available()) {
    const size_t n = f.readBytesUntil('\n', zeile, sizeof(zeile) - 1);
    zeile[n] = '\0';
    if (!n) continue;

    char* tab = strchr(zeile, '\t');
    if (!tab) continue;
    *tab = '\0';

    if (strlen(zeile) == ulen && strcmp(zeile, url) == 0) {
      char* icon = tab + 1;
      char* e = icon + strlen(icon);
      while (e > icon && (e[-1] == '\r' || e[-1] == ' ')) *--e = '\0';
      strncpy(iconOut, icon, cap - 1);
      iconOut[cap - 1] = '\0';
      f.close();
      return iconOut[0] != '\0';
    }
  }
  f.close();
  return false;
}

static void logoIndexAdd(const char* url, const char* iconUrl) {
  if (!url || !url[0] || !iconUrl || !iconUrl[0]) return;

  char schon[RB_URL_LEN];
  if (logoIndexLookup(url, schon, sizeof(schon))) return;   // steht bereits da

  File f = SD.open(LOGO_INDEX, FILE_APPEND);
  if (!f) return;
  f.print(url);
  f.print('\t');
  f.print(iconUrl);
  f.print('\n');
  f.close();
}

static void logoFetch(const char* url, const char* iconUrl) {
  if (!url || !url[0]) return;
  if (WiFi.status() != WL_CONNECTED) { LOGO_SAG("kein WLAN\n"); return; }
  if (!SD.begin())                   { LOGO_SAG("keine SD\n"); return; }

  // Schon da? Es zaehlt **nur** die .565 - sie ist das Endformat. Ein liegen
  // gebliebenes png/jpg ist kein Grund mehr, den Abruf zu ueberspringen: es
  // wuerde ohnehin nie gezeichnet.
  char pfad565[80];
  logoPathFor(url, pfad565, sizeof(pfad565), "565");
  if (SD.exists(pfad565)) return;

  char pfad[80];
  logoPathFor(url, pfad, sizeof(pfad), "png");
  char pfadJpg[80];
  logoPathFor(url, pfadJpg, sizeof(pfadJpg), "jpg");
  if (SD.exists(pfadJpg)) return;

  // Keine Adresse zur Hand - Normalfall bei Sendern aus der lokalen Liste.
  // Dann im Index nachschlagen.
  char ausIndex[RB_URL_LEN];
  if (!iconUrl || !iconUrl[0]) {
    if (logoIndexLookup(url, ausIndex, sizeof(ausIndex))) {
      iconUrl = ausIndex;
      LOGO_SAG("Adresse aus dem Index\n");
    } else {
      LOGO_SAG("keine Adresse fuer dieses Logo\n");
      return;
    }
  }
  // Bis zum 19.8.2026 stand hier der https_Crack: die Adresse wurde auf
  // http heruntergeschrieben, und wer auf TLS bestand, lieferte eben kein
  // Logo. Von 39 geprueften Bild-CDNs waren so nur 15 nutzbar (38 %).
  //
  // Jetzt geht die Adresse, wie sie kam. Was ein TLS-Abruf kostet, ist
  // gemessen und bezahlbar geworden: 32 KB frei, bis 37 KB vom groessten
  // Block - gegen 86.004 Bytes, die die eigenen ESP-IDF-Bibliotheken am
  // Stueck stehen lassen. Der Ton ist waehrenddessen ohnehin abgebaut.
  LOGO_SAG("Adresse: %s\n", iconUrl);

  // Ohne TLS braucht der Abruf nur noch Socket, HTTPClient und den kleinen
  // Lesepuffer. Die alte Schwelle von 56.000 stammt vom TLS-Handschlag und
  // waere jetzt reine Schikane - sie hat Logos abgewiesen, die muehelos
  // durchgegangen waeren.
  // **Vor** dem Laden fragen, nicht danach. Am 18.8.2026 gelernt: die
  // Umrechnung wurde erst nach dem Download geprueft, und ein aussichtsloser
  // Versuch kostete acht Sekunden SD-Schreiben - bei **jedem** Senderstart
  // aufs Neue, weil nichts davon gemerkt wurde.
  //
  // Geprueft wird gegen den Bedarf des PNG-Dekoders, denn der ist der
  // groesste Einzelblock im ganzen Vorgang. Reicht der nicht, waere das Bild
  // ohnehin nur zum Wegwerfen geladen.
  const uint32_t block = ESP.getMaxAllocHeap();
  const uint32_t noetig = (INFO_NEED_HEAP > INFO_NEED_PNG) ? INFO_NEED_HEAP
                                                           : INFO_NEED_PNG;
  if (block < noetig) {
    LOGO_SAG("groesster Block nur %lu, brauche %lu - nichts geladen\n",
             (unsigned long)block, (unsigned long)noetig);
    return;
  }
  // Ab hier wird es langsam: Verbindung, Umleitung, Download. Genau jetzt -
  // und nur jetzt - lohnt der Hinweis. Alle Abbruchgruende oben (Bild liegt
  // schon da, keine Adresse, zu wenig Speicher) sind durch.
  if (logoHinweisErlaubt) logoHinweis();

  LOGO_SAG("Start, groesster Block %lu\n", (unsigned long)block);

  // Adresse, die sich unterwegs noch aendern kann.
  char adresse[RB_URL_LEN];
  strlcpy(adresse, iconUrl, sizeof(adresse));

  size_t geschrieben = 0;
  bool   istJpg = false;

  // Zwei Anlaeufe, und der zweite nur aus einem einzigen Grund: eine
  // Umleitung, die das **Schema wechselt**. HTTPClient folgt einer Umleitung
  // selbst, aber nur solange Protokoll und Client zusammenpassen -
  // setURL() lehnt einen Protokollwechsel ausdruecklich ab
  // (HTTPClient.cpp: "new URL not the same protocol"). Genau das ist bei
  // Logos der Regelfall: 23 von 39 Bild-CDNs schicken von http nach https.
  //
  // Also: Client nach Schema anlegen, und wenn eine Umleitung woandershin
  // zeigt, den Client wegwerfen und mit dem passenden noch einmal anklopfen.
  for (int versuch = 0; versuch < 2 && !geschrieben; versuch++) {
    const bool tls = (strncasecmp(adresse, "https://", 8) == 0);

    // WiFiClientSecure ist ein NetworkClientSecure; setInsecure() heisst
    // kein Zertifikatsspeicher - dieselbe Wahl wie ueberall sonst im Radio.
    WiFiClient* ein = tls ? new WiFiClientSecure() : new WiFiClient();
    if (!ein) return;
    if (tls) {
      static_cast<WiFiClientSecure*>(ein)->setInsecure();
      // 10 s statt der voreingestellten 120 - siehe clientForUrl() im
      // Bibliothekspatch. Ein schweigender Bildserver darf keinen
      // Senderstart blockieren.
      static_cast<WiFiClientSecure*>(ein)->setHandshakeTimeout(10);
    }

    // Ziel einer Umleitung mit Schemawechsel, sonst leer.
    char weiter[RB_URL_LEN];
    weiter[0] = '\0';

    {
      HTTPClient h;
      h.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
      h.setConnectTimeout(LOGO_HOLEN_MS);
      h.setTimeout(LOGO_HOLEN_MS);

      // Aussagekraeftiger User-Agent, seit dem 19.8.2026. Ohne den weist
      // upload.wikimedia.org mit **400** ab (am Geraet gemessen, 511 ms) -
      // deren Richtlinie verlangt einen, der Werkzeug und Herkunft nennt.
      // Wikimedia ist bei radio-browser eine haeufige Logo-Quelle, das war
      // also kein Einzelfall. Andere Server stoert die Zeile nicht.
      h.setUserAgent("Cardputer-WebRadio/3.3 "
                     "(+https://github.com/halbeshuhn/Cardputer-WebRadio)");
      // "Location" ist neu: bei einem Schemawechsel steht das Ziel dort,
      // und HTTPClient reicht die 3xx an uns durch, statt ihr zu folgen.
      const char* keys[] = { "Content-Type", "Location" };
      h.collectHeaders(keys, 2);

      if (h.begin(*ein, adresse)) {
        const uint32_t t0 = millis();
        const int code = h.GET();
        // Der String von header() ist nicht zu vermeiden - HTTPClient hat
        // keine char*-Variante und legt die Kopfzeilen intern selbst als
        // String ab. Was sich vermeiden laesst, ist seine LEBENSDAUER: frueher
        // lebte er von hier bis zum Blockende, also durch den ganzen Download
        // hindurch. In dieser Zeit steht der TLS-Kontext und die SD-Karte wird
        // beschrieben - jede Belegung dieses Fensters landet UEBER dem Block
        // und sperrt ihn ein. Jetzt stirbt er noch vor dem SD.open().
        char ct[48];
        {
          String t = h.header("Content-Type");
          t.toLowerCase();
          strlcpy(ct, t.c_str(), sizeof(ct));
        }
        LOGO_SAG("HTTP %d, \"%s\", %d angekuendigt, %lu ms\n", code, ct,
                 h.getSize(), (unsigned long)(millis() - t0));

        // Umleitung, der HTTPClient nicht folgen konnte. Nur vollstaendige
        // Adressen mit anderem Schema; ein relativer Pfad oder dasselbe
        // Schema waere schon intern erledigt worden.
        if (code == 301 || code == 302 || code == 303
            || code == 307 || code == 308) {
          char loc[RB_URL_LEN];
          {
            String t = h.header("Location");
            strlcpy(loc, t.c_str(), sizeof(loc));
          }
          const bool locHttps = (strncasecmp(loc, "https://", 8) == 0);
          const bool locHttp  = (strncasecmp(loc, "http://",  7) == 0);
          if ((locHttps || locHttp) && locHttps != tls) {
            strlcpy(weiter, loc, sizeof(weiter));
          }
        }

        // Erst fragen, dann schreiben. **Nur png und jpeg**, nicht "image"
        // allgemein - der Dekoder kann nichts anderes, und was er nicht kann,
        // muss gar nicht erst geladen werden.
        //
        // Am 18.8.2026 am Geraet aufgefallen: Rock Antenne liefert ein
        // favicon.ico (image/vnd.microsoft.icon). Die alte Pruefung auf "image"
        // liess es durch, 15.086 Bytes wanderten auf die Karte - **neun
        // Sekunden lang** -, danach erkannte die Bytepruefung weder PNG noch
        // JPEG und warf alles weg. Gemerkt wurde nichts, also lief dasselbe bei
        // **jedem** Senderstart wieder. Neun Sekunden vor dem Ton, jedes Mal.
        //
        // Jetzt endet es nach der Kopfzeile, ohne ein einziges Byte.
        const bool typOk = strstr(ct, "image/png") || strstr(ct, "image/jpeg")
                        || strstr(ct, "image/jpg");
        if (code == HTTP_CODE_OK && !typOk) {
          LOGO_SAG("Typ \"%s\" wird nicht unterstuetzt, nichts geladen\n", ct);
        }
        if (code == HTTP_CODE_OK && typOk) {
          File f = SD.open(pfad, FILE_WRITE);
          if (f) {
            // Erwartete Groesse; meldet der Server keine, wird mit 40 KB
            // gerechnet - der Balken laeuft dann eben ungenau.
            const uint32_t ganz = (h.getSize() > 0) ? (uint32_t)h.getSize() : 40000;
            uint32_t naechste = 0;
            // Eigene Schleife mit kleinem Puffer statt writeToStream(): das
            // will sich 1.460 Bytes am Stueck nehmen und ist am 14.8.2026 bei
            // knappem Speicher genau daran haengengeblieben.
            uint8_t brocken[512];
            WiFiClient* st = h.getStreamPtr();
            const uint32_t frist = millis() + 8000;
            while (st && (long)(millis() - frist) < 0) {
              const size_t da = st->available();
              if (da) {
                const int n = st->readBytes(brocken,
                                            da > sizeof(brocken) ? sizeof(brocken) : da);
                if (n <= 0) break;
                f.write(brocken, n);
                geschrieben += n;
                if (geschrieben >= naechste) naechste = geschrieben + 2048;
                if (geschrieben > LOGO_MAX_BYTES) break;

                // **Fertig ist fertig.** Ohne diese Zeile endete die Schleife
                // erst mit der Frist: der Server haelt die Verbindung offen
                // (keep-alive), available() liefert 0, connected() bleibt wahr,
                // und es wurde acht Sekunden lang mit delay(2) gewartet, obwohl
                // alles laengst da war. Am 18.8.2026 am Geraet gemessen -
                // Kopfzeile nach 75 ms, "fertig" nach neun Sekunden. Bei jedem
                // einzelnen Logo.
                //
                // Nur wenn der Server eine Laenge angekuendigt hat. Ohne die
                // bleibt es bei der Frist als Notbremse.
                if (h.getSize() > 0 && geschrieben >= ganz) break;
              } else if (!h.connected()) {
                break;
              } else {
                delay(2);
              }
            }
            // Kennung merken, bevor die Datei zugeht
            f.flush();
            f.close();
          }
        }
        h.end();
      } else {
        LOGO_SAG("begin() abgelehnt\n");
      }
    }


    delete ein;

    // Nichts geholt, aber ein Ziel mit anderem Schema in der Hand: noch
    // einmal, diesmal mit dem passenden Client. Sonst ist hier Schluss.
    if (!geschrieben && weiter[0]) {
      LOGO_SAG("Umleitung mit Schemawechsel -> %s\n", weiter);
      strlcpy(adresse, weiter, sizeof(adresse));
      continue;
    }
    break;
  }

  LOGO_SAG("%u Bytes geschrieben, Block wieder %lu\n", (unsigned)geschrieben,
           (unsigned long)ESP.getMaxAllocHeap());

  if (!geschrieben) { SD.remove(pfad); return; }

  // PNG oder JPEG? Steht in den ersten beiden Bytes. Ein JPEG bekommt die
  // andere Endung, damit der Info-Schirm den richtigen Decoder nimmt.
  {
    File f = SD.open(pfad);
    if (f) {
      uint8_t k[2] = { 0, 0 };
      f.read(k, 2);
      f.close();
      istJpg = (k[0] == 0xFF && k[1] == 0xD8);
      if (!istJpg && !(k[0] == 0x89 && k[1] == 'P')) {
        SD.remove(pfad);          // weder das eine noch das andere
        return;
      }
    }
  }
  if (istJpg) {
    SD.remove(pfadJpg);
    SD.rename(pfad, pfadJpg);
  }

  // Einmal rechnen, dann nur noch lesen. Das Zwischenbild fliegt danach von
  // der Karte - es wird nie wieder gebraucht, und wer es liegen liesse,
  // haette am Ende zwei Dateien je Sender.
  //
  // Geht die Umrechnung schief, wird auch das Zwischenbild geloescht und
  // **nichts** in den Index geschrieben: dann versucht es der naechste
  // Senderstart noch einmal, statt auf einer halben Ablage sitzenzubleiben.
  const char* quelle = istJpg ? pfadJpg : pfad;
  if (!logoZu565(quelle, pfad565, !istJpg)) {
    SD.remove(quelle);
    LOGO_SAG("Umrechnung fehlgeschlagen, nichts abgelegt\n");
    return;
  }
  SD.remove(quelle);

  logoIndexAdd(url, iconUrl);
  // Die Meldung nennt die .565, denn nur die bleibt liegen - das
  // Zwischenbild ist ein paar Zeilen weiter oben geloescht worden. Vorher
  // stand hier der png/jpg-Pfad, und das war schlicht gelogen.
  LOGO_SAG("fertig: %s bleibt liegen, im Index vermerkt\n", pfad565);
}

// Startet den aktuell gewaehlten Sender und setzt die Ueberwachung auf.
// Wird auch von R und vom Senderwechsel benutzt.
void Playfile(bool mitBalken = true) {
  audio.stopSong();

  currentStreamTitle[0] = '\0';
  streamTitleChanged = true;

  // Merker des vorigen Senders verwerfen - sonst schriebe ein liegen
  // gebliebener ICY-Name den gerade gesetzten neuen wieder zu.
  pendingStationChanged = false;
  pendingId3Changed = false;

  streamState = STREAM_CONNECTING;

  const char* url = extStationActive ? extStationUrl
                                     : stations[curStation].url;

  // Logo holen, solange der Ton aus ist und der Heap sauber - siehe
  // logoFetch(). Beim zweiten Besuch desselben Senders faellt das weg, dann
  // liegt die Datei auf der Karte.
  // Der Hinweis steht ueber den ganzen Vorgang: Logo holen **und** verbinden.
  // Der Waechter, der stumm gewordene Sender von selbst neu aufbaut, zeigt ihn
  // nicht - sonst blitzte er alle paar Sekunden auf.
  // Nur merken, nicht zeigen. Gezeigt wird erst, wenn wirklich etwas dauert -
  // also in logoFetch(), unmittelbar bevor es ins Netz geht. Beim gewoehnlichen
  // Senderwechsel und beim Start liegt das Logo schon auf der Karte, dann
  // erscheint gar nichts und der Wechsel bleibt so schnell wie vorher.
  logoHinweisErlaubt = mitBalken;
  logoFetch(url, extStationActive ? extStationIcon : "");

  // Erst **jetzt** die Uhr des Waechters starten, nicht vor logoFetch().
  //
  // Sonst zaehlt das Logoholen als Verbindungszeit mit: TLS-Handschlag und
  // Download koennen Sekunden dauern, und danach war die Frist von
  // STREAM_TIMEOUT_MS schon fast abgelaufen, bevor das Verbinden ueberhaupt
  // begann. Der Waechter erklaerte den Sender fuer tot, die Fusszeile meldete
  // einen Fehlschlag,
  // und der Wiederholversuch baute den gerade laufenden Ton neu auf - das
  // war das Knacken beim **ersten** Aufruf eines Senders. Beim zweiten liegt
  // das Logo auf der Karte, logoFetch() kehrt sofort zurueck, und man sah
  // nie etwas davon. Am 17.8.2026 am Geraet nachgestellt.
  streamStartMs = millis();

  bool connected = true;

  if (strstr(url, "http")) {
    connected = audio.connecttohost(url);

    // Die Uhr des Waechters **nach** dem Verbindungsaufbau neu stellen.
    //
    // STREAM_TIMEOUT_MS sind 5 s und meinen laut eigener Beschreibung "wie
    // lange nach dem Verbindungsaufbau auf Audiodaten gewartet wird" - bis
    // heute lief der Aufbau aber mit darin. Seit dem Vorfuellen (bis zu 3 s)
    // und dem TLS-Handschlag waere davon fast nichts uebrig: ein gesunder
    // Sender bekaeme "Stream unavailable", und der Waechter risse den gerade
    // aufgebauten Ton wieder ein. Dieselbe Falle wie am 17.8.2026 mit
    // logoFetch(), nur eine Stelle weiter.
    //
    // Verloren geht dadurch nichts: connecttohost() kehrt erst zurueck, wenn
    // es fertig ist, und ein Fehlschlag wird gleich darunter an connected
    // erkannt - nicht ueber diese Frist.
    streamStartMs = millis();
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

  // Verbindung schon beim Aufbau gescheitert - kein Warten noetig. Der
  // groesste freie Block sagt, ob es am Speicher lag; gemessen wird sofort,
  // spaeter hat sich der Heap schon wieder erholt.
  if (!connected) {
    // Der Wächter stammt aus der Zeit von ESP32-audioI2S, als der AAC-Decoder
    // sich seinen Block aus dem Heap holte. Seit AudioCompat.h steht er in
    // einem festen Feld - der groesste freie Block sagt darueber nichts mehr,
    // und die Abfrage meldete "Low RAM - codec too big" bei Sendern, denen
    // nichts fehlte.
    streamFailedLowMem = false;
    streamFailedPlaylist = audio.wasPlaylist();
    streamFailedHls = audio.wasHls();
    streamFailedStalled = false;
    streamState = STREAM_FAILED;
  }


  // Die Anzeigeflaeche muss neu, aus zwei Gruenden: der Hinweis hat sie
  // ueberschrieben, oder der Info-Schirm zeigt noch das Logo des vorigen
  // Senders. Playfile() fasst sie sonst nicht an - Kopf und Fusszeile ja,
  // die Mitte nicht.
  clearContentArea();
  drawVisFrame();

  showStation();
  drawFooter();
}

// Ueberwacht einen laufenden Verbindungsversuch: Sobald der Decoder eine
// Bitrate meldet, fliessen Audiodaten. Bleibt das aus, gilt der Stream
// nach STREAM_TIMEOUT_MS als nicht erreichbar.
// Ein- und ausschalten. Der Menueeintrag ist ein Umschalter: laeuft er schon,
// beendet derselbe Eintrag ihn wieder.
// Laeuft aus loop(). Schaltet weiter, wenn der Takt abgelaufen ist, und
// schreibt vorher die Zeile mit den Zahlen - noch mit dem Speicherstand des
// gerade laufenden Senders, denn genau der ist die Messung.
void updateStreamStatus() {
  // Schlaeft das Geraet, ist der angehaltene Stream Absicht. Ohne diese
  // Zeile erklaerte der Waechter ihn fuer tot und baute ihn nach wenigen
  // Sekunden neu auf - das Radio wuerde sich selbst wecken.
  if (sleepState == SLEEP_ASLEEP) return;

  // Die Sammelsperre in AudioCompat.h hat aufgegeben: der Sender lief, dann
  // kam AC_SAMMEL_TOT_MS lang nichts mehr. Sie hat den Ton schon beendet,
  // hier bekommt es nur noch einen Namen. Nicht auf STREAM_LOST_MS warten -
  // der Befund liegt vor, und er ist genauer als "keine Daten seit 8 s".
  if (audio.takeStalled()) {
    streamFailedLowMem = false;
    streamFailedPlaylist = false;
    streamFailedHls = false;
    streamFailedStalled = true;
    streamState = STREAM_FAILED;
    streamRetryAt = millis() + STREAM_RETRY_MS;
    drawFooter();
    return;
  }

  if (streamState == STREAM_CONNECTING) {
    if (audio.isRunning() && audio.getBitRate() > 0) {
      streamState = STREAM_OK;
      streamAliveMs = millis();
      streamRetries = 0;
      drawFooter();
      return;
    }

    if (millis() - streamStartMs > STREAM_TIMEOUT_MS) {
      // Der Wächter stammt aus der Zeit von ESP32-audioI2S, als der AAC-Decoder
    // sich seinen Block aus dem Heap holte. Seit AudioCompat.h steht er in
    // einem festen Feld - der groesste freie Block sagt darueber nichts mehr,
    // und die Abfrage meldete "Low RAM - codec too big" bei Sendern, denen
    // nichts fehlte.
    streamFailedLowMem = false;
      streamFailedPlaylist = false;
      streamFailedHls = false;
      streamFailedStalled = false;
      streamState = STREAM_FAILED;
      streamRetryAt = millis() + STREAM_RETRY_MS;
      drawFooter();
    }
    return;
  }

  // Laufender Stream: bis v3.1.2 hat hier niemand mehr hingesehen. Faellt die
  // Quelle aus, blieb der Zustand auf OK stehen - kein Ton, keine Meldung,
  // kein zweiter Versuch, im Mitschnitt vom 10.8.2026 76 Sekunden lang.
  if (streamState == STREAM_OK) {
    if (audio.isRunning() && audio.getBitRate() > 0) {
      streamAliveMs = millis();      // es fliesst, alles gut
      return;
    }

    if (millis() - streamAliveMs > STREAM_LOST_MS) {
      streamFailedLowMem = false;    // kein Speicherproblem, die Quelle fehlt
      streamFailedPlaylist = false;
      streamFailedHls = false;
      streamFailedStalled = false;
      streamState = STREAM_FAILED;
      streamRetryAt = millis() + STREAM_RETRY_MS;
      drawFooter();
    }
    return;
  }

  // Gescheitert: von selbst wieder versuchen, mit wachsendem Abstand. Ohne
  // WLAN hat das keinen Zweck - dann wartet erst wifiKeepAlive().
  if (streamState == STREAM_FAILED) {
    if ((long)(millis() - streamRetryAt) < 0) return;
    if (WiFi.status() != WL_CONNECTED) {
      streamRetryAt = millis() + STREAM_RETRY_MS;
      return;
    }

    if (streamRetries < 255) streamRetries++;

    // 5 s, 10 s, 20 s, dann alle 40 s. Ein Sender, der laenger weg ist,
    // kommt auch nicht durch haeufigeres Klopfen zurueck.
    uint32_t wait = STREAM_RETRY_MS;
    for (uint8_t i = 1; i < streamRetries && wait < STREAM_RETRY_MAX_MS; i++) {
      wait *= 2;
    }
    if (wait > STREAM_RETRY_MAX_MS) wait = STREAM_RETRY_MAX_MS;
    streamRetryAt = millis() + wait;

    Playfile(false);   // Waechter: ohne Balken, das laeuft von selbst
  }
}

// Lautstaerke intern 0..255, die Library kennt nur 0..21.
// Ein Griff an den Regler: von hier laeuft der Ausschlag weich zum neuen
// Wert, und der Atem faengt an. Steht bewusst nicht in showVolume() - das
// wird auch beim Neuaufbau des Schirms gerufen, und dann soll nichts pulsen.
static void volTouch() {
  volFrom = (volShown < 0) ? (float)curVolume : volShown;
  volMoveStart  = millis();
  volLastChange = millis();
}

void volumeUp() {
  if (curVolume >= 255) return;
  volTouch();

  if (curVolume + VOLUME_STEP > 255)
    curVolume = 255;
  else
    curVolume += VOLUME_STEP;

  audio.setVolume(curVolume);   // 0..255, die Kennlinie liegt in AudioCompat.h
  showVolume();
}

void volumeDown() {
  if (curVolume == 0) return;
  volTouch();

  if (curVolume <= VOLUME_STEP)
    curVolume = 0;
  else
    curVolume -= VOLUME_STEP;

  audio.setVolume(curVolume);   // 0..255, die Kennlinie liegt in AudioCompat.h
  showVolume();
}

bool isMuted = false;
uint16_t prevVolume = 0;

// Stummschaltung. Achtung: wer waehrend der Stummschaltung die Lautstaerke
// veraendert, verliert die Aenderung beim Aufheben wieder.
void volumeMute() {
  volTouch();
  if (!isMuted) {
    prevVolume = curVolume;
    curVolume = 0;
    isMuted = true;
  } else {
    curVolume = prevVolume;
    isMuted = false;
  }
  audio.setVolume(curVolume);   // 0..255, die Kennlinie liegt in AudioCompat.h
  showVolume();
}

// Mini-Skala der Lautstaerke links in der Batteriezeile, Aufbau siehe die
// VOL_-Defines oben. Zeichnet immer alle Punkte neu, aber nur wenn sich die
// Lautstaerke geaendert hat - 183 Pixel sind schnell gesetzt.
// Malt die Skala mit dem Stand, der gerade angezeigt werden soll.
static void volDrawScale() {
  const uint16_t colGrid = UI_SCALE_GRID;
  const uint16_t colOn   = UI_SCALE_ON;

  const uint8_t amp = volAmp();
  const uint8_t p   = (uint8_t)((uint32_t)uiBreathLevel() * amp / 255);
  const uint16_t colBar = uiMixColor(VOL_C_ON, VOL_C_MID, p);

  // Farbe der Marke, einmal als Kanaele - die Verbindung zwischen ihren
  // beiden Punkten wird daraus mit amp gegen Schwarz geblendet.
  const uint8_t strich[3] = { uiLerp(VOL_C_ON[0], VOL_C_TOP[0], p),
                              uiLerp(VOL_C_ON[1], VOL_C_TOP[1], p),
                              uiLerp(VOL_C_ON[2], VOL_C_TOP[2], p) };
  const uint8_t schwarz[3] = { 0, 0, 0 };
  const uint16_t colTop  = M5Cardputer.Display.color565(strich[0], strich[1],
                                                        strich[2]);
  const uint16_t colLink = uiMixColor(schwarz, strich, amp);

  const int filled = ((int)lroundf(volShown) * VOL_DOTS + 127) / 255;

  const int last    = VOL_DOTS - 1;
  const int quarter = last / 4;
  const int half    = last / 2;
  const int three   = (3 * last) / 4;

  const int yTop = VOL_BASE_Y - (VOL_ROWS - 1) * VOL_ROW_PITCH;

  // Die Striche wandern mit. Wo sie vorher standen, muss die Verbindung
  // zwischen den Punkten weg - die Skala malt dort nichts, sie bliebe sonst
  // stehen. Nur bei Wechsel loeschen, sonst waere es ein Loeschen je Bild.
  int mx[2] = { -1, -1 };
  if (amp && filled > 0) {
    for (int k = 0; k < 2; k++)
      if (filled - 1 - k >= 0) mx[k] = VOL_DOT_L + (filled - 1 - k) * VOL_DOT_PITCH;
  }
  for (int k = 0; k < 2; k++) {
    if (volMarkX[k] >= 0 && volMarkX[k] != mx[k]) {
      M5Cardputer.Display.fillRect(volMarkX[k], yTop + 1, 1,
                                   VOL_ROW_PITCH - 1, TFT_BLACK);
    }
    volMarkX[k] = mx[k];
  }

  for (int i = 0; i < VOL_DOTS; i++) {
    const int x = VOL_DOT_L + i * VOL_DOT_PITCH;

    // Hoehe der Dauermarke an dieser Spalte: 3 an den Enden, 2 bei den
    // Viertelmarken, sonst 1 - die durchgehende Linie ganz unten.
    int mark = 1;
    if (i == 0 || i == last) mark = VOL_ROWS;
    else if (i == quarter || i == half || i == three) mark = 2;

    for (int r = 0; r < VOL_ROWS; r++) {
      const int y = VOL_BASE_Y - r * VOL_ROW_PITCH;

      uint16_t col = colGrid;
      if (i < filled || r < mark) {
        // Ausschlag und Dauermarke leuchten gleich hell. Der Ausschlag atmet
        // beim Regeln mit, Raender und Grundlinie nicht.
        col = (i < filled && i != 0 && i != last && r != 0) ? colBar : colOn;
      }
      M5Cardputer.Display.drawPixel(x, y, col);
    }
  }

  // Zuletzt die beiden Striche.
  for (int k = 0; k < 2; k++) {
    if (mx[k] < 0) continue;
    M5Cardputer.Display.drawPixel(mx[k], yTop, colTop);
    M5Cardputer.Display.drawPixel(mx[k], yTop + VOL_ROW_PITCH, colTop);
    M5Cardputer.Display.fillRect(mx[k], yTop + 1, 1, VOL_ROW_PITCH - 1, colLink);
  }
}

void showVolume() {
  if (volShown < 0) volShown = (float)curVolume;   // beim ersten Mal ohne Lauf
  lastVolumeDrawn = (int)curVolume;
  volDrawScale();
}

// Laeuft aus loop(): Nachlauf des Ausschlags und der Atem waehrend des
// Regelns. Ausserhalb der Nachhallzeit passiert hier nichts.
void updateVolume() {
  if (uiOverlayActive()) return;
  if (volShown < 0) return;

  bool noetig = false;

  const float ziel = (float)curVolume;
  if (volShown != ziel) {
    const uint32_t dt = millis() - volMoveStart;
    if (dt >= VOL_FADE_MS) {
      volShown = ziel;
    } else {
      const float f = (float)dt / (float)VOL_FADE_MS;
      volShown = volFrom + (ziel - volFrom) * f * (2.0f - f);
    }
    noetig = true;
  }

  const uint8_t amp = volAmp();
  const uint8_t lvl = amp ? uiBreathLevel() : 255;
  if (lvl != volBreathLast) { volBreathLast = lvl; noetig = true; }

  if (noetig) volDrawScale();
}

void volumeUp();
void volumeDown();

// Gedrueckt gehaltene Pfeiltaste am Lautstaerkeregler, wie unter Windows.
// Im Equalizer gehoeren die Pfeile dem EQ, dort haelt sich das hier heraus.
void volHandleRepeat() {
  if (visMode == VIS_EQ) return;
  if (uiOverlayActive()) return;

  char k = 0;
  if      (M5Cardputer.Keyboard.isKeyPressed(';')) k = ';';
  else if (M5Cardputer.Keyboard.isKeyPressed('.')) k = '.';

  if (k == 0) { volHeldKey = 0; return; }

  if (k != volHeldKey) {
    volHeldKey  = k;
    volHeldNext = millis() + VOL_REPEAT_DELAY_MS;
    return;
  }

  if ((int32_t)(millis() - volHeldNext) < 0) return;
  volHeldNext = millis() + VOL_REPEAT_RATE_MS;

  if (k == ';') volumeUp(); else volumeDown();
}

// Naechster Sender, laeuft am Ende der Liste wieder auf den ersten.
void stationUp() {
  if (numStations > 0) {
    currentStreamTitle[0] = '\0';
    streamTitleChanged = true;
    extStationActive = false;   // zurueck auf die Liste von SD

    curStation = (curStation + 1) % numStations;
    audio.stopSong();
    Playfile(true);
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
    Playfile(true);
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
  free(rbIcons); rbIcons = NULL;
  rbCount = 0;
}

static bool rbAllocTables() {
  if (rbNames && rbUrls) return true;

  rbNames = (char (*)[RB_NAME_LEN])calloc(RB_MAX_NAMES, RB_NAME_LEN);
  rbUrls  = (char (*)[RB_URL_LEN]) calloc(RB_MAX_NAMES, RB_URL_LEN);
  // Dritte Tabelle seit dem 15.8.2026: 11 mal 256 Bytes fuer die
  // Logoadressen. Vor dem SBR-Ausbau waeren das 2,8 KB gewesen, die es
  // nicht gab.
  rbIcons = (char (*)[RB_URL_LEN]) calloc(RB_MAX_NAMES, RB_URL_LEN);

  if (!rbNames || !rbUrls || !rbIcons) {
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

  if (rbStFilter[0]) {
    strncat(dst, "&name=", cap - strlen(dst) - 1);
    rbAppendEncoded(dst, cap, rbStFilter);
  }
}

// Ein Abruf gegen einen bestimmten Server. Den zweiten Versuch steuert
// rbFetch() weiter unten.
static void rbFetchOnce(const char* host, bool anhaengen = false) {
  if (!anhaengen) {
    rbCount = 0;
    rbSel = 0;
    rbTop = 0;
  }
  rbRaw   = 0;             // zaehlt je Anfrage, nicht je Seite
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

  // HLS-Sender liefern statt Ton eine Liste von Haeppchen - das Geraet kann
  // sie nicht abspielen. Das Verzeichnis kennzeichnet sie selbst. In Taiwan
  // sind es 18 Prozent und fuenf der ersten elf, in Deutschland 9 von 2678.
  //
  // Achtung, Reihenfolge: "hls" steht im Datensatz **hinter** Name und
  // Adresse (Feld 21 von 40). Der Eintrag ist also schon abgelegt, wenn die
  // Angabe kommt, und wird dann wieder zurueckgenommen.
  // Reihenfolge im Datensatz: name, url, url_resolved, homepage, favicon,
  // ... , hls. Das Logo kommt also **nach** der Adresse - der Datensatz ist
  // da schon abgelegt, es gehoert zu rbCount-1. Genau wie bei hls.
  static const char patIcon[] = "\"favicon\":\"";
  static const char patHls[]  = "\"hls\":";
  const int patNameLen = sizeof(patName) - 1;
  const int patUrlLen  = sizeof(patUrl) - 1;
  const int patIconLen = sizeof(patIcon) - 1;
  const int patHlsLen  = sizeof(patHls) - 1;

  int  matchName = 0;
  int  matchUrl  = 0;
  int  matchIcon = 0;
  int  matchHls  = 0;
  bool readHls   = false;   // Muster erkannt, jetzt kommt die Ziffer
  bool justStored = false;  // der letzte Datensatz wurde wirklich abgelegt
  bool inName    = false;
  bool inUrl     = false;
  bool inIcon    = false;
  bool escaped   = false;
  int  hexLeft   = 0;
  uint16_t uni   = 0;        // sammelt die vier Ziffern eines \uXXXX
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
      if (!inName && !inUrl && !inIcon) {
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

        // Die Ziffer hinter "hls": entscheidet ueber den eben abgelegten
        // Datensatz. 1 heisst Haeppchenliste - dann wird er zurueckgenommen,
        // der naechste ueberschreibt seinen Platz.
        if (readHls) {
          if (c == ' ') continue;             // Leerzeichen ueberspringen
          readHls = false;

          if (justStored) {
            justStored = false;
            if (c == '1' && rbCount > 0) rbCount--;
          }
          if (rbCount >= RB_MAX_NAMES) { done = true; break; }
          continue;
        }

        // Logoadresse. Nur mitnehmen, wenn schon ein Datensatz steht.
        if (c == patIcon[matchIcon]) {
          if (++matchIcon == patIconLen) {
            matchIcon = 0;
            if (rbCount > 0) { inIcon = true; len = 0; rbIcons[rbCount-1][0] = '\0'; }
          }
        } else {
          matchIcon = (c == patIcon[0]) ? 1 : 0;
        }

        if (c == patHls[matchHls]) {
          if (++matchHls == patHlsLen) { matchHls = 0; readHls = true; }
        } else {
          matchHls = (c == patHls[0]) ? 1 : 0;
        }
        continue;
      }

      char* dst     = inName  ? rbNames[rbCount]
                    : inIcon  ? rbIcons[rbCount - 1]
                              : rbUrls[rbCount];
      const int cap = inName ? RB_NAME_LEN : RB_URL_LEN;

      // \uXXXX einsammeln und als UTF-8 ablegen. Frueher wurden die vier
      // Ziffern verworfen - damit war jeder chinesische, russische oder
      // griechische Sendername leer oder eine Reihe Fragezeichen (Meldung #1
      // im Repository).
      if (hexLeft > 0) {
        uint8_t v = 0;
        if      (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        uni = (uint16_t)((uni << 4) | v);

        if (--hexLeft == 0) {
          // Ersatzpaare koennen wir nicht abbilden - dort steht Zeichenware
          // jenseits der Grundebene, die keine unserer Schriften hat.
          if (uni >= 0xD800 && uni <= 0xDFFF) {
            if (len < cap - 1) dst[len++] = '?';
          } else if (uni < 0x80) {
            if (len < cap - 1) dst[len++] = (char)uni;
          } else if (uni < 0x800) {
            if (len < cap - 2) {
              dst[len++] = (char)(0xC0 | (uni >> 6));
              dst[len++] = (char)(0x80 | (uni & 0x3F));
            }
          } else {
            if (len < cap - 3) {
              dst[len++] = (char)(0xE0 | (uni >> 12));
              dst[len++] = (char)(0x80 | ((uni >> 6) & 0x3F));
              dst[len++] = (char)(0x80 | (uni & 0x3F));
            }
          }
        }
        continue;
      }

      if (escaped) {
        escaped = false;
        char out = '?';
        if (c == '"' || c == '\\' || c == '/') out = c;
        else if (c == 'n' || c == 't') out = ' ';
        else if (c == 'u') { hexLeft = 4; uni = 0; continue; }
        if (len < cap - 1) dst[len++] = out;
        continue;
      }

      if (c == '\\') { escaped = true; continue; }

      if (c == '"') {
        dst[len] = '\0';

        if (inName) {
          inName = false;
          uiDecodeEntities(dst);  // manche Namen tragen &#NNNN;
          uiTrimUtf8(dst);        // war der Name laenger als das Feld
        } else if (inIcon) {
          inIcon = false;         // Logoadresse steht, sonst aendert sich nichts
        } else {
          inUrl = false;

          // Hier stand bis zum 13.8.2026 eine Doppelerkennung ueber den
          // Namen. Sie ist weg, und zwar aus gutem Grund: gleicher Name heisst
          // nicht gleicher Sender. In Deutschland haben **228 von 346**
          // gleichnamigen Eintraegen verschiedene Adressen - "Sunshine Live"
          // dreimal mit verschiedenen Bitraten, "Rock Antenne" viermal,
          // darunter ein AAC-Strom. Wer nach dem Namen aussortiert, wirft die
          // Auswahl weg und behaelt womoeglich die tote Adresse. Lieber
          // zweimal derselbe Name als ein Sender, der nicht spielt.
          rbRaw++;
          rbCount++;
          justStored = true;      // bleibt, bis "hls" darueber entschieden hat

          // Kein Abbruch an dieser Stelle: erst muss die hls-Angabe dieses
          // Datensatzes gelesen sein, sonst bliebe womoeglich ein
          // unspielbarer Sender als letzter Eintrag stehen.
        }
        continue;
      }

      // Alles ab 0x20 durchlassen, auch die Folgebytes von UTF-8 (>= 0x80).
      // Weggeworfen werden nur Steuerzeichen.
      if (len < cap - 1 && (uint8_t)c >= 0x20) dst[len++] = c;
    }

    const uint32_t heap = ESP.getFreeHeap();
    if (heap < rbHeapLow) rbHeapLow = heap;
  }

  client.stop();

  // Ob das die letzte Seite war, entscheidet rbFetchPage() - dort laufen die
  // Nachforderungen zusammen.

  rbMillis = millis() - t0;
  rbHeapAfter = ESP.getFreeHeap();

#if DEBUG_SERIAL
  // Die vier Marken stehen auch auf dem Schirm - dort aber nur als freier
  // Heap. Entscheidend ist der GROESSTE BLOCK: mbedTLS braucht zwei Flaechen
  // von je gut 16 KB am Stueck, und die kann es auch dann nicht geben, wenn
  // rechnerisch genug frei ist. Deshalb hier beide Zahlen nebeneinander.
  Serial.printf("[rb] frei: lief %lu > gestoppt %lu > tief %lu > danach %lu | "
                "groesster Block jetzt %lu | %lu Bytes in %lu ms, Status %d\n",
                (unsigned long)rbHeapBefore, (unsigned long)rbHeapStopped,
                (unsigned long)rbHeapLow,    (unsigned long)rbHeapAfter,
                (unsigned long)ESP.getMaxAllocHeap(),
                (unsigned long)rbBytes, (unsigned long)rbMillis, rbStatus);
#endif
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
static void rbFetch(bool anhaengen = false) {
  rbFetchOnce(RB_HOST, anhaengen);
  if (!rbWorthRetry()) return;

  delay(RB_RETRY_PAUSE_MS);
  rbFetchOnce(RB_HOST2, anhaengen);
}

// Eine volle Seite zusammentragen.
//
// Der HLS-Filter nimmt Eintraege heraus, und in Taiwan sind das auf der
// ersten Seite acht von elf - die Seite zeigte dann drei Sender und sah aus
// wie das Ende der Liste. Also wird nachgefordert, bis elf beisammen sind
// oder der Dienst nichts mehr hat.
//
// Weitergezaehlt wird dabei nicht in Seiten, sondern in **verbrauchten
// Datensaetzen**: die naechste Seite beginnt dort, wo diese aufgehoert hat.
// Sonst wuerde uebersprungen oder doppelt gezeigt, je nachdem wie viel der
// Filter wegnimmt.
#define RB_REFILL_MAX 4          // hoechstens so viele Anfragen je Seite

static void rbFetchPage() {
  const int start = rbPageOff[rbPageNo];
  int verbraucht = 0;

  for (int runde = 0; runde < RB_REFILL_MAX; runde++) {
    rbOffset = start + verbraucht;
    rbFetch(runde > 0);
    verbraucht += rbRaw;

    if (rbStatus != 200 && rbStatus != 0) break;   // Stoerung: nicht weiter
    if (rbCount >= RB_MAX_NAMES) break;            // Seite ist voll
    if (rbRaw < RB_MAX_NAMES) {                    // der Dienst ist am Ende
      rbTotalPages = rbPageNo + 1;
      break;
    }
  }

  rbPageUsed = verbraucht;
  rbOffset   = start;             // fuer Anzeige und Aufbau der URL
}


// Hinweis waehrend des Abrufs - der dauert je nach Netz eine Weile.
static void rbShowFetching() {
  M5Cardputer.Display.fillScreen(TFT_BLACK);
  M5Cardputer.Display.setFont(FOOTER_FONT);
  M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5Cardputer.Display.drawString("radio-browser.info ...", UI_MARGIN, 60);
}

// Zahl der Zeilen und Beschriftung der gerade gezeigten Ebene.
// Passt der Name? Teilzeichenkette, Gross und Klein egal.
// Die Senderliste laeuft durchgehend in der grossen Schrift, gleich ob
// chinesische Namen dabei sind oder nicht. Zwei Zeilenhoehen im selben Bild
// waren ein Mischmasch, und die kleine Schrift war ohnehin zu klein.
// Laender- und Regionsliste bleiben elfzeilig - dort stehen nur ASCII-Namen.
#define RB_LINE_H_UNI 15
#define RB_VISIBLE_UNI 6

static int rbLineH()   { return (rbPage == RB_PAGE_STATIONS) ? RB_LINE_H_UNI  : RB_LINE_H; }
static int rbVisible() { return (rbPage == RB_PAGE_STATIONS) ? RB_VISIBLE_UNI : RB_VISIBLE; }

static bool rbFilterHit(const char* name) {
  if (!rbFilter[0]) return true;

  for (const char* p = name; *p; p++) {
    const char* a = p;
    const char* b = rbFilter;
    while (*b && *a && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
      a++; b++;
    }
    if (!*b) return true;
  }
  return false;
}

// Nach jedem Tastendruck neu. 241 Vergleiche sind auf diesem Geraet nichts,
// eine Liste im Speicher waere teurer als die Rechnung.
static void rbFilterApply() {
  rbFiltCount = 0;
  for (int i = 0; i < RB_COUNTRY_COUNT; i++) {
    if (rbFilterHit(rbCountries[i].name)) rbFiltIdx[rbFiltCount++] = (uint8_t)i;
  }
  rbSel = 0;
  rbTop = 0;
}

static int rbPageCount() {
  switch (rbPage) {
    case RB_PAGE_COUNTRY: return rbFiltCount;
    default:              return rbCount;
  }
}

static const char* rbPageLabel(int i) {
  switch (rbPage) {
    case RB_PAGE_COUNTRY:
      return rbCountries[rbFiltIdx[i]].name;
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
    default:
      snprintf(dst, cap, T(STR_RB_TITLE_S), c.name, rbOffset + 1);
      break;
  }
}

// Das Suchfeld der Laenderseite. Es steht dort, wo auf den anderen Ebenen die
// Tastenhilfe steht - zwei Zeilen, umrahmt. Rechts die Zahl der Treffer.
#define RB_BOX_X   2
#define RB_BOX_Y 111
#define RB_BOX_W 236
#define RB_BOX_H  22
#define RB_BOX_TY (RB_BOX_Y + 6)

static void rbDrawSearchBox(const char* text, int treffer) {
  const uint16_t head = M5Cardputer.Display.color565(120, 165, 210);
  const uint16_t dim  = M5Cardputer.Display.color565(70, 78, 86);

  M5Cardputer.Display.drawRect(RB_BOX_X, RB_BOX_Y, RB_BOX_W, RB_BOX_H, dim);
  M5Cardputer.Display.setFont(FOOTER_FONT);

  if (text[0]) {
    M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5Cardputer.Display.drawString(text, RB_BOX_X + 5, RB_BOX_TY);
  } else {
    M5Cardputer.Display.setTextColor(dim, TFT_BLACK);
    M5Cardputer.Display.drawString(T(STR_RB_SEARCH), RB_BOX_X + 5, RB_BOX_TY);
  }

  char n[8];
  snprintf(n, sizeof(n), "%d", treffer);
  M5Cardputer.Display.setTextColor(head, TFT_BLACK);
  M5Cardputer.Display.drawRightString(n, RB_BOX_X + RB_BOX_W - 5, RB_BOX_TY);

  M5Cardputer.Display.setFont(UI_FONT);
  M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
}

// Der blinkende Strich. Gezeichnet wird nur er selbst, nie die Zeile darum -
// die Lehre aus dem pulsierenden Equalizer-Band.
static void rbCaretTick() {
  const char* text = (rbPage == RB_PAGE_COUNTRY) ? rbFilter : rbStFilter;
  if (rbPage == RB_PAGE_STATIONS && !text[0]) return;   // Leiste ist aus
  if (millis() - rbCaretMs < 500) return;
  rbCaretMs = millis();
  rbCaretOn = !rbCaretOn;

  M5Cardputer.Display.setFont(FOOTER_FONT);
  const int x = RB_BOX_X + 5 + (text[0]
                ? (int)M5Cardputer.Display.textWidth(text) : 0);
  M5Cardputer.Display.fillRect(x, RB_BOX_TY, 1, 8,
                               rbCaretOn ? TFT_WHITE : TFT_BLACK);
  M5Cardputer.Display.setFont(UI_FONT);
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

  // Seitenzahl oben rechts. Die Gesamtzahl kennt der Dienst nicht - er
  // liefert nur die angeforderte Seite und sagt nichts ueber den Rest.
  // Sie steht deshalb erst fest, wenn eine Seite unvollstaendig zurueckkam;
  // bis dahin ein Fragezeichen.
  if (rbPage == RB_PAGE_STATIONS) {
    char pg[16];
    if (rbTotalPages > 0) snprintf(pg, sizeof(pg), "%d/%d", rbPageNo + 1, rbTotalPages);
    else                  snprintf(pg, sizeof(pg), "%d/?",  rbPageNo + 1);
    M5Cardputer.Display.drawRightString(
      pg, M5Cardputer.Display.width() - UI_MARGIN, 1);
  }

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
    for (int i = 0; i < rbVisible(); i++) {
      const int idx = rbTop + i;
      if (idx >= count) break;

      const int y = RB_TOP_Y + i * rbLineH();
      const char* label = rbPageLabel(idx);

      M5Cardputer.Display.setTextColor(
        idx == rbSel ? TFT_WHITE : M5Cardputer.Display.color565(120, 128, 128),
        TFT_BLACK);

      // Auf der Senderebene alles in der grossen Schrift - auch lateinische
      // Namen. Sonst staenden verschieden hohe Zeilen nebeneinander.
      if (rbPage == RB_PAGE_STATIONS) M5Cardputer.Display.setFont(uiUniFont);

      M5Cardputer.Display.drawString(idx == rbSel ? ">" : " ", UI_MARGIN, y);
      M5Cardputer.Display.drawString(label, UI_MARGIN + 8, y);

      if (rbPage == RB_PAGE_STATIONS) M5Cardputer.Display.setFont(FOOTER_FONT);
    }
  }

  // Fusszeilen: Tastenhilfe und, auf der Senderebene, die Messwerte.
  M5Cardputer.Display.setTextColor(head, TFT_BLACK);

  if (rbPage == RB_PAGE_STATIONS) {
    if (rbStFilter[0]) {
      rbDrawSearchBox(rbStFilter, rbCount);   // Leiste an derselben Stelle
      rbCaretOn = false;
    } else {
      // Die Zeile mit Senderzahl, Dauer und Heap stand hier bis zum
      // 13.8.2026. Sie war fuer die Fehlersuche beim Abruf gedacht und hat
      // auf einem fertigen Geraet nichts verloren; der freie Platz tut dem
      // Bild gut.
      M5Cardputer.Display.drawString(T(STR_RB_FOOT_PLAY), UI_MARGIN, 124);
    }
  } else if (rbPage == RB_PAGE_COUNTRY) {
    rbDrawSearchBox(rbFilter, rbFiltCount);
    rbCaretOn = false;                    // frisch gezeichnet, Strich ist weg
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
  if (rbSel >= rbTop + rbVisible()) rbTop = rbSel - rbVisible() + 1;
}

// Senderseite holen: Stream anhalten (sonst reicht der Heap nicht),
// abrufen, Stream wieder aufnehmen. Siehe Kommentar an rbFetch().
static void rbLoadStations(bool neu = true) {
  if (neu) {
    rbPageNo     = 0;
    rbPageOff[0] = 0;
    rbTotalPages = -1;
  }

  rbShowFetching();

  rbHeapBefore = ESP.getFreeHeap();
  audio.stopSong();
  delay(50);
  rbHeapStopped = ESP.getFreeHeap();

  rbAllocTables();
  rbFetchPage();

  // **Kein Playfile hier.** Bis zum 18.8.2026 lief der Ton nach jedem Abruf
  // wieder an und wurde beim naechsten Blaettern erneut abgewuergt. Jetzt
  // bleibt der Audiozweig die ganze Listensitzung ueber abgebaut: der 44-KB-
  // Vorpuffer, die Decoder und die I2S-Deskriptoren sind waehrend des Suchens
  // frei. Gestartet wird erst beim Verlassen - mit dem gewaehlten Sender
  // (rbPlaySelected) oder mit dem vorherigen (rbBackToMenu/rbBackToRadio).
  rbPage = RB_PAGE_STATIONS;
  rbSel = 0;
  rbTop = 0;
  drawRbList();
}

// Raeumt die Flaeche des groesstmoeglichen Fensters frei. Beim Wechsel der
// Ebene ist das neue Fenster womoeglich kleiner und wuerde sonst Reste des
// alten stehenlassen. Kopfbereich und Footer bleiben dabei stehen.
// ---------------------------------------------------------------------------
// Sleeptimer: Einstellschirm, Ablauf, Einschlafen und Wecken
//
// Die Ziffern stehen in Font0 Groesse 3 statt der 2 des Menues - genau 50 %
// groesser, also 18 statt 12 px je Stelle - und werden einzeln gesetzt, damit
// zwischen ihnen Luft bleibt und jede Stelle ihren eigenen Strich bekommen
// kann. Die beiden Stundenziffern teilen sich einen durchgehenden.
// ---------------------------------------------------------------------------
#define SLEEP_DIG_W    18    // Font0 Groesse 3: 6 px mal 3
#define SLEEP_DIG_H    24
#define SLEEP_DIG_GAP   5    // Abstand zwischen den Stellen
#define SLEEP_FIELDS    3    // Stunden, Minutenzehner, Minuteneiner

static uint16_t sleepEditMinutes() {
  return (uint16_t)sleepEditH * 60 + (uint16_t)sleepEditM10 * 10 + sleepEditM1;
}

// Ueber 12:00 geht es nicht. Statt umzubrechen bleiben die Minuten stehen -
// dieselbe Regel wie an den Enden der Equalizerregler.
static void sleepEditClamp() {
  if (sleepEditH >= 12) { sleepEditH = 12; sleepEditM10 = 0; sleepEditM1 = 0; }
}

static void drawSleepEditor() {
  const int h = 2 * SYS_MENU_PAD + SYS_MENU_LINE_H + SLEEP_DIG_H + 10;
  const int x = (240 - SYS_MENU_W) / 2;
  const int y = (135 - h) / 2;

  const uint16_t frame = M5Cardputer.Display.color565(120, 165, 210);

  M5Cardputer.Display.fillRect(x - 2, y - 2, SYS_MENU_W + 4, h + 4, TFT_BLACK);
  M5Cardputer.Display.drawRect(x, y, SYS_MENU_W, h, frame);

  setMenuFont();
  M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  const int ty = y + SYS_MENU_PAD;
  M5Cardputer.Display.drawString(T(STR_MENU_SLEEP), x + SYS_MENU_PAD, ty);

  // Fuenf Zeichen: zwei Stunden, Doppelpunkt, zwei Minuten.
  char stellen[5] = { (char)('0' + sleepEditH / 10), (char)('0' + sleepEditH % 10),
                      ':', (char)('0' + sleepEditM10), (char)('0' + sleepEditM1) };

  M5Cardputer.Display.setTextSize(3);
  const int breite = 5 * SLEEP_DIG_W + 4 * SLEEP_DIG_GAP;
  const int x0 = x + (SYS_MENU_W - breite) / 2;
  const int zy = ty + SYS_MENU_LINE_H + 2;

  for (int i = 0; i < 5; i++) {
    char buf[2] = { stellen[i], '\0' };
    M5Cardputer.Display.drawString(buf, x0 + i * (SLEEP_DIG_W + SLEEP_DIG_GAP), zy);
  }
  M5Cardputer.Display.setTextSize(1);

  // Der Strich: ueber beide Stundenziffern hinweg, bei den Minuten je Stelle.
  int ux, uw;
  if (sleepEditField == 0) {
    ux = x0;
    uw = 2 * SLEEP_DIG_W + SLEEP_DIG_GAP;
  } else {
    ux = x0 + (2 + sleepEditField) * (SLEEP_DIG_W + SLEEP_DIG_GAP);
    uw = SLEEP_DIG_W;
  }
  M5Cardputer.Display.drawFastHLine(ux, zy + SLEEP_DIG_H + 2, uw, TFT_YELLOW);

  restoreUiFont();
  M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
}

// Pfeil hoch/runter auf dem gewaehlten Feld. Jede Stelle zaehlt fuer sich und
// bricht an ihrem Ende um - ein Uebertrag waere bei einem Stellenzaehler
// ueberraschend.
static void sleepEditAdjust(int delta) {
  switch (sleepEditField) {
    case 0: {
      int v = (int)sleepEditH + delta;
      if (v < 0)  v = 0;
      if (v > 12) v = 12;
      sleepEditH = (uint8_t)v;
      break;
    }
    case 1: {
      int v = (int)sleepEditM10 + delta;
      if (v < 0) v = 0;
      if (v > 5) v = 5;
      sleepEditM10 = (uint8_t)v;
      break;
    }
    default: {
      int v = (int)sleepEditM1 + delta;
      if (v < 0) v = 0;
      if (v > 9) v = 9;
      sleepEditM1 = (uint8_t)v;
      break;
    }
  }
  sleepEditClamp();
  drawSleepEditor();
}

static void sleepEditMove(int delta) {
  const int f = sleepEditField + delta;
  if (f < 0 || f >= SLEEP_FIELDS) return;    // an den Enden stehenbleiben
  sleepEditField = (int8_t)f;
  drawSleepEditor();
}

// Stream anhalten, Schirm dunkel. Der Waechter haelt still, solange
// sleepState auf SLEEP_ASLEEP steht - siehe updateStreamStatus().
static void sleepFire() {
  audio.stopSong();

  sleepPrevBright = currentBrightnessIndex;
  currentBrightnessIndex = 0;                       // Stufe 0 ist "aus"
  M5Cardputer.Display.setBrightness(brightnessLevels[0]);

  sleepState = SLEEP_ASLEEP;
}

// Jede Taste weckt: Helligkeit zurueck auf die gemerkte Stufe, dann den
// Sender neu aufbauen. Playfile() setzt streamState auf STREAM_CONNECTING
// und zeichnet Kopf und Fusszeile - damit laeuft auch der Waechter wieder.
static void sleepWake() {
  if (sleepState != SLEEP_ASLEEP) return;

  currentBrightnessIndex = sleepPrevBright;
  M5Cardputer.Display.setBrightness(brightnessLevels[currentBrightnessIndex]);

  sleepState = SLEEP_OFF;
  lastDrawnSleepMin = -1;

  Playfile(true);
}

// Aus dem Menue heraus abbrechen. Schlaeft das Geraet schon, ist das zugleich
// das Wecken.
static void sleepStop() {
  if (sleepState == SLEEP_ASLEEP) { sleepWake(); return; }
  sleepState = SLEEP_OFF;
  lastDrawnSleepMin = -1;
}

static void sleepStart() {
  const uint16_t min = sleepEditMinutes();
  if (min == 0) return;                    // 00:00 ist keine Zeit
  sleepDeadline = millis() + (uint32_t)min * 60000UL;
  sleepState = SLEEP_RUNNING;
  lastDrawnSleepMin = -1;
}

// Verbleibende Minuten, aufgerundet: "00:01" heisst "weniger als eine Minute".
static uint16_t sleepMinutesLeft() {
  if (sleepState != SLEEP_RUNNING) return 0;
  const long rest = (long)(sleepDeadline - millis());
  if (rest <= 0) return 0;
  return (uint16_t)((rest + 59999L) / 60000L);
}

// Laeuft aus loop().
static void sleepTick() {
  if (sleepState != SLEEP_RUNNING) return;
  if ((long)(millis() - sleepDeadline) >= 0) sleepFire();
}

static void clearSysMenuArea() {
  // Die groesste Flaeche, die je gezeichnet wurde - sonst bleiben beim
  // Wechsel auf eine kleinere Seite Reste stehen. Seit der festen
  // Fensterhoehe ist das immer dieselbe; der Sleep-Schirm ist niedriger.
  const int h = SYS_MENU_H;
  const int x = (240 - SYS_MENU_W) / 2;
  const int y = (135 - h) / 2;

  M5Cardputer.Display.fillRect(x - 2, y - 2, SYS_MENU_W + 4, h + 4, TFT_BLACK);
}

// Das schwebende Fenster des Systemmenues, mittig auf der Anzeige. Zeichnet
// jedes Mal das ganze Fenster - bei einer Handvoll Eintraegen ist das
// billiger als das Nachfuehren einzelner Zeilen und kann nichts stehenlassen.
void drawSysMenu();

// Der weiche Lauf des Menues. Aus loop(), im selben Takt wie die
// auslaufenden Zeilen: ein Pixel alle UI_LAUF_STEP_MS.
void updateSysMenu() {
  if (!sysMenuActive || sysMenuPage == SYS_PAGE_SLEEP) return;
  if (sysMenuOff == sysMenuZiel) return;
  if (millis() - sysMenuStep < SYS_MENU_STEP_MS) return;
  sysMenuStep = millis();

  if (sysMenuZiel > sysMenuOff) {
    sysMenuOff += SYS_MENU_STEP_PX;
    if (sysMenuOff > sysMenuZiel) sysMenuOff = sysMenuZiel;
  } else {
    sysMenuOff -= SYS_MENU_STEP_PX;
    if (sysMenuOff < sysMenuZiel) sysMenuOff = sysMenuZiel;
  }
  drawSysMenu();
}

// Wohin gescrollt werden muss, damit die gewaehlte Zeile ganz im Fenster
// steht. In Pixeln, damit die Bewegung dazwischen weich laufen kann.
static int sysMenuZielFuer(int index) {
  const int count = sysMenuCount();
  const int hoch  = count * SYS_MENU_LINE_H - SYS_MENU_INNER;
  const int maxOff = (hoch > 0) ? hoch : 0;

  const int oben  = index * SYS_MENU_LINE_H;
  const int unten = oben + SYS_MENU_LINE_H;

  // Das Fenster zieht mit: die gewaehlte Zeile steht oben, bis die Liste zu
  // Ende ist. Ein Schritt nach unten schiebt also genau eine Zeile weg -
  // die Rechnung haengt nur am Index, nicht am gerade laufenden Stand.
  int off = oben;
  if (off > maxOff) off = maxOff;
  if (unten - off > SYS_MENU_INNER) {
    off = unten - SYS_MENU_INNER;
    if (off > maxOff) off = maxOff;
  }
  if (off < 0) off = 0;
  return off;
}

void drawSysMenu();

// Auswahl verschieben. **Kein Umbruch**: oben und unten bleibt sie stehen.
// Ein Sprung vom letzten Eintrag zurueck zum ersten waere beim Halten der
// Taste nicht zu beherrschen.
void sysMenuMove(int delta) {
  const int count = sysMenuCount();
  if (count <= 0) return;

  const int neu = sysMenuIndex + delta;
  if (neu < 0 || neu >= count) return;      // am Anschlag
  sysMenuIndex = neu;

  sysMenuZiel = sysMenuZielFuer(sysMenuIndex);
  sysMenuStep = millis();
  drawSysMenu();
}

// Gehaltene Pfeiltaste im Menue, wie ueberall sonst: erst Pause, dann Schritt
// um Schritt.
static char     sysHeldKey = 0;
static uint32_t sysHeldNext = 0;

void sysMenuHandleRepeat() {
  if (!sysMenuActive || sysMenuPage == SYS_PAGE_SLEEP) return;

  char k = 0;
  if      (M5Cardputer.Keyboard.isKeyPressed(';')) k = ';';
  else if (M5Cardputer.Keyboard.isKeyPressed('.')) k = '.';

  if (k == 0) { sysHeldKey = 0; return; }

  if (k != sysHeldKey) {
    sysHeldKey  = k;
    sysHeldNext = millis() + EQ_REPEAT_DELAY_MS;
    return;
  }

  if ((int32_t)(millis() - sysHeldNext) < 0) return;
  sysHeldNext = millis() + EQ_REPEAT_RATE_MS;

  sysMenuMove(k == ';' ? -1 : +1);
}

void drawSysMenu() {
  if (sysMenuPage == SYS_PAGE_SLEEP) { drawSleepEditor(); return; }

  const int count = sysMenuCount();

  // Feste Hoehe, immer dieselbe. Passt alles hinein, wird nicht gescrollt -
  // sysMenuZielFuer() liefert dann null.
  const int h = SYS_MENU_H;
  const int x = (240 - SYS_MENU_W) / 2;
  const int y = (135 - h) / 2;

  const uint16_t frame = M5Cardputer.Display.color565(120, 165, 210);
  const uint16_t dim   = M5Cardputer.Display.color565(45, 60, 75);

  // Zwei Pixel Schwarz rings um den Rahmen setzen das Fenster vom Analyzer
  // ab, der dahinter weiterlaeuft.
  M5Cardputer.Display.fillRect(x - 2, y - 2, SYS_MENU_W + 4, h + 4, TFT_BLACK);
  M5Cardputer.Display.drawRect(x, y, SYS_MENU_W, h, frame);

  setMenuFont();

  // Der Inhalt laeuft unter dem Rahmen durch, deshalb beschnitten. Ohne das
  // stuenden die halben Zeilen ueber dem Rahmen und ausserhalb des Fensters.
  M5Cardputer.Display.setClipRect(x + 1, y + SYS_MENU_PAD,
                                  SYS_MENU_W - 2, SYS_MENU_INNER);

  // **Kein Loeschen des Innenraums.** Das hat beim Laufen geflimmert, aus
  // demselben Grund wie ueberall: zwischen dem Fuellen und dem Text ist die
  // Flaeche einen Moment leer. Font0 malt deckend, also raeumt jede Zeile
  // selbst - geloescht werden nur die schmalen Streifen ueber und unter der
  // Zeichenzelle und der Rest rechts vom Text.
  const int fh    = M5Cardputer.Display.fontHeight();
  const int innX  = x + 1;
  const int innW  = SYS_MENU_W - 2;
  const int textX = x + SYS_MENU_PAD + SYS_MENU_MARK_W;

  for (int i = 0; i < count; i++) {
    const int ly = y + SYS_MENU_PAD + i * SYS_MENU_LINE_H - sysMenuOff;
    if (ly + SYS_MENU_LINE_H < y || ly > y + h) continue;   // ausserhalb

    const int ty = ly + (SYS_MENU_LINE_H - fh) / 2;

    // Die Marke deckt ihre Zelle selbst: gewaehlt ">", sonst ein Leerzeichen.
    M5Cardputer.Display.setTextColor(
        (i == sysMenuIndex) ? TFT_WHITE : dim, TFT_BLACK);
    M5Cardputer.Display.drawString((i == sysMenuIndex) ? ">" : " ",
                                   x + SYS_MENU_PAD, ty);

    const char* lab = sysMenuLabel(i);
    M5Cardputer.Display.drawString(lab, textX, ty);

    // Rest rechts vom Text - die Eintraege sind verschieden lang.
    const int end = textX + M5Cardputer.Display.textWidth(lab);
    if (end < innX + innW) {
      M5Cardputer.Display.fillRect(end, ty, innX + innW - end, fh, TFT_BLACK);
    }

    // Die Streifen ueber und unter der Zeichenzelle. Beim Laufen bleiben
    // dort sonst Reste der vorigen Lage stehen.
    if (ty > ly) {
      M5Cardputer.Display.fillRect(innX, ly, innW, ty - ly, TFT_BLACK);
    }
    const int unten = ty + fh;
    if (unten < ly + SYS_MENU_LINE_H) {
      M5Cardputer.Display.fillRect(innX, unten, innW,
                                   ly + SYS_MENU_LINE_H - unten, TFT_BLACK);
    }
  }

  M5Cardputer.Display.clearClipRect();

  // Zeigt an, dass ueber oder unter dem Ausschnitt noch etwas steht. Zwei
  // kleine Dreiecke am rechten Rand, sie brauchen keine eigene Zeile.
  const int hoch   = count * SYS_MENU_LINE_H - SYS_MENU_INNER;
  const int maxOff = (hoch > 0) ? hoch : 0;
  const int mx = x + SYS_MENU_W - 9;
  if (sysMenuOff > 0) {
    M5Cardputer.Display.fillTriangle(mx, y + 6, mx + 6, y + 6, mx + 3, y + 2,
                                     frame);
  }
  if (sysMenuOff < maxOff) {
    M5Cardputer.Display.fillTriangle(mx, y + h - 6, mx + 6, y + h - 6,
                                     mx + 3, y + h - 2, frame);
  }

  restoreUiFont();
  M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
}

void openSysMenu() {
  sysMenuActive = true;
  sysMenuPage = SYS_PAGE_MAIN;
  sysMenuIndex = 0;
  sysMenuOff = 0;
  sysMenuZiel = 0;
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
// ---------------------------------------------------------------------------
// Charge Mode
//
// Fuer das Geraet am Ladegeraet: alles aus, was Strom zieht, und der Schirm
// dunkel. Vorbild ist der M5Launcher (settings.cpp, chargeMode()) - der
// nimmt CPU auf 80 MHz und dimmt. Hier geht es weiter: das WLAN wird
// abgeschaltet, und der Schirm geht nach CHARGE_SHOW_MS ganz aus.
//
// Jede Taste holt die Anzeige fuer weitere CHARGE_SHOW_MS zurueck.
// Beendet wird **nur mit BtnG0**, wie beim Launcher - eine versehentlich
// gestreifte Taste soll das Radio nicht wieder hochfahren.
//
// Das Abschalten des WLAN kostet beim Verlassen die uebliche Wartezeit fuer
// Verbindung und Senderstart. Ecki hat das ausdruecklich so gewollt:
// "Kann aus, max sparen".
// ---------------------------------------------------------------------------
#define CHARGE_SHOW_MS 5000
#define CHARGE_CPU_MHZ   80

static void chargeDrawLevel() {
  M5Cardputer.Display.fillScreen(TFT_BLACK);

  char t[8];
  snprintf(t, sizeof(t), "%d%%", M5.Power.getBatteryLevel());

  M5Cardputer.Display.setFont(&fonts::Font0);
  M5Cardputer.Display.setTextSize(4);
  M5Cardputer.Display.setTextColor(UI_SCALE_ON, TFT_BLACK);
  M5Cardputer.Display.drawCentreString(t, 120, 40);

  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.setTextColor(FOOTER_TEXT_COLOR, TFT_BLACK);
  M5Cardputer.Display.drawCentreString("Charge Mode - G0 exits", 120, 100);

  restoreUiFont();
  M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
}

static void chargeMode() {
  // Ton und Waechter zuerst: sonst baut der Streamwaechter den Sender im
  // Ladebetrieb immer wieder auf. Dieselbe Falle wie beim Sleeptimer.
  audio.stopSong();
  streamState = STREAM_CONNECTING;   // haelt den Waechter still, wie in
                                     // wifiActionBegin()

  // Trennen ohne den Netzeintrag des Treibers im geteilten NVS zu loeschen,
  // dann das Funkmodul ganz aus - der groesste Verbraucher.
  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_OFF);

  const uint32_t cpuVorher = getCpuFrequencyMhz();
  setCpuFrequencyMhz(CHARGE_CPU_MHZ);

  chargeDrawLevel();
  M5Cardputer.Display.setBrightness(brightnessLevels[currentBrightnessIndex]);

  bool     an   = true;
  uint32_t seit = millis();

  while (true) {
    M5Cardputer.update();

    if (M5Cardputer.BtnA.wasPressed()) break;

    // Jede Taste weckt die Anzeige. Der Druck selbst verstellt nichts.
    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
      if (!an) {
        chargeDrawLevel();
        M5Cardputer.Display.setBrightness(
            brightnessLevels[currentBrightnessIndex]);
        an = true;
      }
      seit = millis();
    }

    if (an && millis() - seit >= CHARGE_SHOW_MS) {
      M5Cardputer.Display.setBrightness(0);
      M5Cardputer.Display.fillScreen(TFT_BLACK);
      an = false;
    }

    delay(20);
  }

  // Zurueck: erst Takt und Schirm, dann Netz, dann Ton.
  setCpuFrequencyMhz(cpuVorher);
  M5Cardputer.Display.setBrightness(brightnessLevels[currentBrightnessIndex]);
  M5Cardputer.Display.fillScreen(TFT_BLACK);

  // BtnG0 hat gerade den Ladebetrieb beendet. connectToWiFi() fragt in den
  // ersten 2,5 s, ob BtnG0 gehalten wird - das ist der WLAN-Reset. Ohne das
  // Warten wuerde derselbe Druck die Zugangsdaten loeschen.
  while (M5Cardputer.BtnA.isPressed()) { M5Cardputer.update(); delay(20); }
  delay(300);

  WiFi.mode(WIFI_STA);
  connectToWiFi();          // blockiert, zeigt seine eigene Maske
  restoreUiFont();

  streamTitleChanged = true;
  redrawUI();
  Playfile(true);
}

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

  Playfile(true);
  redrawUI();
}

// Liste verlassen: zurueck ins Menue oder gleich zum Radio.
static void rbBackToMenu() {
  rbListActive = false;
  rbFreeTables();
  M5Cardputer.Display.fillScreen(TFT_BLACK);
  Playfile(true);            // der vorherige Sender, der Ton war ja aus
  openSysMenu();
}

static void rbBackToRadio() {
  rbListActive = false;
  rbFreeTables();
  closeSysMenu();
  Playfile(true);            // der vorherige Sender, der Ton war ja aus
}

// Den markierten Sender uebernehmen und abspielen. Er kommt nicht in die
// Liste von SD - er gilt nur, bis mit den Pfeiltasten oder ueber die
// Senderliste wieder ein eigener Sender gewaehlt wird.
static void rbPlaySelected() {
  if (!rbNames || !rbUrls) return;
  if (rbCount == 0 || rbUrls[rbSel][0] == '\0') return;

  strncpy(extStationName, rbNames[rbSel], sizeof(extStationName) - 1);
  extStationName[sizeof(extStationName) - 1] = '\0';

  strncpy(extStationIcon, rbIcons ? rbIcons[rbSel] : "", sizeof(extStationIcon) - 1);
  extStationIcon[sizeof(extStationIcon) - 1] = '\0';
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

  Playfile(true);
  redrawUI();
}

// ENTER: eine Ebene tiefer, auf der Senderebene abspielen.
static void rbEnter() {
  switch (rbPage) {
    case RB_PAGE_COUNTRY:
      rbCountryIdx = rbFiltIdx[rbSel];
      uiSetUniFontForCountry(rbCountries[rbCountryIdx].code);
      rbStFilter[0] = '\0';        // neue Suche je Land
      rbLoadStations();
      break;

    default:
      rbPlaySelected();
      break;
  }
}

// ESC / BS: eine Ebene zurueck, vom Land aus zurueck ins Menue.
// Vom Rueckweg gebraucht: die Laendernummer in die Zeile der gefilterten
// Liste umrechnen. Ohne das zeigte die Auswahl mit gesetztem Filter ins Leere -
// rbCountryIdx zaehlt die ganze Tabelle, die Liste nur noch die Treffer.
static void rbSelFromCountry() {
  for (int i = 0; i < rbFiltCount; i++) {
    if (rbFiltIdx[i] == (uint8_t)rbCountryIdx) { rbSel = i; return; }
  }
  rbSel = 0;
}

static void rbBack() {
  switch (rbPage) {
    case RB_PAGE_STATIONS:
      rbFreeTables();          // Tabellen werden hier nicht mehr gebraucht
      rbPage = RB_PAGE_COUNTRY;
      rbSelFromCountry();
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
  if (rbPage != RB_PAGE_STATIONS) { rbListJump(rbVisible()); return; }
  if (rbPageUsed <= 0) return;
  if (rbTotalPages > 0 && rbPageNo + 1 >= rbTotalPages) return;
  if (rbPageNo + 1 >= RB_MAX_PAGES) return;

  rbPageOff[rbPageNo + 1] = (uint16_t)(rbPageOff[rbPageNo] + rbPageUsed);
  rbPageNo++;
  rbLoadStations(false);
}

static void rbPrevPage() {
  if (rbPage != RB_PAGE_STATIONS) { rbListJump(-rbVisible()); return; }
  if (rbPageNo == 0) return;

  rbPageNo--;              // der Anfang dieser Seite steht in rbPageOff
  rbLoadStations(false);
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
    // Dieselbe Groesse wie die Onlineliste: durchgehend die grosse Schrift,
    // damit beide Listen sich gleich anfuehlen - und damit ein gesicherter
    // chinesischer Sender hier lesbar bleibt.
    M5Cardputer.Display.setFont(uiUniFont);

    for (int i = 0; i < RB_VISIBLE_UNI; i++) {
      const int idx = localTop + i;
      if (idx >= numStations) break;

      const int y = RB_TOP_Y + i * RB_LINE_H_UNI;

      M5Cardputer.Display.setTextColor(
        idx == localSel ? TFT_WHITE
                        : M5Cardputer.Display.color565(120, 128, 128),
        TFT_BLACK);

      M5Cardputer.Display.drawString(idx == localSel ? ">" : " ", UI_MARGIN, y);
      M5Cardputer.Display.drawString(stations[idx].name, UI_MARGIN + 8, y);
    }

    M5Cardputer.Display.setFont(FOOTER_FONT);
  }

  M5Cardputer.Display.setTextColor(head, TFT_BLACK);
  M5Cardputer.Display.drawString("Ok=Play, Back=rmv", UI_MARGIN, 114);
  M5Cardputer.Display.drawString(T(STR_LOC_FOOT), UI_MARGIN, 124);

  M5Cardputer.Display.setFont(UI_FONT);
  M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
}

static void localScrollIntoView() {
  if (localSel < localTop) localTop = localSel;
  if (localSel >= localTop + RB_VISIBLE_UNI) localTop = localSel - RB_VISIBLE_UNI + 1;
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

  Playfile(true);
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
// Die Online-Liste betreten. Beide Wege - Menuepunkt und Taste o - kommen
// hier zusammen, damit der Ton an genau einer Stelle abgebaut wird.
static void rbOpenCountryList() {
  audio.stopSong();          // Vorpuffer, Decoder und I2S sofort zurueck
  streamState = STREAM_CONNECTING;

  sysMenuActive = false;
  rbListActive = true;
  rbPage = RB_PAGE_COUNTRY;
  rbCountryIdx = 0;
  rbOffset = 0;
  rbFilter[0] = '\0';       // jedes Mal mit voller Liste anfangen
  rbFilterApply();           // setzt auch rbSel und rbTop
  drawRbList();
}

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
        rbOpenCountryList();
        break;

      case IT_LOCAL:                       // Lokale Liste
        openLocalList();
        break;

      case IT_SAVE:                        // laufenden Online-Sender sichern
        sysSaveOnlineStation();
        break;

      case IT_SLEEP:
        // Laeuft schon einer, ist derselbe Eintrag der Ausschalter. Sonst
        // geht es in den Einstellschirm.
        if (sleepState != SLEEP_OFF) {
          sleepStop();
          sysBuildMain();                  // die Beschriftung wechselt
          clearSysMenuArea();
          drawSysMenu();
        } else {
          sleepEditField = 0;
          sysMenuPage = SYS_PAGE_SLEEP;
          clearSysMenuArea();
          drawSysMenu();
        }
        break;

      case IT_CHARGE:                      // Ladebetrieb, alles aus
        closeSysMenu();
        chargeMode();
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

#if TLS_DAUERTOPF
// Lebt bis zum Ausschalten. Absichtlich nie freigegeben - das ist der Zweck.
static WiFiClientSecure* tlsDauer = nullptr;

// Einmal verbinden, damit mbedTLS seine Puffer anlegt. Danach nichts weiter:
// nicht lesen, nicht schliessen. Der Rueckgabewert interessiert nur fuer die
// Meldung; scheitert es, laeuft das Radio ohne Topf einfach weiter.
static void tlsDauertopfAnlegen() {
  const uint32_t vorFrei  = ESP.getFreeHeap();
  const uint32_t vorBlock = ESP.getMaxAllocHeap();

  tlsDauer = new WiFiClientSecure();
  if (!tlsDauer) {
#if DEBUG_SERIAL
    Serial.println("[tls] kein Speicher fuer den Client");
#endif
    return;
  }
  tlsDauer->setInsecure();            // kein Zertifikatsspeicher, wie ueberall
  tlsDauer->setTimeout(8);

  const uint32_t t0 = millis();
  const bool ok = tlsDauer->connect("www.google.com", 443);

  if (ok) {
    // Eine winzige Anfrage, damit der Handschlag wirklich durch ist.
    // /generate_204 antwortet mit einem leeren Rumpf.
    tlsDauer->print("GET /generate_204 HTTP/1.1\r\n"
                    "Host: www.google.com\r\n"
                    "Connection: keep-alive\r\n\r\n");
  } else {
    // Gescheitert: dann auch nichts festhalten.
    delete tlsDauer;
    tlsDauer = nullptr;
  }

#if DEBUG_SERIAL
  Serial.printf("[tls] Dauertopf %s nach %lu ms | frei %lu -> %lu (%ld) | "
                "groesster Block %lu -> %lu (%ld)\n",
                ok ? "steht" : "GESCHEITERT",
                (unsigned long)(millis() - t0),
                (unsigned long)vorFrei,  (unsigned long)ESP.getFreeHeap(),
                (long)ESP.getFreeHeap() - (long)vorFrei,
                (unsigned long)vorBlock, (unsigned long)ESP.getMaxAllocHeap(),
                (long)ESP.getMaxAllocHeap() - (long)vorBlock);
#endif
}
#endif

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

  // Die Audio-Bibliothek schreibt in der Vorgabe ins Leere
  // (AudioLogger.cpp: audioLogger = &silencedLogger). Deshalb blieb der
  // Hinweis "Out of memory error! hAACDecoder==NULL" unsichtbar, und der
  // fehlende Ton bei AAC war nur zu erraten. Ab jetzt kommt er hier an.
  audioLogger = &Serial;

  debugBootReport();
#endif

  M5Cardputer.Speaker.begin();
  M5Cardputer.Speaker.setVolume(255);

  M5Cardputer.Display.setBrightness(brightnessLevels[currentBrightnessIndex]);

  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.setFont(UI_FONT);

  // Sprache aus dem NVS holen, bevor der erste Text erscheint.
  langLoad();
  eqLoad();      // gespeicherte Klangkurve, sofort wirksam
  logoPrepareSd();  // /logos anlegen, Ersatzbild hineinlegen

  // Blockiert, bis eine Verbindung steht. BtnG0 setzt dabei das WLAN zurueck.
  // Die Masken darin stellen auf den Skalenfont in 1,5-facher Groesse um;
  // das gilt global und muss danach zurueck, sonst zeichnet die ganze
  // Oberflaeche zu gross.
  connectToWiFi();
  restoreUiFont();

#if TLS_DAUERTOPF
  // Genau hier und nirgends spaeter: der Heap ist noch unversehrt, die beiden
  // Puffer landen unten am Stueck. Nach dem ersten Playfile() waere die
  // Gelegenheit vorbei.
  tlsDauertopfAnlegen();
#endif

  // Muss vor dem ersten Verbinden stehen, sonst ist der Puffer schon da und
  // die Bibliothek lehnt die Aenderung mit einer Meldung ab. -1 fuer PSRAM
  // heisst "nicht anfassen" - vorhanden ist ohnehin keines.
#if AUDIO_INBUF_BYTES
  audio.setBufsize(AUDIO_INBUF_BYTES, -1);
#endif

  audio.setPinout(I2S_BCK, I2S_WS, I2S_DOUT);
  audio.setVolume(curVolume);   // 0..255, die Kennlinie liegt in AudioCompat.h
  audio.setBalance(0);

  M5Cardputer.Display.fillScreen(BLACK);

  audio.stopSong();
  mergeRadioStations();
  Playfile(true);
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
// Kleinster Fuellstand des Vorpuffers seit der letzten Heap-Zeile.
//
// Gemessen wird bei **jedem** Schleifendurchgang, nicht erst beim Schreiben:
// ein Loch von 200 ms waere zwischen zwei Zeilen im Abstand von 1,5 s sonst
// unsichtbar, und genau die kurzen Loecher sind das Stottern.
//
// 100 heisst auch "spielt gerade nicht" - bufferFillPct() liefert das, solange
// kein Puffer steht. Eine Zeile mit 100 % ist also nie ein Alarm, eine mit
// kleinen Zahlen dagegen immer.
static uint8_t pufferMinPct = 100;

static void debugPufferProbe() {
  const uint8_t p = audio.bufferFillPct();
  if (p < pufferMinPct) pufferMinPct = p;
}

static void debugHeapReport() {
  static uint32_t last = 0;
  if (millis() - last < DEBUG_HEAP_MS) return;
  last = millis();

  // Der Stapel steht mit dabei, seit der Absturz vom 12.8.2026 auf einen
  // verbogenen Zeiger in der Schleife zeigte, waehrend der Heap laut Pruefung
  // heil war. uxTaskGetStackHighWaterMark() nennt den kleinsten je freien
  // Rest in Woertern - faellt der Richtung null, ist der Stapel die Ursache.
  // Die neuen Schriften bauen jedes Zeichen mit alloca auf genau diesem
  // Stapel auf.
  Serial.printf("[heap] frei %lu  min %lu  groesster Block %lu  Stapel frei %lu"
                "  Puffer jetzt %u%% min %u%%\n",
                (unsigned long)ESP.getFreeHeap(),
                (unsigned long)ESP.getMinFreeHeap(),
                (unsigned long)ESP.getMaxAllocHeap(),
                (unsigned long)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)),
                (unsigned)audio.bufferFillPct(),
                (unsigned)pufferMinPct);

  // Zuruecksetzen auf den jetzigen Stand, nicht auf 100: sonst zeigte die
  // naechste Zeile ein Loch an, das schon in dieser stand.
  pufferMinPct = audio.bufferFillPct();
}

// ---------------------------------------------------------------------------
// Heappruefung
//
// Der Absturz vom 11.8.2026 lag in tlsf_malloc/remove_free_block, aufgerufen
// aus dem WLAN-Empfang. Dort faellt der Schaden nur auf: der Empfangstask
// legt am haeufigsten Puffer an und laeuft deshalb als Erster in eine
// zerstoerte Freiliste. Verursacht hat ihn jemand anders, Sekunden vorher.
//
// heap_caps_check_integrity_all() geht die Listen selbst durch. Weil die
// Bibliothek mit eingeschalteter Vergiftung uebersetzt ist - in der
// Rueckverfolgung stand multi_heap_poisoning.c - findet sie auch Schreiber
// knapp hinter einem Block. Damit schrumpft der Abstand zwischen Ursache und
// Meldung von Sekunden auf den Pruefabstand.
//
// print_errors = true laesst die Pruefung selbst ausgeben, welcher Block
// betroffen ist. Diese Zeilen sind der eigentliche Fund.
static void debugHeapCheck() {
  static uint32_t last = 0;
  static bool     broken = false;

  if (broken) return;   // einmal reicht, danach ist die Ausgabe nur Laerm
  if (millis() - last < DEBUG_CHECK_MS) return;
  last = millis();

  if (!heap_caps_check_integrity_all(true)) {
    broken = true;
    Serial.println("[heap] ### PRUEFUNG GESCHEITERT ###");
    Serial.printf("[heap] frei %lu  groesster Block %lu  Laufzeit %lu ms\n",
                  (unsigned long)ESP.getFreeHeap(),
                  (unsigned long)ESP.getMaxAllocHeap(),
                  (unsigned long)millis());
    Serial.printf("[heap] Zustand: %s, Bitrate %u, Anzeige %d\n",
                  audio.isRunning() ? "Wiedergabe" : "still",
                  (unsigned)audio.getBitRate(), (int)visMode);
  }
}

// Einmal beim Start: woher der Neustart kam und aus welcher Partition das
// Programm laeuft. Der Fehler tritt nur auf, wenn der M5Launcher das
// Programm geladen hat - die Partition benennt diesen Unterschied.
static void debugBootReport() {
  const char* r = "?";
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  r = "POWERON (Kaltstart, Kabel oder Schalter)"; break;
    case ESP_RST_SW:       r = "SW (esp_restart, z.B. aus dem Launcher)";  break;
    case ESP_RST_PANIC:    r = "PANIC (Absturz)";                          break;
    case ESP_RST_INT_WDT:  r = "INT_WDT";                                  break;
    case ESP_RST_TASK_WDT: r = "TASK_WDT";                                 break;
    case ESP_RST_WDT:      r = "WDT";                                      break;
    case ESP_RST_BROWNOUT: r = "BROWNOUT (Spannung eingebrochen)";         break;
    case ESP_RST_DEEPSLEEP:r = "DEEPSLEEP";                                break;
    case ESP_RST_EXT:      r = "EXT";                                      break;
    default:               r = "UNKNOWN";                                  break;
  }
  Serial.printf("[boot] Neustartgrund: %s\n", r);

  const esp_partition_t* p = esp_ota_get_running_partition();
  if (p) {
    Serial.printf("[boot] Partition: %s  Adresse 0x%06lX  Groesse %lu KB\n",
                  p->label, (unsigned long)p->address,
                  (unsigned long)(p->size / 1024));
  }

  // Der Heap zu diesem fruehen Zeitpunkt sagt, ob der Launcher etwas
  // hinterlassen hat. Ueber USB geflasht und ueber den Launcher gestartet
  // muessen hier dieselbe Zahl ergeben - tun sie es nicht, ist genau das
  // die Spur.
  Serial.printf("[boot] Heap beim Start: frei %lu  groesster Block %lu\n",
                (unsigned long)ESP.getFreeHeap(),
                (unsigned long)ESP.getMaxAllocHeap());

  Serial.printf("[boot] Heap zu Beginn unversehrt: %s\n",
                heap_caps_check_integrity_all(true) ? "ja" : "NEIN");
}
#endif

// Die Tastenkuerzel in einer eigenen Funktion, nicht im Tastenblock des
// Hauptschirms. Grund am 18.8.2026 am Geraet gelernt: der EQ-Schirm hat einen
// **eigenen** Tastenblock, der mit return endet - dort waren v, i, w, l, o
// und s tot, und aus dem Equalizer kam man mit v nicht mehr heraus.
// Liefert true, wenn eine Taste dabei war.
static bool uiShortcutKeys() {
  if (M5Cardputer.Keyboard.isKeyPressed('v')) { visCycleMeters(); return true; }
  if (M5Cardputer.Keyboard.isKeyPressed('i')) { visSet(VIS_INFO); return true; }
  if (M5Cardputer.Keyboard.isKeyPressed('e')) { visSet(VIS_EQ);   return true; }

  if (M5Cardputer.Keyboard.isKeyPressed('w')) {
    openSysMenu();                       // gleich in die WLAN-Ebene
    sysMenuPage = SYS_PAGE_WIFI;
    sysMenuIndex = 0;
    sysMenuOff = 0;
  sysMenuZiel = 0;
    clearSysMenuArea();
    drawSysMenu();
    return true;
  }
  if (M5Cardputer.Keyboard.isKeyPressed('l')) { openLocalList(); return true; }
  if (M5Cardputer.Keyboard.isKeyPressed('o')) { rbOpenCountryList(); return true; }
  if (M5Cardputer.Keyboard.isKeyPressed('s')) {
    // Laeuft schon einer, ist die Taste der Ausschalter - wie der
    // Menueeintrag, der seine Beschriftung wechselt.
    if (sleepState != SLEEP_OFF) {
      sleepStop();
    } else {
      openSysMenu();
      sleepEditField = 0;
      sysMenuPage = SYS_PAGE_SLEEP;
      clearSysMenuArea();
      drawSysMenu();
    }
    return true;
  }
  return false;
}

void loop() {
  audio.loop();       // muss oft laufen, sonst reisst die Wiedergabe ab
  M5Cardputer.update();

  // Sleeptimer. Erst pruefen, ob die Zeit um ist, dann das Wecken: schlaeft
  // das Geraet, holt **jede** Taste es zurueck - und dieser eine Druck tut
  // sonst nichts, sonst stellte das Aufwachen nebenbei den Sender um. Der
  // Rest der Schleife laeuft im Schlaf normal weiter, nur eben unsichtbar.
  sleepTick();
  if (sleepState == SLEEP_ASLEEP) {
    if (M5Cardputer.BtnA.wasPressed() ||
        (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed())) {
      sleepWake();
      lastButtonPress = millis();
      return;
    }
  }

#if DEBUG_SERIAL
  debugPufferProbe();     // muss vor dem Bericht stehen, er setzt zurueck
  debugHeapReport();
  debugHeapCheck();
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
    rbCaretTick();

    if (M5Cardputer.BtnA.wasPressed()) {
      rbBackToRadio();
      return;
    }

    if (M5Cardputer.Keyboard.isChange() &&
        millis() - lastButtonPress > DEBOUNCE_DELAY) {

      const int count = rbPageCount();

      // Reihenfolge ist wichtig: die Pfeiltasten sind auf diesem Geraet
      // Satzzeichen (; . , /). Sie werden zuerst abgefragt, damit sie auf der
      // Laenderseite nicht im Suchfeld landen. Ins Feld kommen nur Buchstaben,
      // Ziffern und Leerzeichen.
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
      else if (M5Cardputer.Keyboard.isKeyPressed('`')) {
        rbBack();
      }
      else if (rbPage == RB_PAGE_COUNTRY) {
        // Suchfeld. Backspace loescht ein Zeichen; ist das Feld leer, ist es
        // wieder der Rueckweg wie auf den anderen Ebenen.
        Keyboard_Class::KeysState st = M5Cardputer.Keyboard.keysState();
        bool changed = false;

        if (st.del) {
          // Backspace loescht hier nur Zeichen und ist **nie** der Rueckweg.
          // Wer beim schnellen Loeschen einmal zu oft drueckt, soll nicht aus
          // der Liste fliegen. Hinaus geht es mit ESC oder BtnG0.
          const size_t n = strlen(rbFilter);
          if (n > 0) { rbFilter[n - 1] = '\0'; changed = true; }
        } else {
          for (auto ch : st.word) {
            const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')
                         || (ch >= '0' && ch <= '9') || ch == ' ';
            if (!ok) continue;

            const size_t n = strlen(rbFilter);
            if (n + 1 < RB_FILTER_LEN) { rbFilter[n] = ch; rbFilter[n + 1] = '\0'; changed = true; }
          }
        }

        if (changed) { rbFilterApply(); drawRbList(); }
      }
      else if (rbPage == RB_PAGE_STATIONS) {
        // Suchleiste der Senderliste. Sie ist unsichtbar, solange nichts
        // getippt ist, und ihr Platz bleibt trotzdem frei - die Liste steht
        // also still, wenn sie erscheint. Gesucht wird beim Dienst, nicht
        // hier: im Geraet liegen immer nur elf Namen.
        Keyboard_Class::KeysState st = M5Cardputer.Keyboard.keysState();
        bool changed = false;

        if (st.del) {
          // Wie auf der Laenderseite: Backspace loescht nur Zeichen. Hinaus
          // geht es mit ESC oder mit BtnG0 zum Radio.
          const size_t n = strlen(rbStFilter);
          if (n > 0) { rbStFilter[n - 1] = '\0'; changed = true; }
        } else {
          for (auto ch : st.word) {
            const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')
                         || (ch >= '0' && ch <= '9') || ch == ' ';
            if (!ok) continue;

            const size_t n = strlen(rbStFilter);
            if (n + 1 < RB_FILTER_LEN) { rbStFilter[n] = ch; rbStFilter[n + 1] = '\0'; changed = true; }
          }
        }

        if (changed) rbLoadStations();   // jede Aenderung faengt vorn an
      }
      lastButtonPress = millis();
    }

    return;
  }

  // Systemmenue: schwebendes Fenster, nimmt alle Tasten fuer sich. Der Rest
  // der Schleife wird uebersprungen, damit nichts ins Fenster hineinzeichnet.
  if (sysMenuActive) {
    delay(1);

    // Der weiche Lauf und die gehaltene Pfeiltaste gehoeren hierher, nicht
    // ans Ende der Schleife: dieser Block endet mit return, alles danach
    // laeuft bei offenem Menue nie.
    updateSysMenu();
    sysMenuHandleRepeat();

    // BtnG0 fuehrt aus jeder Ebene sofort zum Radio zurueck.
    if (M5Cardputer.BtnA.wasPressed()) {
      closeSysMenu();
      return;
    }

    if (M5Cardputer.Keyboard.isChange() &&
        millis() - lastButtonPress > DEBOUNCE_DELAY) {

      // Der Einstellschirm des Sleeptimers nimmt die Pfeiltasten fuer sich:
      // hoch/runter aendert die Stelle, links/rechts wechselt das Feld.
      // Erlaubt bleiben Stummschaltung und Helligkeit, alles andere ruht.
      if (sysMenuPage == SYS_PAGE_SLEEP) {
        if (M5Cardputer.Keyboard.isKeyPressed(';')) sleepEditAdjust(+1);
        else if (M5Cardputer.Keyboard.isKeyPressed('.')) sleepEditAdjust(-1);
        else if (M5Cardputer.Keyboard.isKeyPressed('/')) sleepEditMove(+1);
        else if (M5Cardputer.Keyboard.isKeyPressed(',')) sleepEditMove(-1);
        else if (M5Cardputer.Keyboard.isKeyPressed('m')) volumeMute();
        else if (M5Cardputer.Keyboard.isKeyPressed('b')) toggleBrightness();
        else if (M5Cardputer.Keyboard.isKeyPressed(KEY_ENTER)) {
          sleepStart();                    // Feuer
          closeSysMenu();
        }
        else if (M5Cardputer.Keyboard.isKeyPressed('`') ||
                 M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE)) {
          sysMenuPage = SYS_PAGE_MAIN;     // ohne zu starten zurueck
          clearSysMenuArea();
          drawSysMenu();
        }

        lastButtonPress = millis();
        return;
      }

      const int count = sysMenuCount();

      if (M5Cardputer.Keyboard.isKeyPressed(';')) {          // hoch
        sysMenuMove(-1);
      }
      else if (M5Cardputer.Keyboard.isKeyPressed('.')) {     // runter
        sysMenuMove(+1);
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

  updateBatteryDisplay(5000);

  if (M5Cardputer.Keyboard.isChange() && (millis() - lastButtonPress > DEBOUNCE_DELAY)) {

    // Der Equalizer nimmt die Pfeiltasten fuer sich: hoch/runter ist der Wert,
    // links/rechts das Band. Lautstaerke und Sender liegen so lange still,
    // Stummschaltung und Helligkeit bleiben erreichbar, die Dateiwiedergabe
    // nicht - sie wuerde den halben Schirm neu zeichnen.
    if (visMode == VIS_EQ) {
      if (M5Cardputer.Keyboard.isKeyPressed(';')) eqAdjust(+1);       // hoch
      else if (M5Cardputer.Keyboard.isKeyPressed('.')) eqAdjust(-1);  // runter
      else if (M5Cardputer.Keyboard.isKeyPressed('/')) eqSelect(+1);  // rechts
      else if (M5Cardputer.Keyboard.isKeyPressed(',')) eqSelect(-1);  // links
      else if (M5Cardputer.Keyboard.isKeyPressed('0')) eqZeroAll();
      else if (M5Cardputer.Keyboard.isKeyPressed('m')) volumeMute();
      else if (M5Cardputer.Keyboard.isKeyPressed('b')) toggleBrightness();
      else if (M5Cardputer.Keyboard.isKeyPressed('f')) toggleVuMode();
      else if (uiShortcutKeys()) { }     // v i e w l o s auch von hier aus

      lastButtonPress = millis();
      return;
    }

    if (M5Cardputer.Keyboard.isKeyPressed(';')) volumeUp();        // hoch
    else if (M5Cardputer.Keyboard.isKeyPressed('.')) volumeDown(); // runter
    else if (M5Cardputer.Keyboard.isKeyPressed('m')) volumeMute();
    else if (M5Cardputer.Keyboard.isKeyPressed('/')) stationUp();  // rechts
    else if (M5Cardputer.Keyboard.isKeyPressed(',')) stationDown(); // links
    else if (M5Cardputer.Keyboard.isKeyPressed('r')) {
      Playfile(true);
    }
    else if (M5Cardputer.Keyboard.isKeyPressed('f')) {
      toggleVuMode();
    }
    else if (M5Cardputer.Keyboard.isKeyPressed('b')) {
      toggleBrightness();
    }

    else if (uiShortcutKeys()) { }        // v i e w l o s, siehe oben

    lastButtonPress = millis();

   }

  // Alle folgenden Aufrufe bremsen sich selbst und zeichnen nur bei Bedarf.
  wifiKeepAlive();
  updateStreamStatus();
  drawWifi();
  drawBitrate();
  updateSleepFooter();
  updateVuMeter();
  updateSpectrum();
  updateEq();
  eqHandleRepeat();
  updateVolume();
  volHandleRepeat();
  updateInfoScreen();
  updateInfoUrl();
  updateHeaderName();
  eqSaveIfIdle();

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

    // Erst die Verweise aufloesen, dann pruefen: manche Sender schicken
    // ASCII mit &#NNNN; darin, das soll durchkommen.
    char probe[sizeof(pendingStation)];
    strncpy(probe, showstation, sizeof(probe) - 1);
    probe[sizeof(probe) - 1] = '\0';
    uiDecodeEntities(probe);
    uiTrimUtf8(probe);

    if (!uiValidUtf8(probe)) return;   // Big5 und Konsorten: alten Namen behalten

    strncpy(pendingStation, probe, sizeof(pendingStation) - 1);
    pendingStation[sizeof(pendingStation) - 1] = '\0';
    pendingStationChanged = true;
}

// Rueckruf der Library: neuer Titel. Wird nur uebernommen, gezeichnet wird
// spaeter in drawStreamTitle().
void audio_showstreamtitle(const char *info) {
  if (!info || !*info) return;

  strncpy(currentStreamTitle, info, sizeof(currentStreamTitle) - 1);
  currentStreamTitle[sizeof(currentStreamTitle) - 1] = '\0';
  uiDecodeEntities(currentStreamTitle);
  uiTrimUtf8(currentStreamTitle);
  streamTitleChanged = true;
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

    uiDrawHeaderName(pendingStation);
  }

  if (pendingId3Changed) {
    pendingId3Changed = false;

    // ID3 und Streamtitel teilten sich frueher eine Bildschirmzeile und
    // ueberschrieben sich gegenseitig. Jetzt gibt es nur noch einen Text:
    // die ID3-Angabe wird der Titel und wandert damit in denselben Block.
    strncpy(currentStreamTitle, pendingId3, sizeof(currentStreamTitle) - 1);
    currentStreamTitle[sizeof(currentStreamTitle) - 1] = '\0';
    uiDecodeEntities(currentStreamTitle);   // wie beim Streamtitel
    uiTrimUtf8(currentStreamTitle);
    drawTitleBlock();
  }
}

// Laeuft in jedem Durchlauf der Hauptschleife. Die rote Linie wird dabei
// nachgezogen; der Titel selbst nur, wenn ein neuer angekommen ist. Bis
// v3.1.1 stand hier eine Laufschrift, die bei jedem Pixel neu zeichnete.
void drawStreamTitle() {
  if (uiOverlayActive()) return;

  M5Cardputer.Display.fillRect(
    0, HDR_RULE_Y, M5Cardputer.Display.width(), 1, TFT_RED);

  if (streamTitleChanged) {
    streamTitleChanged = false;
    drawTitleBlock();     // prueft selbst, ob die Anzeige leer ist
  }
  else if (titlePending) {
    drawTitleBlock();     // aufgeschoben, weil der Puffer knapp war
  }
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
    // Codec und Rate in einem Feld. "/s" faellt weg, sonst wird es zu breit:
    // FOOTER_RATE_X bis zum Rand sind 84 px, Font0 ist 6 px je Zeichen, also
    // 14 Zeichen. "VORBIS 320kBit" passt damit gerade noch.
    // Die Bibliothek nennt AAC+ intern "AACP" - hier ausgeschrieben.
    const char* codec = audio.getCodecname();
    if (strcmp(codec, "AACP") == 0) codec = "AAC+";

    char buf[20];
    snprintf(buf, sizeof(buf), "%s %dkBit", codec, kbit);

    M5Cardputer.Display.setTextColor(FOOTER_TEXT_COLOR, TFT_BLACK);
    M5Cardputer.Display.drawRightString(
      buf, M5Cardputer.Display.width() - UI_MARGIN, ty);
  }

  M5Cardputer.Display.setFont(UI_FONT);
}

// Linkes Feld der Fusszeile. Normalerweise die Kennung; laeuft ein
// Sleeptimer, steht dort stattdessen die Restzeit in Gelb.
//
// Font0 ist fest breit, 6 px je Zeichen - "01:27 .zZ" sind also immer 54 px,
// egal welche Ziffern. Nichts springt, solange die fuehrende Null bleibt.
// Bis zum WLAN-Feld sind es 86 px, es passt mit Luft.
static void drawSleepOrName(int ty) {
  M5Cardputer.Display.setFont(FOOTER_FONT);

  // Im Equalizer steht hier die Vordaempfung - der Platz der Kennung ist dort
  // besser gebraucht. Der Wert ist die Daempfung: 6 macht leiser, -6 lauter.
  // Sie geht auch einer laufenden Schlafzeit vor; die kommt zurueck, sobald F
  // den Schirm verlaesst.
  if (visMode == VIS_EQ) {
    char pre[16];
    snprintf(pre, sizeof(pre), "Pre %d dB", (int)audio.eqPreDb());
    M5Cardputer.Display.fillRect(0, M5Cardputer.Display.height() - FOOTER_HEIGHT,
                                 FOOTER_WIFI_X, FOOTER_HEIGHT, TFT_BLACK);
    M5Cardputer.Display.setTextColor(FOOTER_TEXT_COLOR, TFT_BLACK);
    M5Cardputer.Display.drawString(pre, UI_MARGIN, ty);
    lastDrawnSleepMin = -1;
    return;
  }

  if (sleepState == SLEEP_RUNNING) {
    const uint16_t left = sleepMinutesLeft();
    char buf[16];
    snprintf(buf, sizeof(buf), "%02u:%02u .zZ", left / 60, left % 60);
    M5Cardputer.Display.fillRect(0, M5Cardputer.Display.height() - FOOTER_HEIGHT,
                                 FOOTER_WIFI_X, FOOTER_HEIGHT, TFT_BLACK);
    M5Cardputer.Display.setTextColor(TFT_YELLOW, TFT_BLACK);
    M5Cardputer.Display.drawString(buf, UI_MARGIN, ty);
    lastDrawnSleepMin = (int)left;
  } else {
    M5Cardputer.Display.setTextColor(FOOTER_TEXT_COLOR, TFT_BLACK);
    M5Cardputer.Display.drawString(FOOTER_NAME, UI_MARGIN, ty);
    lastDrawnSleepMin = -1;
  }
}

// Nachfuehren aus loop(): nur zeichnen, wenn sich die Minutenzahl wirklich
// geaendert hat - sonst liefe hier jede Runde eine Ausgabe ueber den Bus.
void updateSleepFooter() {
  if (uiOverlayActive()) return;
  if (streamState == STREAM_FAILED) return;   // dort steht die orange Meldung
  if (visMode == VIS_EQ) return;              // dort steht die Vordaempfung
  if (sleepState != SLEEP_RUNNING && lastDrawnSleepMin < 0) return;

  const int now = (sleepState == SLEEP_RUNNING) ? (int)sleepMinutesLeft() : -1;
  if (now == lastDrawnSleepMin) return;

  drawSleepOrName(footerTextY());
  M5Cardputer.Display.setFont(UI_FONT);
  M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
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
    // Fuenf Ursachen, fuenf Meldungen. Die dritte kam am 12.8.2026 dazu: manche
    // Sender liefern statt Ton eine Wiedergabeliste (m3u, m3u8, pls). Frueher
    // ging die an den MP3-Decoder und wurde als Rauschen hoerbar.
    //
    // Seit dem 14.8.2026 loest AudioCompat.h .pls und .m3u selbst auf. Uebrig
    // bleibt der eine Fall, in dem nichts aufzuloesen ist: HLS liefert nur
    // Haeppchen. https-Adressen in einer Liste sind seit dem 19.8.2026
    // spielbar wie alle anderen.
    //
    // "https not work" ist am 19.8.2026 weggefallen: https ist kein
    // Sonderfall mehr, sondern der zweite gewoehnliche Weg zum Sender. Ein
    // gescheiterter https-Sender ist jetzt einfach ein gescheiterter Sender.
    M5Cardputer.Display.setTextColor(TFT_ORANGE, TFT_BLACK);
    if (streamFailedLowMem) {
      M5Cardputer.Display.drawCentreString("Low RAM - codec too big", 120, ty);
    } else if (streamFailedHls) {
      M5Cardputer.Display.drawCentreString("HLS - not supported", 120, ty);
    } else if (streamFailedPlaylist) {
      M5Cardputer.Display.drawCentreString("Playlist - not supported", 120, ty);
    } else if (streamFailedStalled) {
      M5Cardputer.Display.drawCentreString("Stream stopped - no data", 120, ty);
    } else {
      M5Cardputer.Display.drawCentreString("Stream unavailable", 120, ty);
    }
  } else {
    drawSleepOrName(ty);
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

