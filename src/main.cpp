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
//    [7]  LST calcolato dal controller clock in Telescope
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
#include <string.h>
#include <time.h>
#include <Adafruit_NeoPixel.h>
#include "telescope.h"
#include "config.h"
#include "display.h"
#include "graphic_screens.h"
#include "mount_link.h"
#include "system_stats.h"
#include "touch_input.h"

// -----------------------------------------------------------------------------
// Variabili globali
// -----------------------------------------------------------------------------

WiFiServer  tcpServer(STEL_PORT);
WiFiClient  stelClient;
Adafruit_NeoPixel statusLed(RGB_LED_COUNT, RGB_LED_PIN, NEO_GRB + NEO_KHZ800);

Telescope telescope(LATITUDE_DEG, LONGITUDE_DEG);

constexpr BaseType_t MODBUS_TASK_CORE = 1;
constexpr BaseType_t STATUS_LED_TASK_CORE = 0;
constexpr BaseType_t DISPLAY_TASK_CORE = 1;
constexpr BaseType_t TOUCH_TASK_CORE = 1;
constexpr BaseType_t CPU_LOAD_TASK_CORE = 0;
constexpr BaseType_t TASK_STATS_TASK_CORE = 0;
constexpr BaseType_t SYSTEM_INPUTS_TASK_CORE = 0;

// FIX [2]: target separato dalla posizione corrente
double targetRA_h     = 0.0;
double targetDEC_deg  = 0.0;
bool   targetRASet    = false;
bool   targetDECSet   = false;
bool   targetSet      = false;   // true dopo aver ricevuto sia :Sr che :Sd
bool   controllerTimeManual = false;  // true dopo :SL o :SC, per non sovrascrivere il tempo centralina con NTP

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

unsigned long lastStelUpdate = 0;

enum WifiLedState {
    WIFI_LED_DISCONNECTED,
    WIFI_LED_CONNECTED,
    WIFI_LED_STELLARIUM_CONNECTED,
    WIFI_LED_CAPTIVE_PORTAL
};

WifiLedState wifiLedState = WIFI_LED_DISCONNECTED;
bool statusLedHasRendered = false;
TaskHandle_t displayTaskHandle = nullptr;

// -----------------------------------------------------------------------------
// Prototipi
// -----------------------------------------------------------------------------
bool    connectWiFi();
void    initStatusLed();
void    setWifiLedState(WifiLedState state);
void    renderStellariumConnectedLed();
void    updateWifiLedFromStatus();
void    statusLedTask(void* pvParams);
void    initSystemInputs();
bool    readNightModeInput();
void    applyNightModeInput(bool nightMode);
void    systemInputsTask(void* pvParams);
void    displayTask(void* pvParams);
void    notifyDisplayTask();
void    syncNTP();
void    handleStellarium();
void    processLX200Command(const String &cmd);
void    sendResponse(const String &res);
bool    executeGoto();
void    parseStelBinaryPacket(uint8_t* buf, int len);
void    sendCurrentPosition(WiFiClient& client);

