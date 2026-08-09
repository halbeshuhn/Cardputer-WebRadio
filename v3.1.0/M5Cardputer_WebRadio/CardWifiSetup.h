#include <WiFi.h>
#include <Preferences.h>
#include <esp_wifi.h>
#include <vector>

// ---------------------------------------------------------------------------
// Sprache und Schrift dieser Datei
//
// Die Masken hier sind fest englisch und haengen NICHT an Lang.h. Sie laufen
// beim Start, bevor die Oberflaeche steht; eine Sprachwahl gibt es an dieser
// Stelle noch nicht zu sehen.
//
// Sie benutzen als einzige den Skalenfont (Font0) in 1,5-facher Groesse:
// 9 px je Zeichen, 12 px hoch. Das ist besser lesbar als die Pixelschrift
// und laesst mehr Text auf die Zeile - bei 236 px Platz sind das 26 Zeichen.
//
// setTextSize() gilt global. wifiSetFont() steht deshalb an den beiden
// Einstiegen - connectToWiFi() fuer den Start, wifiActionBegin() fuers
// Systemmenue. Zurueckgestellt wird auf der Sketchseite mit restoreUiFont(),
// in setup() und in wifiActionEnd(). Eine eigene Rueckstellfunktion gibt es
// hier bewusst nicht: sie muesste UI_FONT ein zweites Mal benennen.
// ---------------------------------------------------------------------------
#define WIFI_MASK_FONT (&fonts::Font0)
#define WIFI_MASK_SIZE 1.5f

static void wifiSetFont() {
    M5Cardputer.Display.setFont(WIFI_MASK_FONT);
    M5Cardputer.Display.setTextSize(WIFI_MASK_SIZE);
}

#define NVS_NAMESPACE "M5_settings"
#define MIN_WIFI_RSSI -80
#define MAX_NETWORKS 10
#define MAX_SAVED_WIFI 5
#define WIFI_TIMEOUT 9000
#define WIFI_INFO_MS 5000    // Standzeit der WiFi-Info, mit BtnG0 abkuerzbar

Preferences preferences;

// Ein gespeichertes Netz. slot ist die Platznummer im NVS (0..MAX_SAVED_WIFI-1)
// und NICHT die Position in dieser Liste: die Liste ist verdichtet, im NVS
// koennen Luecken stehen. Wer speichert oder loescht, muss slot benutzen -
// die Verwechslung der beiden war die Ursache der doppelten Eintraege.
struct SavedWiFi {
    String ssid;
    String pass;
    int32_t rssi;
    int slot;
};

SavedWiFi saved[MAX_SAVED_WIFI];
int savedCount = 0;
int lastUsed = -1;

struct WiFiNetwork {
    String ssid;
    int32_t rssi;
    wifi_auth_mode_t encryption;
};
std::vector<WiFiNetwork> networks;

int scrollX = 0;

// ---------------------------------------------------------------------------
// Abbruch per BtnG0.
//
// Die Masken hier drin haben alle ihre eigene blockierende Schleife; die
// Hauptschleife des Radios steht solange still. Damit BtnG0 trotzdem
// herausfuehrt, fragt jede dieser Schleifen wifiCheckAbort() ab und steigt
// mit einem leeren Ergebnis aus (-1 bzw. "" bzw. false).
//
// wifiAbortEnabled schaltet das nur fuer Aufrufe aus dem Systemmenue frei.
// Beim Start ist BtnG0 mit dem WLAN-Reset belegt, dort darf der Abbruch
// nicht dazwischenfunken.
// ---------------------------------------------------------------------------
bool wifiAbortEnabled = false;
bool wifiAbort        = false;

bool wifiCheckAbort() {
    if (!wifiAbortEnabled) return false;

    if (M5Cardputer.BtnA.wasPressed()) wifiAbort = true;

    return wifiAbort;
}

// Warten, ohne BtnG0 zu verschlucken. false = abgebrochen.
bool wifiWait(unsigned long ms) {
    unsigned long start = millis();

    while (millis() - start < ms) {
        M5Cardputer.update();
        if (wifiCheckAbort()) return false;
        delay(10);
    }

    return true;
}

