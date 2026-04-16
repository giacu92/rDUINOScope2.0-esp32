// =============================================================================
//  stellarium_goto_esp32.ino  –  rev 3
//  ESP32 – Stellarium Telescope Server  →  Modbus RTU  →  STM32
//
//  Supporta entrambi i client Stellarium sulla stessa porta 10003:
//    • Stellarium Desktop  →  protocollo binario nativo (pacchetti 20/24 byte)
//    • Stellarium Mobile   →  LX200 ASCII over TCP
//  Il protocollo viene rilevato automaticamente dal primo byte ricevuto [14].
//
//  Fix cumulativi rispetto alla versione originale:
//    [1]  lastClockUpdate dichiarata static
//    [2]  Target RA/Dec salvati da :Sr/:Sd, executeGoto() li usa correttamente
//    [3]  Posizione aggiornata solo dopo conferma STM32 (status=done)
//    [4]  Parser TCP robusto: comandi frammentati e multipli nello stesso chunk
//    [5]  Timezone offset separato, non sommato a tm_hour
//    [6]  handleStellariumOld() rimossa
//    [7]  calcLST() usa timezoneOffsetHours, non le costanti NTP hardcoded
//    [8]  stelClient.stop() prima di sovrascrivere il client
//    [9]  Modbus in task FreeRTOS separato (niente delay nel loop principale)
//   [10]  Watchdog sul task Modbus
//   [11]  Buffer TCP resettato ad ogni nuova connessione
//   [12]  Comandi handshake mancanti aggiunti (:GZ#, :GA#, :GT#, :pS#)
//   [13]  Pacchetti binari bloccati durante handshake LX200 (fix "Trying LX200...")
//   [14]  Autodetect protocollo: Desktop (binario) vs Mobile (LX200)
// =============================================================================

#include <WiFi.h>
#include <math.h>
#include <time.h>
#include <esp_task_wdt.h>
#include <ModbusMaster.h>
#include <Adafruit_NeoPixel.h>
#include "telescope.h"
#include "config.h"

// -----------------------------------------------------------------------------
// Variabili globali
// -----------------------------------------------------------------------------

WiFiServer  tcpServer(STEL_PORT);
WiFiClient  stelClient;
ModbusMaster modbus;
Adafruit_NeoPixel statusLed(RGB_LED_COUNT, RGB_LED_PIN, NEO_GRB + NEO_KHZ800);

Telescope telescope(LATITUDE_DEG, LONGITUDE_DEG);

// Posizione CORRENTE del telescopio (letta periodicamente da STM32 via Modbus)
double currentRA_h    = 0.0;
double currentDEC_deg = 0.0;

// FIX [2]: target separato dalla posizione corrente
double targetRA_h     = 0.0;
double targetDEC_deg  = 0.0;
bool   targetRASet    = false;
bool   targetDECSet   = false;
bool   targetSet      = false;   // true dopo aver ricevuto sia :Sr che :Sd
bool   lx200TimeManual = false;  // true dopo :SL o :SC, per non sovrascrivere il tempo LX200 con NTP

// FIX [5]: timezone offset in ore (impostato da :St, default da config)
float  timezoneOffsetHours = (float)(GMT_OFFSET_SEC + DAYLIGHT_OFFSET_SEC) / 3600.0f;

// FIX [13] + [14]: autodetect protocollo sulla stessa porta
//   PROTO_UNKNOWN  → appena connesso, aspettiamo il primo byte
//   PROTO_LX200    → Stellarium Mobile (testo ASCII, LX200)
//   PROTO_BINARY   → Stellarium Desktop (pacchetti binari 20/24 byte)
enum StelProto { PROTO_UNKNOWN, PROTO_LX200, PROTO_BINARY };
StelProto stelProto = PROTO_UNKNOWN;

// lx200Ready: true dopo che LX200 ha superato l'handshake (primo :GR# o :GD#)
// usato solo in modalità PROTO_LX200 per non inviare pacchetti binari durante il "Trying..."
bool lx200Ready = false;

// Buffer accumulo pacchetti binari Desktop
static uint8_t binBuf[64];
static int     binIdx = 0;

// Coda FreeRTOS per i comandi GOTO verso STM32  FIX [9]
struct GotoCmd { long ra_steps; long dec_steps; };
QueueHandle_t gotoQueue;

unsigned long lastStelUpdate = 0;

enum WifiLedState {
    WIFI_LED_DISCONNECTED,
    WIFI_LED_CONNECTED,
    WIFI_LED_STELLARIUM_CONNECTED,
    WIFI_LED_CAPTIVE_PORTAL
};

WifiLedState wifiLedState = WIFI_LED_DISCONNECTED;
bool statusLedHasRendered = false;

// -----------------------------------------------------------------------------
// Prototipi
// -----------------------------------------------------------------------------
void    connectWiFi();
void    initStatusLed();
void    setWifiLedState(WifiLedState state);
void    renderStellariumConnectedLed();
void    updateWifiLedFromStatus();
void    statusLedTask(void* pvParams);
void    syncNTP();
double  calcLST();
void    handleStellarium();
void    processLX200Command(const String &cmd);
void    sendResponse(const String &res);
bool    executeGoto();
void    parseStelBinaryPacket(uint8_t* buf, int len);
void    sendCurrentPosition(WiFiClient& client);
void    preTransmission();
void    postTransmission();
void    modbusTask(void* pvParams);
void    readPositionFromModbus();
double  raDeltaHours(double a, double b);
bool    currentPositionMatchesTarget(long targetRaArcsec100, long targetDecArcsec100);