// =============================================================================
//  SETUP
// =============================================================================
void setup() {
    Serial.begin(115200);
    delay(500);
    initStatusLed();
    initSystemInputs();
    Serial.printf("\n=== %s ESP32 FW %02X.%02X ===\n",
                  ESP32_FIRMWARE_NAME,
                  (ESP32_FIRMWARE_VERSION >> 8) & 0xFF,
                  ESP32_FIRMWARE_VERSION & 0xFF);

    /*
     * L'inizializzazione completa viene fatta in displayShowInitScreen() per mostrare lo stato di ogni step sul display.
     * I passi principali sono:
     *   1. Connessione WiFi
     *   2. Sincronizzazione NTP e impostazione orologio
     *   3. GPS, Sensori, RTC, SD Card (non implementati, ma riservati nello splash screen)
     *   3. Avvio comunicazione con STM32 (MountLink)
     *   4. Avvio server TCP Stellarium
     *   5. Avvio task display e diagnostica
     */
    //displayBootSetStatus(0, "Touchscreen XPT2046", BootStatus::Pending);
    //displayBootSetStatus(1, "WiFi", BootStatus::Pending);
    //displayBootSetStatus(2, "Clock / NTP", BootStatus::Pending);
    //displayBootSetStatus(3, "GPS", BootStatus::Pending);
    //displayBootSetStatus(4, "BME280", BootStatus::Pending);
    //displayBootSetStatus(5, "RTC", BootStatus::Pending);
    //displayBootSetStatus(6, "SD Card", BootStatus::Pending);
    //displayBootSetStatus(7, "Modbus (STM32F)", BootStatus::Pending);
    //displayBootSetStatus(8, "TCP server", BootStatus::Pending);
    //displayBootSetStatus(9, "Custom configs", BootStatus::Pending);

    bool isFatalError = false;
    displayBootSetStatus(-1, "System booting", BootStatus::None);

    if (!displayBegin()) {
        Serial.println("[DISPLAY] LovyanGFX init failed; continuing headless.");
        isFatalError = true;
    } else {
        displayShowBootScreen();
        displayBootSetStatus(0, "Touchscreen XPT2046", BootStatus::Running);
        displayBootSetStatus(0, "Touchscreen XPT2046", displayProbeTouch() ? BootStatus::Ok : BootStatus::Fail);
    }

    displayBootSetStatus(1, "WiFi", BootStatus::Running);
    displayBootSetStatus(1, "WiFi", connectWiFi() ? BootStatus::Ok : BootStatus::Fail);

    displayBootSetStatus(2, "Clock / NTP", BootStatus::Running);
    syncNTP();

    // GPS non implementato, ma riservato nello splash screen
    displayBootSetStatus(3, "GPS", BootStatus::Skip);
    bool gpsReady = true; 
    //gpsReady = gpsInit();
    isFatalError |= !gpsReady;

    // BME280 non implementato, ma riservato nello splash screen
    displayBootSetStatus(4, "BME280", BootStatus::Skip);
    bool bmeReady = true;
    //bmeReady = bmeInit();
    if (!bmeReady) {
        Serial.println("[WARN] BME280 init failed, continuing without environmental data.");
        displayBootSetStatus(4, "BME280", BootStatus::Fail);
    }
    
    // RTC non implementato, ma riservato nello splash screen
    displayBootSetStatus(5, "RTC", BootStatus::Skip);
    bool rtcReady = true;
    //rtcReady = rtcInit();
    if (!rtcReady) {
        Serial.println("[WARN] RTC init failed, continuing without real-time clock.");
        displayBootSetStatus(5, "RTC", BootStatus::Fail);
    }

    // SD Card non implementato, ma riservato nello splash screen
    displayBootSetStatus(6, "SD Card", BootStatus::Skip);
    
    // Init Mount Link (Modbus RTU verso STM32)
    displayBootSetStatus(7, "Modbus (STM32F)", BootStatus::Running);
    bool mountLinkReady = mountLinkBegin(telescope, notifyDisplayTask);
    displayBootSetStatus(7, "Modbus (STM32F)", mountLinkReady ? BootStatus::Ok : BootStatus::Fail);
    isFatalError |= !mountLinkReady;
    if (mountLinkReady) {
        mountLinkStartTask(MODBUS_TASK_CORE);
    }
    else {
        Serial.println("[ERROR] STM32 MountLink init failed. Cannot continue.");
        return;
    }

    displayBootSetStatus(8, "TCP server", BootStatus::Running);
    tcpServer.begin();
    tcpServer.setNoDelay(true);
    displayBootSetStatus(8, "TCP server", BootStatus::Ok);
    Serial.printf("[INFO] Server TCP in ascolto sulla porta %d\n", STEL_PORT);

    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
        telescope.setControllerDateTime(timeinfo);
        displayBootSetStatus(2, "Clock / NTP", BootStatus::Ok);
    } else {
        displayBootSetStatus(2, "Clock / NTP", BootStatus::Fail);
    }

    // Custom configs non implementati, ma riservati nello splash screen
    // Verranno salvate nella EEPROM e applicati da displayTask() dopo lo splash screen
    /* Implementeremo la suddetta cosa usando Preferences.h: 
        #include <Preferences.h>
        Preferences prefs;
        // Per esempio, per salvare la posizione:
        prefs.begin("config", false);   // namespace "config", modalità lettura/scrittura
        prefs.putDouble("latitude", 42.350);

        // Per leggere la posizione all'avvio:
        prefs.begin("config", true);    // namespace "config", modalità sola lettura
        double latitude = prefs.getDouble("latitude", 42.350); // default 42.350 se non esiste
    */
    displayBootSetStatus(9, "Custom configs", BootStatus::Skip);

#if ENABLE_CPU_LOAD_MONITOR
    cpuLoadBegin(CPU_LOAD_TASK_CORE, notifyDisplayTask);
    taskStatsBegin(TASK_STATS_TASK_CORE);
#endif
    xTaskCreatePinnedToCore(systemInputsTask, "system_inputs", 2048, nullptr, 0, nullptr, SYSTEM_INPUTS_TASK_CORE);

    delay(700);

    xTaskCreatePinnedToCore(displayTask, "display", 6144, nullptr, 1, &displayTaskHandle, DISPLAY_TASK_CORE);
    touchInputBegin(TOUCH_TASK_CORE);
}