void loadSavedWiFi() {
    preferences.begin(NVS_NAMESPACE, true);
    savedCount = 0;
    lastUsed = preferences.getInt("wifi_last", -1);

    for (int i = 0; i < MAX_SAVED_WIFI; i++) {
        char key[24];

        snprintf(key, sizeof(key), "wifi_%d_ssid", i + 1);
        String ssid = preferences.getString(key, "");

        snprintf(key, sizeof(key), "wifi_%d_pass", i + 1);
        String pass = preferences.getString(key, "");

        if (!ssid.isEmpty()) {
            saved[savedCount++] = { ssid, pass, -100, i };
        }
    }
    preferences.end();
}

// Erster freier Platz im NVS, sonst -1.
int findFreeSlot() {
    preferences.begin(NVS_NAMESPACE, true);

    int freeSlot = -1;

    for (int i = 0; i < MAX_SAVED_WIFI; i++) {
        char key[24];
        snprintf(key, sizeof(key), "wifi_%d_ssid", i + 1);

        if (preferences.getString(key, "").isEmpty()) {
            freeSlot = i;
            break;
        }
    }

    preferences.end();
    return freeSlot;
}

// Einen einzelnen Platz aus dem NVS werfen.
void removeSavedWiFi(int slot) {
    preferences.begin(NVS_NAMESPACE, false);

    char key[24];

    snprintf(key, sizeof(key), "wifi_%d_ssid", slot + 1);
    preferences.remove(key);

    snprintf(key, sizeof(key), "wifi_%d_pass", slot + 1);
    preferences.remove(key);

    // War das der zuletzt benutzte, gibt es keinen zuletzt benutzten mehr.
    if (preferences.getInt("wifi_last", -1) == slot) {
        preferences.remove("wifi_last");
    }

    preferences.end();
}

// Eingabezeile. Passwoerter werden bewusst im Klartext angezeigt - auf einem
// Geraet, das man in der Hand haelt, bringen Sternchen nichts ausser Tippfehlern.
String inputText(const String& prompt, int x, int y) {

    String data;
    data.reserve(64);
    
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.setTextScroll(true);
    M5Cardputer.Display.drawString(prompt, x, y);
    
    while (true) {
        M5Cardputer.update();
        if (wifiCheckAbort()) return "";

        if (M5Cardputer.Keyboard.isChange()) {
            if (M5Cardputer.Keyboard.isPressed()) {

                Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();
                                
                for (auto i : status.word) {
                    if (data.length() < 63) data += i;
                }
                
                if (status.del && data.length() > 0) {
                    data.remove(data.length() - 1);
                }
                
                if (status.enter) {
                    return data;
                }
                
                M5Cardputer.Display.fillRect(0, y - 4, M5Cardputer.Display.width(), 25, BLACK);

                String display;
                display.reserve(66);
                display = "> ";
                display += data;

                M5Cardputer.Display.drawString(display, 4, y);

            }
        }
        delay(10);
    }
}

void displayWiFiInfo() {
    M5Cardputer.Display.fillRect(0, 20, 240, 135, BLACK);
    M5Cardputer.Display.setCursor(1, 1);
    // "Connected to" und der Netzname darunter. Eine Zeile wuerde bei
    // laengeren SSIDs ueberlaufen: 12 Zeichen "Connected to" plus Leerraum
    // liessen bei 236 px nur noch 13 Zeichen fuer den Namen.
    M5Cardputer.Display.drawString("Connected to", 1, 1);
    M5Cardputer.Display.drawString(WiFi.SSID(), 1, 18);
    M5Cardputer.Display.drawString("IP: " + WiFi.localIP().toString(), 1, 33);
    int8_t rssi = WiFi.RSSI();
    M5Cardputer.Display.drawString("RSSI: " + String(rssi) + " dBm", 1, 48);
    wifiWait(WIFI_INFO_MS);
    M5Cardputer.Display.fillRect(0, 0, 240, 135, BLACK);
}