// =============================================================================
//  SETUP
// =============================================================================
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== Stellarium GOTO Bridge – ESP32 rev2 ===");

    initStatusLed();

    pinMode(RS485_DE_PIN, OUTPUT);
    digitalWrite(RS485_DE_PIN, LOW);

    Serial2.begin(MODBUS_BAUDRATE, SERIAL_8N1, MODBUS_RX_PIN, MODBUS_TX_PIN);
    modbus.begin(MODBUS_SLAVE_ID, Serial2);
    modbus.preTransmission(preTransmission);
    modbus.postTransmission(postTransmission);

    // FIX [9]: crea la coda GOTO e il task Modbus
    gotoQueue = xQueueCreate(4, sizeof(GotoCmd));
    xTaskCreatePinnedToCore(modbusTask, "modbus", 4096, nullptr, 1, nullptr, 1);

    connectWiFi();
    syncNTP();

    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
        telescope.setLX200Date(timeinfo);
    }

    tcpServer.begin();
    tcpServer.setNoDelay(true);
    Serial.printf("Server TCP in ascolto sulla porta %d\n", STEL_PORT);
}

// Differenza minima tra due RA, considerando il wrap 0..24h.
double raDeltaHours(double a, double b) {
    double delta = fabs(a - b);
    if (delta > 12.0) delta = 24.0 - delta;
    return delta;
}

bool currentPositionMatchesTarget(long targetRaArcsec100, long targetDecArcsec100) {
    const double RA_TOL_H    = 0.001111;  // circa 1 arcmin in RA
    const double DEC_TOL_DEG = 0.016667;  // circa 1 arcmin

    double targetRaH  = ((targetRaArcsec100 / 100.0) / 3600.0) / 15.0;
    double targetDecD =  (targetDecArcsec100 / 100.0) / 3600.0;

    return raDeltaHours(currentRA_h, targetRaH) <= RA_TOL_H
        && fabs(currentDEC_deg - targetDecD) <= DEC_TOL_DEG;
}

// =============================================================================
//  LOOP  (core 0 – solo WiFi/TCP, niente delay lunghi)
// =============================================================================
void loop() {
    handleStellarium();

																					
																							 
    if (stelClient && stelClient.connected()) {
        if (millis() - lastStelUpdate >= STEL_UPDATE_MS) {
            lastStelUpdate = millis();
            // Invia posizione solo nel modo giusto:
            //   Desktop (PROTO_BINARY) → sempre, è il protocollo nativo
            if (stelProto == PROTO_BINARY) {
                sendCurrentPosition(stelClient);
            }
        }
    }

    // FIX [1]: static → sopravvive tra le iterazioni del loop
    static unsigned long lastClockUpdate = 0;
    if (!lx200TimeManual && millis() - lastClockUpdate >= 1000) {
        lastClockUpdate = millis();
        struct tm timeinfo;
        if (getLocalTime(&timeinfo)) {
            telescope.setLX200Date(timeinfo);
        }
    }

    delay(10);
}

// =============================================================================
//  WiFi
// =============================================================================
void initStatusLed() {
    statusLed.begin();
    statusLed.setBrightness(RGB_LED_BRIGHTNESS);
    setWifiLedState(WIFI_LED_DISCONNECTED);
    xTaskCreatePinnedToCore(statusLedTask, "status_led", 2048, nullptr, 0, nullptr, 0);
}

void setWifiLedState(WifiLedState state) {
    if (wifiLedState == state && statusLedHasRendered) return;

    wifiLedState = state;

    uint32_t color = 0;
    switch (state) {
        case WIFI_LED_CONNECTED:
            color = statusLed.Color(0, 0, 255);      // blue
            break;
        case WIFI_LED_STELLARIUM_CONNECTED:
            renderStellariumConnectedLed();
            statusLedHasRendered = true;
            return;
        case WIFI_LED_CAPTIVE_PORTAL:
            color = statusLed.Color(255, 180, 0);    // yellow
            break;
        case WIFI_LED_DISCONNECTED:
        default:
            color = statusLed.Color(255, 0, 0);      // red
            break;
    }

    statusLed.setPixelColor(0, color);
    statusLed.show();
    statusLedHasRendered = true;
}

void renderStellariumConnectedLed() {
    constexpr uint32_t BREATH_PERIOD_MS = 4000;  // 0.25 Hz
    constexpr float BREATH_TWO_PI = 6.28318530718f;
    constexpr uint8_t MIN_LEVEL = 32;
    constexpr uint8_t MAX_LEVEL = 255;

    uint32_t phase = millis() % BREATH_PERIOD_MS;
    float breath = (1.0f - cosf((float)phase * BREATH_TWO_PI / BREATH_PERIOD_MS)) * 0.5f;
    uint8_t level = MIN_LEVEL + (uint8_t)((MAX_LEVEL - MIN_LEVEL) * breath);

    statusLed.setPixelColor(0, statusLed.Color(level / 2, 0, level));
    statusLed.show();
}