// =============================================================================
//  LOOP  (Arduino core 1) - WiFi/TCP, niente delay lunghi
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
    if (!controllerTimeManual && millis() - lastClockUpdate >= 1000) {
        lastClockUpdate = millis();
        struct tm timeinfo;
        if (getLocalTime(&timeinfo)) {
            telescope.setControllerDateTime(timeinfo);
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
    xTaskCreatePinnedToCore(statusLedTask, "status_led", 2048, nullptr, 0, nullptr, STATUS_LED_TASK_CORE);
}

void setWifiLedState(WifiLedState state) {
    if (wifiLedState == state && statusLedHasRendered) return;

    wifiLedState = state;
    notifyDisplayTask();

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

void initSystemInputs() {
    pinMode(IS_NIGHT_MODE_PIN, INPUT_PULLUP);
    applyNightModeInput(readNightModeInput());
}

bool readNightModeInput() {
    return digitalRead(IS_NIGHT_MODE_PIN) == LOW;
}

void applyNightModeInput(bool nightMode) {
    if (displayIsNightMode() == nightMode) return;

    displaySetNightMode(nightMode);
    Serial.printf("[SYSTEM] Night mode %s (GPIO %u)\n",
                  nightMode ? "enabled" : "disabled",
                  IS_NIGHT_MODE_PIN);

    if (displayTaskHandle) {
        notifyDisplayTask();
    } else if (display().isReady()) {
        displayShowBootScreen();
    }
}

void systemInputsTask(void* pvParams) {
    (void)pvParams;

    for (;;) {
        applyNightModeInput(readNightModeInput());

        // Future slow sensors and board-level inputs can be sampled here.
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void displayTask(void* pvParams) {
    (void)pvParams;

    struct DisplayViewState {
        bool wifiConnected;
        bool stellariumConnected;
        char ipAddress[16];
        uint16_t stm32FirmwareVersion;
        bool soundEnabled;
        bool motorsEnabled;
        State mountStatus;
        uint8_t cpu0Load;
        uint8_t cpu1Load;
    };

    DisplayViewState lastRendered = {};
    bool hasRendered = false;
    bool lastNightMode = displayIsNightMode();

    for (;;) {
        MountLinkSnapshot state = mountLinkGetSnapshot();
#if ENABLE_CPU_LOAD_MONITOR
        CpuLoadSnapshot cpuLoad = cpuLoadGetSnapshot();
#endif
        DisplayViewState view = {};
        view.wifiConnected = WiFi.status() == WL_CONNECTED;
        view.stellariumConnected = stelClient && stelClient.connected();
        if (view.wifiConnected) {
            IPAddress ip = WiFi.localIP();
            snprintf(view.ipAddress, sizeof(view.ipAddress), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
        } else {
            snprintf(view.ipAddress, sizeof(view.ipAddress), "-");
        }
        view.stm32FirmwareVersion = state.stm32FirmwareVersion;
        view.soundEnabled = false;
        view.motorsEnabled = state.motorsEnabled;
        view.mountStatus = state.status;
#if ENABLE_CPU_LOAD_MONITOR
        view.cpu0Load = cpuLoad.core0;
        view.cpu1Load = cpuLoad.core1;
#else
        view.cpu0Load = 0;
        view.cpu1Load = 0;
#endif

        bool mainChanged = !hasRendered
                        || view.wifiConnected != lastRendered.wifiConnected
                        || view.stellariumConnected != lastRendered.stellariumConnected
                        || strcmp(view.ipAddress, lastRendered.ipAddress) != 0
                        || view.stm32FirmwareVersion != lastRendered.stm32FirmwareVersion
                        || view.soundEnabled != lastRendered.soundEnabled
                        || view.motorsEnabled != lastRendered.motorsEnabled
                        || view.mountStatus != lastRendered.mountStatus
                        || displayIsNightMode() != lastNightMode;
#if ENABLE_CPU_LOAD_MONITOR
        bool cpuChanged = !hasRendered
                       || view.cpu0Load != lastRendered.cpu0Load
                       || view.cpu1Load != lastRendered.cpu1Load;
#endif

        if (mainChanged) {
            displayShowMainScreen(view.wifiConnected,
                                  view.stellariumConnected,
                                  view.ipAddress,
                                  view.stm32FirmwareVersion,
                                  view.soundEnabled,
                                  view.motorsEnabled,
                                  view.mountStatus,
                                  view.cpu0Load,
                                  view.cpu1Load);
            lastRendered = view;
            lastNightMode = displayIsNightMode();
            hasRendered = true;
#if ENABLE_CPU_LOAD_MONITOR
        } else if (cpuChanged) {
            displayShowCpuLoad(view.cpu0Load, view.cpu1Load);
            lastRendered.cpu0Load = view.cpu0Load;
            lastRendered.cpu1Load = view.cpu1Load;
#endif
        }

        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
}

void notifyDisplayTask() {
    if (displayTaskHandle) {
        xTaskNotifyGive(displayTaskHandle);
    }
}

bool connectWiFi() {
    constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000;

    setWifiLedState(WIFI_LED_DISCONNECTED);
    Serial.printf("Connessione a: %s", WIFI_SSID);
    WiFi.disconnect(true, true);
    delay(200);
    WiFi.mode(WIFI_STA);
#if WIFI_DISABLE_SLEEP
    WiFi.setSleep(false);
#endif
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    uint32_t startMs = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startMs < WIFI_CONNECT_TIMEOUT_MS) {
        delay(500);
        Serial.print(".");
    }

    if (WiFi.status() != WL_CONNECTED) {
        Serial.printf("\n[WiFi] Connessione fallita, status=%d\n", WiFi.status());
        return false;
    }

    setWifiLedState(WIFI_LED_CONNECTED);
    Serial.printf("\nConnesso! IP: %s\n", WiFi.localIP().toString().c_str());
    return true;
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
        telescope.setControllerDateTime(t);
    }
    else
        Serial.println("\nWARNING: NTP fallito!");
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
            telescope.getControllerDateTime(timeinfo);
            timeinfo.tm_year = (year < 100 ? year + 2000 : year) - 1900;
            timeinfo.tm_mon = month - 1;
            timeinfo.tm_mday = day;
            if (DEBUG_LX200) Serial.printf("[LX200] \":SC\" Date set to: %02d/%02d/%04d\n", month, day, timeinfo.tm_year + 1900);
            if (month >= 1 && month <= 12 && day >= 1 && day <= 31 && telescope.setControllerDateTime(timeinfo)) {
                controllerTimeManual = true;
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
        uint16_t axis = (cmd.startsWith(":Mn#") || cmd.startsWith(":Ms#")) ? JOG_AXIS_DEC : JOG_AXIS_RA;
        uint16_t direction = (cmd.startsWith(":Mn#") || cmd.startsWith(":Me#")) ? JOG_DIR_POSITIVE : JOG_DIR_NEGATIVE;
        mountLinkRequestJog(axis, direction, JOG_SPEED_CENTER);
        sendResponse("");
    }

    // --- PRODUCT NAME ---
    else if (cmd.startsWith(":GVP#")) {
        snprintf(reply, sizeof(reply), "%s#", ESP32_FIRMWARE_NAME);
        sendResponse(reply);
    }

    // --- VERSION ---
    else if (cmd.startsWith(":GVN#")) {
        snprintf(reply, sizeof(reply), "%02X.%02X#",
                 (ESP32_FIRMWARE_VERSION >> 8) & 0xFF,
                 ESP32_FIRMWARE_VERSION & 0xFF);
        sendResponse(reply);
    }

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
        MountLinkSnapshot state = mountLinkGetSnapshot();
        sendResponse(state.isSlewing ? "|#" : "#");
    }

    // --- ABORT :Q# ---
    else if (cmd.startsWith(":Q#") || cmd.startsWith("Q#")) {
        mountLinkRequestStop();
    }

    // --- ABORT MANUAL SLEW AXIS (:Qn#/:Qs#/:Qe#/:Qw#) ---
    else if (cmd.startsWith(":Qn#") || cmd.startsWith(":Qs#") ||
             cmd.startsWith(":Qe#") || cmd.startsWith(":Qw#")) {
        mountLinkRequestJogStop();
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
            telescope.getControllerDateTime(timeinfo);
            timeinfo.tm_hour = h;
            timeinfo.tm_min = m;
            timeinfo.tm_sec = s;
            
            // Aggiorna il telescope
            telescope.setControllerDateTime(timeinfo);
            controllerTimeManual = true;
            
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
    return mountLinkRequestGoto(targetRA_h, targetDEC_deg);
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
//  Invio posizione corrente a Stellarium (pacchetto 24 byte)
// =============================================================================
void sendCurrentPosition(WiFiClient& client) {
    if (!client || !client.connected()) return;

    MountLinkSnapshot state = mountLinkGetSnapshot();
    uint32_t ra_raw  = (uint32_t)((state.ra_h    / 24.0) * 0xFFFFFFFFUL);
    int32_t  dec_raw = (int32_t) ((state.dec_deg / 90.0) * 0x3FFFFFFFL);

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