bool fastConnect(const String& ssid, const String& pass) {
    M5Cardputer.Display.clear();
    WiFi.begin(ssid.c_str(), pass.c_str());

    unsigned long start = millis();
    unsigned long lastDot = 0;
    int dots = 0;

    while (millis() - start < WIFI_TIMEOUT) {
        M5Cardputer.update();

        if (WiFi.status() == WL_CONNECTED) {
            return true;
        }

        if (wifiCheckAbort()) {
            WiFi.disconnect(true, true);
            return false;
        }

        if (millis() - lastDot > 350) {
            lastDot = millis();
            dots = (dots + 1) % 4;

            M5Cardputer.Display.fillRect(20, 60, 200, 16, BLACK);

            String line = "Connecting";
            for (int i = 0; i < dots; i++) line += ".";

            M5Cardputer.Display.drawString(line, 20, 60);
        }

        vTaskDelay(1);
    }

    WiFi.disconnect(true, true);
    delay(150);

    return false;
}

int findBestSavedNetwork() {
    WiFi.scanDelete();
    int n = WiFi.scanNetworks(false, false);

    int best = -1;
    int bestRSSI = MIN_WIFI_RSSI;

    for (int i = 0; i < n; i++) {
        for (int s = 0; s < savedCount; s++) {

    if (WiFi.SSID(i) == saved[s].ssid) {
        saved[s].rssi = WiFi.RSSI(i);
        if (saved[s].rssi > bestRSSI) {
            bestRSSI = saved[s].rssi;
            best = s;
                }
            }
        }
    }

    return best;
}

// Liste der gespeicherten Netze mit ">" als Marker. Liefert die Position in
// der Liste (nicht den NVS-Platz!) oder -1 bei Abbruch.
//
// allowRemove schaltet BS als Loeschtaste frei; dann bricht nur noch ESC
// (Zeichen `) oder BtnG0 ab. Ohne allowRemove bricht BS wie bisher ab.
int selectSavedToReplace(const char* title = "Replace WiFi:",
                         const char* hint  = "ENTER = OK",
                         bool allowRemove  = false) {

    String line;
    line.reserve(64);

    int sel = 0;
    int scrollX = 0;
    int lastSel = -1;

    M5Cardputer.update();
    delay(150);

    M5Cardputer.Display.clear();
    M5Cardputer.Display.drawString(title, 2, 2);

    while (true) {
        if (sel != lastSel) {
            scrollX = 0;
            lastSel = sel;
        }

        for (int i = 0; i < savedCount; i++) {
            int y = 20 + i * 18;

            M5Cardputer.Display.fillRect(0, y, 240, 18, BLACK);

            line = (i == sel ? "> " : "  ");
            line += saved[i].ssid;
            int w = M5Cardputer.Display.textWidth(line);

            if (i == sel && w > 230) {
                scrollX++;
                if (scrollX > w) scrollX = 0;
                M5Cardputer.Display.drawString(line, 2 - scrollX, y);
            } else {
                M5Cardputer.Display.drawString(line, 2, y);
            }
        }

        M5Cardputer.Display.fillRect(0, 108, 240, 18, BLACK);
        M5Cardputer.Display.drawString(hint, 2, 110);

        M5Cardputer.update();
        if (wifiCheckAbort()) return -1;

        if (M5Cardputer.Keyboard.isChange()) {
            if (M5Cardputer.Keyboard.isPressed()) {

                if (M5Cardputer.Keyboard.isKeyPressed(';') && sel > 0) {
                    sel--;
                }

                if (M5Cardputer.Keyboard.isKeyPressed('.') && sel < savedCount - 1) {
                    sel++;
                }

                if (M5Cardputer.Keyboard.isKeyPressed(KEY_ENTER)) {
                    return sel;
                }

                if (allowRemove && M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE)) {
                    removeSavedWiFi(saved[sel].slot);
                    loadSavedWiFi();

                    // Die Liste ist kuerzer geworden - Bereich einmal raeumen,
                    // sonst bleibt die unterste Zeile stehen.
                    M5Cardputer.Display.fillRect(0, 20, 240, 88, BLACK);

                    if (savedCount == 0) return -1;
                    if (sel >= savedCount) sel = savedCount - 1;

                    lastSel = -1;
                    delay(150);
                }

                if (M5Cardputer.Keyboard.isKeyPressed('`') ||
                    (!allowRemove &&
                     M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE))) {
                    return -1;
                }
            }
        }

        delay(10);
    }
}