void updateWifiLedFromStatus() {
    // Keep yellow reserved for the future captive portal implementation.
    if (wifiLedState == WIFI_LED_CAPTIVE_PORTAL) return;

    if (WiFi.status() != WL_CONNECTED) {
        setWifiLedState(WIFI_LED_DISCONNECTED);
    } else if (stelClient && stelClient.connected()) {
        setWifiLedState(WIFI_LED_STELLARIUM_CONNECTED);
    } else {
        setWifiLedState(WIFI_LED_CONNECTED);
    }
}

void statusLedTask(void* pvParams) {
    (void)pvParams;

    for (;;) {
        updateWifiLedFromStatus();
        if (wifiLedState == WIFI_LED_STELLARIUM_CONNECTED) {
            renderStellariumConnectedLed();
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void connectWiFi() {
    setWifiLedState(WIFI_LED_DISCONNECTED);
    Serial.printf("Connessione a: %s", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
    setWifiLedState(WIFI_LED_CONNECTED);
    Serial.printf("\nConnesso! IP: %s\n", WiFi.localIP().toString().c_str());
}

// =============================================================================
//  NTP
// =============================================================================
void syncNTP() {
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
    Serial.print("Sincronizzazione NTP");
    struct tm t;
    int attempts = 0;
    while (!getLocalTime(&t) && attempts < 20) { delay(100); Serial.print("."); attempts++; }
    if (attempts < 20) {
        Serial.printf("\nOra: %02d:%02d:%02d\n", t.tm_hour, t.tm_min, t.tm_sec);
        telescope.setLX200Date(t);
    }
    else
        Serial.println("\nWARNING: NTP fallito!");
}

// =============================================================================
//  LST  –  FIX [7]: usa timezoneOffsetHours invece dei costanti NTP
// =============================================================================
double calcLST() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);

    // Rimuove l'offset di fuso dall'epoch Unix per ottenere UTC puro
    double utcSec = (double)tv.tv_sec - (double)timezoneOffsetHours * 3600.0
                  + tv.tv_usec / 1e6;

    double jd = 2440587.5 + utcSec / 86400.0;

    double T        = (jd - 2451545.0) / 36525.0;
    double gmst_deg = 280.46061837
                    + 360.98564736629 * (jd - 2451545.0)
                    + 0.000387933 * T * T
                    - (T * T * T) / 38710000.0;

    gmst_deg = fmod(gmst_deg, 360.0);
    if (gmst_deg < 0) gmst_deg += 360.0;

    double lst_deg = gmst_deg + LONGITUDE_DEG;
    lst_deg = fmod(lst_deg, 360.0);
    if (lst_deg < 0) lst_deg += 360.0;

    return lst_deg / 15.0;
}

// =============================================================================
//  Gestione connessione/dati Stellarium – autodetect protocollo [14]
//
//  Primo byte ricevuto decide il protocollo per tutta la sessione:
//    0x14 (=20) → Stellarium Desktop, pacchetti binari da 20 byte
//    qualsiasi altro → Stellarium Mobile, LX200 ASCII
// =============================================================================

																		   
static char  tcpBuf[128];
static int   tcpIdx = 0;

void handleStellarium() {

																	  
    if (!stelClient || !stelClient.connected()) {
        if (stelClient) stelClient.stop();
        WiFiClient newClient = tcpServer.accept();
        if (newClient) {
            stelClient    = newClient;
            stelClient.setNoDelay(true);
            stelProto     = PROTO_UNKNOWN;
            lx200Ready    = false;
            targetRASet   = false;
            targetDECSet  = false;
            targetSet     = false;
            tcpIdx        = 0;
            binIdx        = 0;
            memset(tcpBuf, 0, sizeof(tcpBuf));
            memset(binBuf, 0, sizeof(binBuf));
							   
            if (DEBUG_LX200) Serial.println("[TCP] Client connesso, attendo primo byte...");
			 
        }
    }

    if (!stelClient || !stelClient.available()) return;

    // -------------------------------------------------------------------
    // Autodetect: leggi il primo byte e decidi il protocollo
    // -------------------------------------------------------------------
    if (stelProto == PROTO_UNKNOWN) {
        uint8_t first = (uint8_t)stelClient.peek();
        if (first == 20) {                      // 0x14 = lunghezza pacchetto Desktop
            stelProto = PROTO_BINARY;
            Serial.println("[TCP] Protocollo: Stellarium Desktop (binario)");
        } else {
            stelProto = PROTO_LX200;
            Serial.println("[TCP] Protocollo: Stellarium Mobile (LX200)");
        }
    }

    // -------------------------------------------------------------------
    // PROTO_BINARY – Stellarium Desktop
    // Pacchetti in entrata: 20 byte (GOTO)
    // Pacchetti in uscita:  24 byte (posizione) – già gestiti nel loop()
    // -------------------------------------------------------------------
    if (stelProto == PROTO_BINARY) {
        while (stelClient.available()) {
            if (binIdx < (int)sizeof(binBuf)) {
                binBuf[binIdx++] = (uint8_t)stelClient.read();
            } else {
                stelClient.read();  // scarta se overflow
            }

            // Pacchetto GOTO completo (20 byte)
            if (binIdx >= 20) {
                uint16_t pkt_len  = binBuf[0] | (binBuf[1] << 8);
                uint16_t pkt_type = binBuf[2] | (binBuf[3] << 8);

                if (pkt_len == 20 && pkt_type == 0) {
                    parseStelBinaryPacket(binBuf, 20);
                } else {
                    Serial.printf("[BIN] Pacchetto non riconosciuto len=%d type=%d\n",
                                  pkt_len, pkt_type);
                }
                binIdx = 0;
            }
        }
        return;
    }

    // -------------------------------------------------------------------
    // PROTO_LX200 – Stellarium Mobile
    // -------------------------------------------------------------------
    while (stelClient.available()) {
        char c = stelClient.read();

        // ACK immediato (0x06)
        if ((uint8_t)c == 0x06) {
            sendResponse("P");
            continue;
        }

        if (tcpIdx < (int)sizeof(tcpBuf) - 1) {
            tcpBuf[tcpIdx++] = c;
        }

									  
        if (c == '#') {
            tcpBuf[tcpIdx] = '\0';
            if (DEBUG_LX200_FULL) { Serial.print("[RECV] "); Serial.println(tcpBuf); }
            processLX200Command(String(tcpBuf));
            tcpIdx = 0;
        }

																	 
        if (tcpIdx >= (int)sizeof(tcpBuf) - 1) {
            if (DEBUG_LX200) Serial.println("[WARN] Buffer overflow LX200, flush.");
            tcpIdx = 0;
        }
    }
}

// =============================================================================
//  Parser comandi LX200  –  FIX [2] [5] [12]
// =============================================================================
void processLX200Command(const String &cmd) {
    char reply[32];

    // Comando vuoto (solo '#'): Stellarium lo manda come flush/ping, ignora silenziosamente
    if (cmd == "#" || cmd.length() == 0) return;

    if (DEBUG_LX200 && (cmd.startsWith(":S") || cmd.startsWith(":M"))) {
        Serial.print("[LX200 CMD] ");
        Serial.println(cmd);
    }

    // --- SYNC :CM# ---
    if (cmd.startsWith(":CM#")) {
        telescope.getLX200Sync(reply);
        sendResponse(reply);
    }
    // --- GET ALTITUDE :GA# ---
    else if (cmd.startsWith(":GA#")) {
        sendResponse("+00*00#");   // placeholder
    }
    // --- GET DEC ---
    else if (cmd.startsWith(":GD#")) {
        lx200Ready = true;   // FIX [13]
        telescope.normalize();
        telescope.getLX200Dec(reply);
        sendResponse(reply);
    }
    // --- GET LOCAL TIME ---
    else if (cmd.startsWith(":GL#")) {
        telescope.getLX200Localtime(reply);
        if (DEBUG_LX200) Serial.printf("[LX200] :GL# --> Reporting local time: %s\n", reply);
        sendResponse(reply);
    }
    // --- GET LONGITUDE / TRACKING STATUS ---
    // --- GET ALIGNMENT STATUS :GW# già gestito sopra con :Gg# ---
    else if (cmd.startsWith(":Gg#") || cmd.startsWith(":GW#")) {
        telescope.getLX200Longitude(reply);
        sendResponse(reply);
    }
    // --- GET UTC OFFSET TIME :GG# ---  sHH#
    else if (cmd.startsWith(":GG#")) {
        char sign = (timezoneOffsetHours >= 0) ? '+' : '-';
        snprintf(reply, sizeof(reply), "%c%02d#", sign, (int)fabs(timezoneOffsetHours));
        if (DEBUG_LX200) Serial.printf("[LX200] :GG# --> Reporting timezone offset: %s\n", reply);
        sendResponse(reply);
    }
    // --- GET LATITUDE ---
    else if (cmd.startsWith(":Gt#")) {
        telescope.getLX200Latitude(reply);
        sendResponse(reply);
    }
    // --- GET TRACKING RATE :GT# ---
    else if (cmd.startsWith(":GT#")) {
        sendResponse("60.0#");     // siderale
    }
    // --- GET RA ---
    else if (cmd.startsWith(":GR#")) {
        lx200Ready = true;   // FIX [13]: handshake superato, ora possiamo inviare posizione
        telescope.normalize();
        telescope.getLX200RA(reply);
        sendResponse(reply);
    }
    // --- GET AZIMUTH :GZ# (richiesto da alcuni client durante handshake) FIX [12] ---
    else if (cmd.startsWith(":GZ#")) {
        sendResponse("000*00#");   // placeholder
    }
    // --- GET ALIGNMENT MENU :pS# ---
    else if (cmd.startsWith(":pS#")) {
        sendResponse("East#");
    }
    // --- SET DATE : Example: ":SC04/15/26#"
    else if (cmd.startsWith(":SC")) {
        // Parse the date string into struct tm
        String dateStr = cmd.substring(3, cmd.length() - 1); // Remove :SC and #
        int month, day, year;
        if (sscanf(dateStr.c_str(), "%d/%d/%d", &month, &day, &year) == 3) {
            struct tm timeinfo;
            telescope.getLX200DateTime(timeinfo);
            timeinfo.tm_year = (year < 100 ? year + 2000 : year) - 1900;
            timeinfo.tm_mon = month - 1;
            timeinfo.tm_mday = day;
            if (DEBUG_LX200) Serial.printf("[LX200] \":SC\" Date set to: %02d/%02d/%04d\n", month, day, timeinfo.tm_year + 1900);
            if (month >= 1 && month <= 12 && day >= 1 && day <= 31 && telescope.setLX200Date(timeinfo)) {
                lx200TimeManual = true;
                sendResponse("1Updating Planetary Data# #");
            } else {
                sendResponse("0");
            }
        } else {
            sendResponse("0");
        }
    }
    // --- SET TARGET DEC :SdsDD*MM'SS#  FIX [2] ---
    else if (cmd.startsWith(":Sd")) {
        int sign = (cmd.charAt(3) == '-') ? -1 : 1;
        int d = 0, m = 0, s = 0;
        // Accetta sia sDD*MM'SS# che sDD*MM#
        int parsed = sscanf(cmd.c_str() + 4, "%d*%d'%d#", &d, &m, &s);
        if (parsed < 2) parsed = sscanf(cmd.c_str() + 4, "%d*%d:%d#", &d, &m, &s);
        if (parsed < 2) parsed = sscanf(cmd.c_str() + 4, "%d*%d#", &d, &m);
        if (parsed < 2) parsed = sscanf(cmd.c_str() + 4, "%d:%d:%d#", &d, &m, &s);
        if (parsed < 2) parsed = sscanf(cmd.c_str() + 4, "%d:%d#", &d, &m);
        bool decParsed = parsed >= 2;
        if (decParsed) {
            targetDEC_deg = sign * (d + m / 60.0 + s / 3600.0);
            targetDECSet = true;
            targetSet = targetRASet && targetDECSet;
            sendResponse("1");
            if (DEBUG_LX200) Serial.printf("[LX200] Target DEC: %.4f°\n", targetDEC_deg);
        }
        if (!decParsed) {
            if (DEBUG_LX200) {
                Serial.print("[WARN] :Sd non valido: ");
                Serial.println(cmd);
            }
            sendResponse("0");
        }
    }
    // --- SET TARGET RA :SrHH:MM:SS#  FIX [2] ---
    else if (cmd.startsWith(":Sr")) {
        int h = 0, m = 0, s = 0;
        float mf = 0.0f;
        int parsed = sscanf(cmd.c_str() + 3, "%d:%d:%d#", &h, &m, &s);
        if (parsed == 3) {
            targetRA_h = h + m / 60.0 + s / 3600.0;
        } else if (sscanf(cmd.c_str() + 3, "%d:%f#", &h, &mf) == 2) {
            targetRA_h = h + mf / 60.0;
            parsed = 2;
        }

        if (parsed >= 2) {
            targetRASet = true;
            targetSet = targetRASet && targetDECSet;
            if (DEBUG_LX200) Serial.printf("[LX200] Target RA: %.4f h\n", targetRA_h);
            sendResponse("1");
        } else {
            if (DEBUG_LX200) {
                Serial.print("[WARN] :Sr non valido: ");
                Serial.println(cmd);
            }
            sendResponse("0");
        }
    }
    // --- GOTO :MS#  FIX [2] ---
    else if (cmd.startsWith(":MS#")) {
        if (targetSet) {
            if (DEBUG_LX200) {
                Serial.printf("[LX200] :MS# start GOTO RA=%.4fh DEC=%.4f deg\n",
                              targetRA_h,
                              targetDEC_deg);
            }
            sendResponse(executeGoto() ? "0" : "1Goto queue full#");
        } else {
            // LX200: "1" = oggetto sotto orizzonte / non pronto
            Serial.printf("[WARN] :MS# ricevuto senza target valido (RA=%d DEC=%d)\n",
                          targetRASet ? 1 : 0,
                          targetDECSet ? 1 : 0);
            sendResponse("1No target set#");
        }
    }

    // --- MANUAL SLEW (:Mn#/:Ms#/:Me#/:Mw#) ---
    // Stellarium/SkySafari possono inviare questi durante alcuni controlli manuali.
    else if (cmd.startsWith(":Mn#") || cmd.startsWith(":Ms#") ||
             cmd.startsWith(":Me#") || cmd.startsWith(":Mw#")) {
        sendResponse("");
    }

    // --- PRODUCT NAME ---
    else if (cmd.startsWith(":GVP#")) { sendResponse("rDuinoScope2.0#"); }

    // --- VERSION ---
    else if (cmd.startsWith(":GVN#")) { sendResponse("02.0#"); }

    // --- FIRMWARE DATE  (formato LX200: "MMM DD YYYY#") ---
    else if (cmd.startsWith(":GVD#")) {
        telescope.getLX200FwDate(reply);
        sendResponse(reply);
    }

    // --- FIRMWARE TIME ---
    else if (cmd.startsWith(":GVT#")) {
        telescope.getLX200FwTime(reply);
        sendResponse(reply);
    }

    // --- GET DATE ---
    else if (cmd.startsWith(":GC#")) {
        telescope.getLX200Date(reply);
        sendResponse(reply);
    }

    // --- SLEW STATUS :D# ---
    else if (cmd.startsWith(":D#")) {
        sendResponse(telescope.isSlewing ? "|#" : "#");
    }

    // --- ABORT :Q# ---
    else if (cmd.startsWith(":Q#") || cmd.startsWith("Q#")) {
        telescope.setSlewing(false);
        // Invia comando STOP a STM32 tramite coda (non bloccante)
        GotoCmd stopCmd = {0, 0};
        // Usiamo ra_steps == LONG_MIN come segnale "STOP" nel task Modbus
        stopCmd.ra_steps = LONG_MIN;
        xQueueSend(gotoQueue, &stopCmd, 0);
    }

    // --- SET LONGITUDE :SgsDDD*MM#  FIX [5]: parse separato ---
    else if (cmd.startsWith(":Sg")) {
        const char *lonText = cmd.c_str() + 3;
        int sign = 1;
        if (*lonText == '+' || *lonText == '-') {
            sign = (*lonText == '-') ? -1 : 1;
            lonText++;
        }

        int d = 0, m = 0;
        if (sscanf(lonText, "%d*%d#", &d, &m) == 2) {
            double meadeWestLon = sign * (d + m / 60.0);
            double eastLon = -meadeWestLon; // LX200/Meade usa Ovest positivo; interno usa Est positivo.
            while (eastLon < -180.0) eastLon += 360.0;
            while (eastLon > 180.0) eastLon -= 360.0;
            telescope.setLX200Longitude(eastLon);
            sendResponse("1");
        } else {
            sendResponse("0");
        }
    }

    // --- SET LATITUDE :StsDDD*MM#  (riusa lo stesso parser) ---
    else if (cmd.startsWith(":St")) {
        int sign = (cmd.charAt(3) == '-') ? -1 : 1;
        int d = 0, m = 0;
        sscanf(cmd.c_str() + 4, "%d*%d#", &d, &m);
        double val = sign * (d + m / 60.0);
        telescope.setLX200Latitude(val);
        sendResponse("1");
    }

    // --- SET UTC OFFSET :SG sHH.H#  FIX [5]: salva offset, non modifica tm_hour ---
    else if (cmd.startsWith(":SG")) {
        float offset = 0.0f;
        int sign = (cmd.charAt(3) == '-') ? -1 : 1;
        int h = 0, h2 = 0;
        sscanf(cmd.c_str() + 4, "%02d.%d#", &h, &h2);
        offset = sign * (h + h2 / 10.0);
        timezoneOffsetHours = offset;
        if (DEBUG_LX200)    Serial.printf("[LX200] Timezone offset: %.1f h\n", timezoneOffsetHours);
        sendResponse("1");
    }

    // --- SET LOCAL TIME :SL HH:MM:SS# (aggiorna l'ora nell'oggetto telescope) ---
    else if (cmd.startsWith(":SL")) {
        int h = 0, m = 0, s = 0;
        if (sscanf(cmd.c_str() + 3, "%d:%d:%d#", &h, &m, &s) == 3) {
            // Prendi la data attuale dal telescope, modifica solo l'ora
            struct tm timeinfo;
            telescope.getLX200DateTime(timeinfo);
            timeinfo.tm_hour = h;
            timeinfo.tm_min = m;
            timeinfo.tm_sec = s;
            
            // Aggiorna il telescope
            telescope.setLX200Date(timeinfo);
            lx200TimeManual = true;
            
            if (DEBUG_LX200) Serial.printf("[LX200] Ora impostata a: %02d:%02d:%02d\n", h, m, s);
            sendResponse("1");
        } else {
            sendResponse("0");
        }
    }

    // --- UNKNOWN ---
    else {
        if (DEBUG_LX200) {
            Serial.print("[WARN] Comando non gestito: ");
            Serial.println(cmd);
        }
    }
}

// Helper risposta
void sendResponse(const String &res) {
    if (stelClient && stelClient.connected()) {
        stelClient.print(res);
        stelClient.flush();
        if (DEBUG_LX200_FULL) { Serial.print("[SEND] "); Serial.println(res); }
    }
}

// =============================================================================
//  executeGoto  –  FIX [2]: usa targetRA_h / targetDEC_deg
//  Condivisa da entrambi i protocolli
// =============================================================================
bool executeGoto() {
    // Converte RA (ore) → arcsec*100  e  DEC (gradi) → arcsec*100
    // Lo STM32 riceve coordinate assolute in arcsec*100 e le converte in
    // passi motore internamente (inclusa la conversione RA→HA con il suo clock)
    long ra_arcsec100  = (long)(targetRA_h    * 15.0 * 3600.0 * 100.0);
    long dec_arcsec100 = (long)(targetDEC_deg *        3600.0 * 100.0);

    if (DEBUG_LX200_MODBUS) {
        Serial.printf("[GOTO] RA=%.4fh DEC=%.4f deg -> RA_arcsec100=%ld DEC_arcsec100=%ld\n",
                      targetRA_h, targetDEC_deg, ra_arcsec100, dec_arcsec100);
    }

    telescope.setSlewing(true);

    GotoCmd cmd = { ra_arcsec100, dec_arcsec100 };
    if (xQueueSend(gotoQueue, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
        if (DEBUG_LX200) { Serial.println("[WARN] Coda GOTO piena, comando scartato."); }
        telescope.setSlewing(false);
        return false;
    }

    return true;
}

// =============================================================================
//  Parsing pacchetto GOTO Stellarium Desktop (20 byte, little-endian)
//  uint16 len | uint16 type | uint64 time_us | uint32 ra | int32 dec
// =============================================================================
void parseStelBinaryPacket(uint8_t* buf, int len) {
    if (len < 20) return;

    uint32_t ra_raw  = (uint32_t)buf[12]
                     | ((uint32_t)buf[13] << 8)
                     | ((uint32_t)buf[14] << 16)
                     | ((uint32_t)buf[15] << 24);

    int32_t dec_raw  = (int32_t)buf[16]
                     | ((int32_t)buf[17] << 8)
                     | ((int32_t)buf[18] << 16)
                     | ((int32_t)buf[19] << 24);

    targetRA_h    = ((double)ra_raw  / 0xFFFFFFFFUL) * 24.0;
    targetDEC_deg = ((double)dec_raw / 0x3FFFFFFFL)  * 90.0;
    targetRASet   = true;
    targetDECSet  = true;
    targetSet     = true;

    if (DEBUG_LX200) { Serial.printf("[BIN] GOTO → RA=%.4f h  DEC=%.4f°\n", targetRA_h, targetDEC_deg); }
    executeGoto();
}

// =============================================================================
//  Task FreeRTOS Modbus  –  FIX [9] [10]
//  Gira su core 1, legge dalla coda e gestisce tutta la comunicazione RS485
// =============================================================================
void modbusTask(void* pvParams) {
    // FIX [10]: registra questo task nel watchdog
    esp_task_wdt_add(nullptr);

    static unsigned long lastPosRead = 0;
    for (;;) {
        esp_task_wdt_reset();   // keep-alive watchdog

        GotoCmd cmd;
        if (xQueueReceive(gotoQueue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE) {

            // Segnale STOP (RA == LONG_MIN)
            if (cmd.ra_steps == LONG_MIN) {
                modbus.setTransmitBuffer(0, 0);
                modbus.setTransmitBuffer(1, 0);
                modbus.setTransmitBuffer(2, 0);
                modbus.setTransmitBuffer(3, 0);
                modbus.setTransmitBuffer(4, 0x0002);  // CMD_STOP
                modbus.writeMultipleRegisters(REG_RA_HIGH, 5);
                continue;
            }

            uint16_t ra_high  = (uint16_t)((cmd.ra_steps  >> 16) & 0xFFFF);
            uint16_t ra_low   = (uint16_t)( cmd.ra_steps         & 0xFFFF);
            uint16_t dec_high = (uint16_t)((cmd.dec_steps >> 16) & 0xFFFF);
            uint16_t dec_low  = (uint16_t)( cmd.dec_steps        & 0xFFFF);

            uint8_t resetResult = modbus.writeSingleRegister(REG_COMMAND, 0x0000);
            if (resetResult != modbus.ku8MBSuccess && DEBUG_LX200_MODBUS) {
                if (DEBUG_LX200_MODBUS) { Serial.printf("[Modbus] Errore reset COMMAND prima del GOTO: 0x%02X\n", resetResult); }
            }
            vTaskDelay(pdMS_TO_TICKS(20));

            modbus.setTransmitBuffer(0, ra_high);
            modbus.setTransmitBuffer(1, ra_low);
            modbus.setTransmitBuffer(2, dec_high);
            modbus.setTransmitBuffer(3, dec_low);
            modbus.setTransmitBuffer(4, 0x0001);  // CMD_GOTO
            
            if (DEBUG_LX200_MODBUS) { Serial.printf("[Modbus] sending: RA_high=0x%04X RA_low=0x%04X DEC_high=0x%04X DEC_low=0x%04X COMMAND=%d\n",
                          ra_high, ra_low, dec_high, dec_low, 1); }

            uint8_t result = modbus.writeMultipleRegisters(REG_RA_HIGH, 5);
            if (result != modbus.ku8MBSuccess) {
                if (DEBUG_LX200_MODBUS) { Serial.printf("[Modbus] Errore scrittura: 0x%02X\n", result); }
                telescope.setSlewing(false);
                continue;
            }

            // Polling status STM32 fino a TRACKING (done) o ERROR o timeout
            // Ogni iterazione legge davvero i registri Modbus e aggiorna telescope
            const int MAX_POLL = 3000;   // max ~30 secondi (100 ms * 300)
            bool gotoFinished = false;
            bool sawSlewing = false;
            bool commandCleared = false;
            bool warnedTrackingWithoutSlewing = false;
            for (int i = 0; i < MAX_POLL; i++) {
                esp_task_wdt_reset();
                vTaskDelay(pdMS_TO_TICKS(100));

                readPositionFromModbus();   // aggiorna telescope.status, ra, dec

                if (telescope.status == State::SLEWING) {
                    sawSlewing = true;

                    // Dopo che STM32 ha preso in carico il comando, rimetti REG_COMMAND
                    // a zero: il prossimo GOTO sara' una nuova transizione 0 -> 1.
                    if (!commandCleared) {
                        uint8_t clearResult = modbus.writeSingleRegister(REG_COMMAND, 0x0000);
                        if (clearResult == modbus.ku8MBSuccess) {
                            commandCleared = true;
                            if (DEBUG_LX200_MODBUS) { Serial.println("[Modbus] Setting COMMAND = 0."); }
                        } else if (DEBUG_LX200_MODBUS) {
                            Serial.printf("[Modbus] Error: unable to clear COMMAND. COMMAND = 0x%02X\n", clearResult);
                        }
                    }
                } else if (telescope.status == State::TRACKING) {
                    if (sawSlewing || currentPositionMatchesTarget(cmd.ra_steps, cmd.dec_steps)) {
                        if (DEBUG_LX200_MODBUS) { Serial.println("[Modbus] GOTO completato, tracking attivo."); }
                        telescope.setSlewing(false);
                        gotoFinished = true;
                        break;
                    }

                    if (i > 5 && !warnedTrackingWithoutSlewing && DEBUG_LX200_MODBUS) {
                        Serial.println("[Modbus] TRACKING letto senza SLEWING: attendo avvio reale GOTO.");
                        warnedTrackingWithoutSlewing = true;
                    }
                } else if (telescope.status == State::ERROR) {
                    if (DEBUG_LX200_MODBUS) { Serial.println("[Modbus] STM32 ha riportato errore GOTO."); }
                    telescope.setSlewing(false);
                    gotoFinished = true;
                    break;
                } else if (telescope.status == State::IDLE) {
                    // STM32 tornato IDLE senza passare per TRACKING: GOTO non partito?
                    if (i > 2) {   // ignora i primi cicli (latenza comando)
                        if (DEBUG_LX200_MODBUS) { Serial.println("[Modbus] STM32 IDLE durante GOTO, uscita."); }
                        telescope.setSlewing(false);
                        gotoFinished = true;
                        break;
                    }
                }
            }

            if (!gotoFinished) {
                if (DEBUG_LX200_MODBUS) { Serial.println("[Modbus] Timeout polling STM32."); }
                modbus.writeSingleRegister(REG_COMMAND, 0x0000);
                telescope.setSlewing(false);
            }
            lastPosRead = millis();   // evita doppia lettura immediata a fine GOTO
        }

        // Lettura periodica della posizione corrente (~4 volte/secondo)
        if (millis() - lastPosRead >= 250) {
            lastPosRead = millis();
            readPositionFromModbus();
        }
    }
}

// =============================================================================
//  Legge STATUS + posizione corrente da STM32 (REG 5..9, 5 registri)
//  Aggiorna telescope.status, telescope.ra, telescope.dec, currentRA_h, currentDEC_deg
// =============================================================================
void readPositionFromModbus() {
    uint8_t result = modbus.readHoldingRegisters(REG_STATUS, 5);
    if (result == modbus.ku8MBSuccess) {
        telescope.status = static_cast<State>(modbus.getResponseBuffer(0) & 0xF);

        // RA e DEC sono in arcsec*100, signed 32-bit, split su due 16-bit registers
        int32_t ra_arcsec100  = (int32_t)((uint32_t)modbus.getResponseBuffer(1) << 16
                                         | (uint32_t)modbus.getResponseBuffer(2));
        int32_t dec_arcsec100 = (int32_t)((uint32_t)modbus.getResponseBuffer(3) << 16
                                         | (uint32_t)modbus.getResponseBuffer(4));

        // arcsec*100 → gradi → ore (solo RA)
        currentRA_h    = (ra_arcsec100  / 100.0) / 3600.0 / 15.0;
        currentDEC_deg = (dec_arcsec100 / 100.0) / 3600.0;
        telescope.ra   = currentRA_h;
        telescope.dec  = currentDEC_deg;
        telescope.isSlewing = (telescope.status == State::SLEWING);

        if (DEBUG_LX200_FULL) {
            Serial.printf("[Modbus] Status=%d RA=%.4fh DEC=%.4f°\n",
                          (int)telescope.status, currentRA_h, currentDEC_deg);
        }
    } else {
        if (DEBUG_LX200_MODBUS) { Serial.printf("[Modbus] Errore lettura posizione: 0x%02X\n", result); }
    }
}

// =============================================================================
//  Invio posizione corrente a Stellarium (pacchetto 24 byte)
// =============================================================================
void sendCurrentPosition(WiFiClient& client) {
    if (!client || !client.connected()) return;

    uint32_t ra_raw  = (uint32_t)((currentRA_h    / 24.0) * 0xFFFFFFFFUL);
    int32_t  dec_raw = (int32_t) ((currentDEC_deg / 90.0) * 0x3FFFFFFFL);

    struct timeval tv;
    gettimeofday(&tv, nullptr);
    uint64_t time_us = (uint64_t)tv.tv_sec * 1000000ULL + tv.tv_usec;

    uint8_t pkt[24];
    pkt[0] = 24; pkt[1] = 0;
    pkt[2] = 0;  pkt[3] = 0;
    for (int i = 0; i < 8; i++) pkt[4+i] = (time_us >> (8*i)) & 0xFF;
    pkt[12] = (ra_raw      ) & 0xFF;
    pkt[13] = (ra_raw >>  8) & 0xFF;
    pkt[14] = (ra_raw >> 16) & 0xFF;
    pkt[15] = (ra_raw >> 24) & 0xFF;
    pkt[16] = (dec_raw      ) & 0xFF;
    pkt[17] = (dec_raw >>  8) & 0xFF;
    pkt[18] = (dec_raw >> 16) & 0xFF;
    pkt[19] = (dec_raw >> 24) & 0xFF;
    pkt[20] = 0; pkt[21] = 0; pkt[22] = 0; pkt[23] = 0;

    client.write(pkt, 24);
}

// =============================================================================
//  RS485 direction callbacks
// =============================================================================
void preTransmission()  { digitalWrite(RS485_DE_PIN, HIGH); }
void postTransmission() { digitalWrite(RS485_DE_PIN, LOW);  }