// slot ist die NVS-Platznummer, nicht die Position in der Liste saved[].
void saveWiFiAt(int slot, const String& ssid, const String& pass) {
    preferences.begin(NVS_NAMESPACE, false);

    char key[24];

    snprintf(key, sizeof(key), "wifi_%d_ssid", slot + 1);
    preferences.putString(key, ssid);

    snprintf(key, sizeof(key), "wifi_%d_pass", slot + 1);
    preferences.putString(key, pass);

    preferences.putInt("wifi_last", slot);
    preferences.end();
}

String getSecurityString(wifi_auth_mode_t encType) {
    switch(encType) {
        case WIFI_AUTH_OPEN: return "Open";
        case WIFI_AUTH_WEP: return "WEP";
        case WIFI_AUTH_WPA_PSK: return "WPA";
        case WIFI_AUTH_WPA2_PSK: return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
        default: return "---";
    }
}

String scanAndDisplayNetworks() {
    scrollX = 0;

    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true, true);
    delay(200);

    WiFi.scanDelete();
    WiFi.scanNetworks(true, true);
    
    M5Cardputer.Display.clear();
    M5Cardputer.Display.drawString("WiFi scanning", 1, 1);
    
    int16_t scanResult;
    do {
        scanResult = WiFi.scanComplete();
        M5Cardputer.update();
        if (wifiCheckAbort()) return "";
        delay(100);
    } while(scanResult == WIFI_SCAN_RUNNING);

    if (scanResult == 0) {
        M5Cardputer.Display.drawString("No WiFi found", 1, 15);
        wifiWait(2000);
        return "";
    }
    
    networks.clear();
    networks.reserve(MAX_NETWORKS);
    for (int i = 0; i < scanResult && i < MAX_NETWORKS; i++) {
        String ssid = WiFi.SSID(i);
        if (ssid.length() == 0 || ssid.length() > 32) continue;

        networks.push_back({
            ssid,
            WiFi.RSSI(i),
            WiFi.encryptionType(i)
        });
    }

    if (networks.empty()) {
        M5Cardputer.Display.drawString("No usable networks", 1, 20);
        wifiWait(2000);
        return "";
    }

    std::sort(networks.begin(), networks.end(),
             [](const WiFiNetwork& a, const WiFiNetwork& b) {
                 return a.rssi > b.rssi;
             });
    
    M5Cardputer.Display.clear();
    M5Cardputer.Display.drawString("Available:", 1, 1);
    
    int selectedNetwork = 0;
    int lastSelectedLocal = -1;
    while (true) {
        if (selectedNetwork != lastSelectedLocal) {
            scrollX = 0;
            lastSelectedLocal = selectedNetwork;
        }
        for (size_t i = 0; i < networks.size(); i++) {
            int y = 18 + i * 18;
            bool selected = (i == selectedNetwork);

            M5Cardputer.Display.fillRect(0, y, 240, 18, BLACK);

            String line;
            line.reserve(96);
            line = selected ? "-> " : "   ";
            line += networks[i].ssid;
            line += " (";
            line += networks[i].rssi;
            line += "dBm)";
            if (networks[i].encryption != WIFI_AUTH_OPEN) {
                line += " *";
            }

            int textWidth = M5Cardputer.Display.textWidth(line);

            if (selected && textWidth > 230) {
                scrollX++;
                if (scrollX > textWidth) scrollX = 0;
                M5Cardputer.Display.drawString(line, 1 - scrollX, y);
            } else {
                M5Cardputer.Display.drawString(line, 1, y);
            }
        }
        
        M5Cardputer.Display.drawString("Select ENTER: OK", 1, 108);
        M5Cardputer.update();
        if (wifiCheckAbort()) return "";

        if (M5Cardputer.Keyboard.isChange()) {
            if (M5Cardputer.Keyboard.isPressed()) {
                
                if (M5Cardputer.Keyboard.isKeyPressed(';') && selectedNetwork > 0) {
                    selectedNetwork--;
                }
                if (M5Cardputer.Keyboard.isKeyPressed('.') && 
                    selectedNetwork < networks.size() - 1) {
                    selectedNetwork++;
                }
                if (M5Cardputer.Keyboard.isKeyPressed(KEY_ENTER)) {
                    scrollX = 0;
                    return networks[selectedNetwork].ssid;
                }
            }
        }
        delay(10);
    }
}

// Platz eines bereits gespeicherten Netzes, sonst -1.
int findSavedIndex(const String& ssid) {
    for (int i = 0; i < savedCount; i++) {
        if (saved[i].ssid == ssid) return i;
    }
    return -1;
}

// Passwort abfragen, auf dem NVS-Platz slot speichern und verbinden.
// slot < 0 heisst: nur verbinden, nicht speichern.
static void askPasswordAndConnect(const String& ssid, int slot) {
    M5Cardputer.Display.clear();
    M5Cardputer.Display.drawString("Password:", 2, 40);
    M5Cardputer.Display.drawString(ssid, 2, 60);

    String pass = inputText("> ", 4, 110);
    if (wifiAbort) return;

    M5Cardputer.Display.clear();

    if (slot >= 0) saveWiFiAt(slot, ssid, pass);

    if (fastConnect(ssid, pass)) {
        displayWiFiInfo();
    } else if (!wifiAbort) {
        M5Cardputer.Display.clear();
        M5Cardputer.Display.drawString("Connection", 4, 50);
        M5Cardputer.Display.drawString("failed", 4, 70);
        wifiWait(4000);
    }
}

// Scan, Auswahl, dann verbinden. Frueher stand das am Ende von
// connectToWiFi(); als eigene Funktion ist es auch aus dem Systemmenue
// heraus aufrufbar.
//
// Ist das gewaehlte Netz schon gespeichert, wird zuerst still mit den
// hinterlegten Daten versucht. Erst wenn das scheitert - typisch, weil das
// Passwort geaendert wurde - kommt die Abfrage, und zwar von selbst nach dem
// Ablauf von WIFI_TIMEOUT (9 s). Das neue Passwort ersetzt dann den alten
// Eintrag, es entsteht kein Doppel in der Liste.
void scanAndConnectWiFi() {
    M5Cardputer.Display.clear();

    loadSavedWiFi();   // Liste frisch aus dem NVS, savedCount stimmt danach

    String ssid = scanAndDisplayNetworks();
    if (wifiAbort || ssid.isEmpty()) return;

    int known = findSavedIndex(ssid);

    if (known >= 0) {
        if (fastConnect(saved[known].ssid, saved[known].pass)) {
            saveWiFiAt(saved[known].slot, saved[known].ssid, saved[known].pass);
            displayWiFiInfo();
            return;
        }
        if (wifiAbort) return;

        M5Cardputer.Display.clear();
        M5Cardputer.Display.drawString("Saved password", 4, 40);
        M5Cardputer.Display.drawString("no longer", 4, 60);
        M5Cardputer.Display.drawString("matches", 4, 80);
        if (!wifiWait(1800)) return;

        askPasswordAndConnect(ssid, saved[known].slot);   // derselbe Platz
        return;
    }

    // Unbekanntes Netz: freien Platz nehmen, sonst einen ersetzen lassen.
    int slot = findFreeSlot();

    if (slot < 0) {
        // Alle Plaetze belegt: einer muss weichen. Bricht der Benutzer ab,
        // wird nur verbunden und nichts gespeichert.
        int sel = selectSavedToReplace("Replace WiFi:");
        if (wifiAbort) return;
        if (sel >= 0) slot = saved[sel].slot;
        M5Cardputer.Display.clear();
    }

    askPasswordAndConnect(ssid, slot);
}

// Gespeicherte Netze auflisten und mit dem gewaehlten verbinden.
void connectSavedWiFi() {
    loadSavedWiFi();

    if (savedCount == 0) {
        M5Cardputer.Display.clear();
        M5Cardputer.Display.drawString("No saved", 4, 50);
        M5Cardputer.Display.drawString("networks", 4, 70);
        wifiWait(2000);
        return;
    }

    // ENTER verbindet, BS wirft den markierten Eintrag raus, ESC bricht ab.
    int sel = selectSavedToReplace("Connect to:", "Ok=Cnnct, Back=rmv", true);
    if (sel < 0) return;

    const String ssid = saved[sel].ssid;
    const String pass = saved[sel].pass;
    const int    slot = saved[sel].slot;

    if (fastConnect(ssid, pass)) {
        // Merkt den Platz als zuletzt benutzt, danach startet das Radio
        // beim naechsten Mal gleich hierhin.
        saveWiFiAt(slot, ssid, pass);
        displayWiFiInfo();
    } else if (!wifiAbort) {
        // Passwort passt nicht mehr: gleich neu abfragen, selber Platz.
        M5Cardputer.Display.clear();
        // Dieselben drei Zeilen wie oben. Zweizeilig ginge im Deutschen,
        // im Englischen fehlte ohne die erste Zeile das Subjekt.
        M5Cardputer.Display.drawString("Saved password", 4, 40);
        M5Cardputer.Display.drawString("no longer", 4, 60);
        M5Cardputer.Display.drawString("matches", 4, 80);
        if (!wifiWait(1800)) return;

        askPasswordAndConnect(ssid, slot);
    }
}

// Loescht alle gespeicherten Zugaenge aus dem NVS.
void clearSavedWiFi() {
    preferences.begin(NVS_NAMESPACE, false);

    for (int i = 0; i < MAX_SAVED_WIFI; i++) {

        char key[24];

        snprintf(key, sizeof(key), "wifi_%d_ssid", i + 1);
        preferences.remove(key);

        snprintf(key, sizeof(key), "wifi_%d_pass", i + 1);
        preferences.remove(key);

    }
    preferences.remove("wifi_last");

    preferences.end();

    savedCount = 0;
    lastUsed = -1;
}

// Reset aus dem Systemmenue: Liste loeschen, trennen, neu scannen und
// verbinden. Ohne Neustart - das Radio soll weiterlaufen koennen.
void resetAndReconnectWiFi() {
    M5Cardputer.Display.clear();
    M5Cardputer.Display.drawString("WiFi reset", 4, 40);
    M5Cardputer.Display.drawString("Please wait...", 4, 60);

    clearSavedWiFi();

    WiFi.disconnect(true, true);
    if (!wifiWait(300)) return;

    scanAndConnectWiFi();
}

// Reset beim Start: dasselbe Loeschen, danach aber Neustart des Geraets.
void resetWiFiSettings() {
    M5Cardputer.Display.clear();
    M5Cardputer.Display.drawString("Reset WiFi settings", 4, 50);
    M5Cardputer.Display.drawString("Please wait...", 4, 70);

    clearSavedWiFi();

    WiFi.disconnect(true, true);
    delay(200);
    esp_restart();
}

void connectToWiFi() {

    // Gilt fuer alle Masken, die von hier aus erreicht werden. Zurueck-
    // gestellt wird in setup(), weil diese Funktion mehrere Ausgaenge hat.
    wifiSetFont();

    unsigned long resetStart = millis();
    bool resetShown = false;

    while (millis() - resetStart < 1000) {
        M5Cardputer.update();

        if (!resetShown) {
            M5Cardputer.Display.clear();
            M5Cardputer.Display.drawString("Hold BtnG0 for", 40, 50);
            M5Cardputer.Display.drawString("WiFi reset", 50, 70);
            resetShown = true;
        }

        if (M5Cardputer.BtnA.isPressed()) {
            resetWiFiSettings();
            return;
        }

        delay(100);
    }

    WiFi.mode(WIFI_STA);
    esp_wifi_set_ps(WIFI_PS_NONE);
    
    WiFi.setSleep(false);

    loadSavedWiFi();

    if (savedCount > 0) {

        // wifi_last ist eine NVS-Platznummer; die Liste ist verdichtet und
        // hat womoeglich eine andere Reihenfolge. Also den Eintrag suchen.
        int li = -1;
        for (int i = 0; i < savedCount; i++) {
            if (saved[i].slot == lastUsed) li = i;
        }

        if (li >= 0) {
            if (fastConnect(saved[li].ssid, saved[li].pass)) {
                displayWiFiInfo();
                return;
            }
        }

        int best = findBestSavedNetwork();
        if (best >= 0) {
            if (fastConnect(saved[best].ssid, saved[best].pass)) {
                saveWiFiAt(saved[best].slot, saved[best].ssid, saved[best].pass);
                displayWiFiInfo();
                return;
            }
        }
    }

    scanAndConnectWiFi();
}

