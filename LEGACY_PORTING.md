# Porting funzionalita legacy rDUINOScope 2.4.0

Questo documento riassume cosa manca alla repo ESP32-S3 per recuperare le
funzionalita del firmware legacy in:

`E:\Proj\rDUINOScope\_2.4.0_Leonardo_EQ_giacu92\fw`

Il main legacy e `_2.4.0_Leonardo_EQ_giacu92.ino`. I file studiati sono:

- `_2.4.0_Leonardo_EQ_giacu92.ino`: bootstrap hardware, stato globale, loop principale.
- `defines.h`: colori, note, stelle di allineamento, costanti orbitali.
- `functions.ino`: LST/HA, GOTO, tracking, manual move, BMP, opzioni, calibrazioni, batteria.
- `graphic_screens.ino`: tutte le schermate ILI9488.
- `touch_inputs.ino`: navigazione touch e azioni UI.
- `regular_updates.ino`: aggiornamenti tempo, GPS, temperatura, umidita, batteria.
- `planets_calc.ino`: calcolo Sole, pianeti e Luna.

`BT.ino` non va portato: serviva per la seriale LX200/Stellarium via Bluetooth,
mentre questa repo ha gia una implementazione WiFi/TCP LX200 e protocollo
binario Stellarium.

## Stato attuale della repo ESP32

La repo attuale implementa soprattutto il bridge di comunicazione:

- WiFi su ESP32-S3.
- Server TCP Stellarium su porta `10003`.
- Autodetect tra protocollo binario Stellarium Desktop e LX200 ASCII.
- Sincronizzazione NTP.
- Modbus RTU verso STM32 per inviare target RA/DEC e leggere stato/posizione.
- LED RGB di stato.
- Base display/touch Milestone 1 avviata con LovyanGFX: boot diagnostics,
  init/main screen, eventi touch debounced, griglia soft-key 2x3 sulla main,
  pulsante 5 per ruotare le funzioni, STOP da soft-key Mount e helper comuni
  per le schermate.

Non sono ancora presenti navigazione touch completa, cataloghi locali,
allineamento, sensori ambientali, GPS/RTC locali, storage offline cataloghi,
menu utente completo, joystick, buzzer, ventole e log osservazioni. Il night
mode base via GPIO e palette day/night e gia presente.

## Architettura da raggiungere

La vecchia scheda Arduino/Teensy faceva tutto in un solo firmware: UI, calcoli
astronomici, stepper e comunicazione. Nella nuova architettura va mantenuta la
separazione:

- ESP32-S3: UI, touchscreen, cataloghi, SD, GPS/RTC, sensori, opzioni, comandi
  utente, Stellarium/LX200 WiFi.
- STM32: pulse generation, accelerazione/decelerazione, tracking deterministico,
  stato motori, stop, errori, eventuali finecorsa/encoder.

Ogni funzione legacy che muove direttamente i pin step/dir va quindi tradotta in
un comando o stato condiviso con STM32, non copiata 1:1 sull'ESP32.

## Decisioni architetturali e note di progetto

Queste regole servono a evitare di ricreare il monolite legacy con hardware piu
moderno. Il porting deve recuperare i comportamenti utili, non copiare la
struttura `.ino` globale del vecchio firmware.

Decisioni vincolanti:

- ESP32 non genera step pulse.
- STM32 e proprietario del moto: pulse generation, tracking deterministico,
  accelerazione/decelerazione, stop, errori meccanici e sicurezza motori.
- ESP32 e proprietario di UI, cataloghi, rete, sorgenti tempo/posizione,
  aggiornamenti OTA, sensori utente e comandi alto livello.
- SD e lo storage principale per cataloghi, immagini, star map, report, backup,
  import/export e file scaricati.
- NVS/Preferences va usato solo per configurazione piccola e robusta.
- OTA firmware ESP32 solo manuale da interfaccia, mai automatico o silenzioso.
- GPS, RTC, NTP e LX200 devono avere priorita esplicite e non sovrascriversi in
  modo opaco.
- BME280 sostituisce DHT22.

Stato applicativo:

- Creare uno stato centrale, per esempio `MountState`/`AppState`, con tempo,
  sito, oggetto selezionato, coordinate attuali, target, tracking mode, stato
  STM32, stato sensori, stato WiFi/OTA e schermata corrente.
- La UI deve leggere lo stato e inviare eventi/comandi. Non deve manipolare
  direttamente motori, Modbus, SD e sensori in mezzo al rendering.
- I servizi hardware devono aggiornare lo stato con interfacce piccole e
  testabili.

Protocollo STM32 prima della UI avanzata:

- Prima di portare joystick, tracking solar/lunar, meridian flip e motors
  on/off serve estendere Modbus con comandi chiari.
- I bottoni UI devono inviare comandi alto livello, non replicare la vecchia
  logica step/dir.
- Lo stato STM32 deve essere leggibile in modo abbastanza ricco da mostrare
  all'utente cosa sta succedendo: idle, slewing, tracking, error, disabled,
  manual jog, eventuale flip.

Cataloghi e aggiornamenti dati:

- Aggiungere su SD un manifest, per esempio `/catalogs/manifest.json`, con
  versione cataloghi, data, checksum e lista file.
- L'update cataloghi da GitHub deve scaricare su file temporaneo, validare,
  chiedere conferma e solo dopo sostituire i file correnti.
- Evitare overwrite ciechi: se download o parse fallisce, restano validi i
  cataloghi precedenti.

Firmware OTA:

- Verificare presto il partizionamento in `platformio.ini`: OTA richiede una
  configurazione partizioni compatibile con il firmware finale.
- Il flusso OTA deve mostrare versione installata, versione remota, stato
  download, verifica e riavvio.
- L'aggiornamento firmware richiede alimentazione stabile e WiFi connesso.

Schermate consigliate:

- Tenere `Options` per impostazioni operative: luminosita, timeout, suono,
  motori, tracking, meridian flip.
- Aggiungere una schermata `System` o `About` per WiFi/IP, firmware, OTA, SD,
  RTC, GPS, BME280, Modbus e diagnostica.
- Il boot ESP32 ora usa `displayBootSetStatus()` per mostrare OK/FAIL/SKIP
  riga per riga. `mountLinkBegin()` valida la presenza STM32 leggendo
  `REG_RES_STM32_FW_VERSION`; `0xFFFF` viene trattato come valore da bus
  scollegato.
- Il pulsante `Controlla aggiornamenti` dovrebbe stare in `System`/`About`,
  oppure in una sezione chiaramente separata di `Options`.

Sorgenti tempo e posizione:

- La UI deve partire anche senza fix GPS, usando ultimo sito salvato o default.
- Il GPS non deve bloccare il boot: aggiorna posizione e tempo quando ha un fix
  valido.
- Il DS3231 va aggiornato solo da sorgenti affidabili: NTP valido, GPS con fix
  valido, oppure impostazione manuale confermata.
- Un client LX200/Stellarium non dovrebbe sovrascrivere automaticamente l'RTC
  senza una policy esplicita.

Sensore BME280:

- Il BME280 va posizionato lontano da ESP32, regolatori e driver motori se deve
  misurare l'ambiente reale.
- Se resta dentro il box, va documentato come temperatura interna controller,
  non come temperatura ambiente.
- Log e UI devono chiarire cosa rappresenta la misura.

Diagnostica e sviluppo:

- Aggiungere un log rotante su SD, per esempio `/logs/system.log`, con boot,
  errori sensori, errori Modbus, mount SD, GOTO falliti, OTA e aggiornamenti
  cataloghi.
- Prevedere una modalita `simulator`/`dry run` per sviluppare UI e cataloghi
  senza STM32 o motori collegati.
- In modalita simulatore il Modbus puo essere finto e la posizione puo convergere
  gradualmente al target.

Safety:

- Tracking e GOTO del Sole devono richiedere conferma esplicita per sessione.
- Il meridian flip non deve partire durante manual jog o durante un altro stato
  critico.
- Stop e motors off devono avere priorita su ogni altra azione.
- Oggetti sotto orizzonte devono bloccare GOTO/tracking locale, salvo override
  esplicito e consapevole se mai verra aggiunto.

Qualita codice:

- Evitare porting massivo dei file `.ino`.
- Portare moduli testabili: astronomia, cataloghi, stato mount, storage,
  sorgenti tempo, UI, hardware services.
- Limitare l'uso di `String` nei parser cataloghi e nei loop frequenti per
  ridurre frammentazione heap.
- Le funzioni bloccanti legacy con `delay()`, attese touch o loop SD vanno
  riscritte con task, timer o operazioni brevi compatibili con WiFi e OTA.

## Hardware e periferiche da portare

### Display ILI9488

Il legacy usa un display ILI9488 SPI 480x320, con layout disegnato in coordinate
portrait 320x480. Sul vecchio codice:

- Arduino Due: `TFT_CS=47`, `TFT_DC=48`, `TFT_RST=46`, libreria `ILI9488_DMA`.
- Teensy 4.1: `TFT_CS=10`, `TFT_DC=9`, `TFT_RST=23`, libreria `ILI9488_t3`.
- Rendering basato su primitive Adafruit GFX e `bmpDraw()` da SD.
- Backlight PWM su `TFTBright=13`.
- Luminosita salvata in `options.txt`.
- Timeout spegnimento display: always on, 30 s, 60 s, 2 min, 5 min, 10 min.

Da fare:

- Scegliere libreria ESP32 per ILI9488. Candidati: `TFT_eSPI`, `Arduino_GFX`,
  oppure driver ILI9488 compatibile Adafruit GFX.
- Definire pin ESP32-S3 per `TFT_CS`, `TFT_DC`, `TFT_RST`, `SCK`, `MOSI`, `MISO`
  e backlight PWM.
- Portare una classe/display service invece di funzioni globali monolitiche.
- Verificare orientamento 320x480 e rimappare coordinate se necessario.
- Portare `DrawButton()`, status bar, palette day/night e primitive comuni.
- Portare `bmpDraw()` o sostituirlo con decoder BMP/JPEG/PNG adatto a ESP32.
- Ridurre i redraw full-screen dove possibile: ILI9488 a 24 bit/pixel e SPI puo
  diventare lento.

### Touchscreen AD7873/XPT2046/TSC2046

Il legacy usa `XPT2046_Touchscreen` con:

- `TP_CS=4`
- `TP_IRQ=2`
- `myTouch.tirqTouched()`, `myTouch.touched()`, `myTouch.getPoint()`
- soglia `p.z < 600` per filtrare rumore
- calibrazione iniziale fissa `tx=(p.x-257)/calx`, `ty=(p.y-445)/caly`
- procedura completa `calibrateTFT()` a 8 punti

Da fare:

- Scegliere pin ESP32-S3 separando il vecchio `TP_CS=4`, perche nella repo
  attuale GPIO 4 e gia `RS485_DE_PIN`.
- Portare driver touch e interrupt IRQ.
- Portare calibrazione touch, salvataggio parametri e test post-calibrazione.
- Creare router eventi touch equivalente a `considerTouchInput()`. Base fatta:
  eventi press/release debounced, soft-key 2x3 sulla main, routing schermate e
  azione STOP come comando UI alto livello verso il mount link.
- Gestire wake-up display al primo touch quando il backlight e spento.

### SD e dati offline

Il legacy richiedeva SD per:

- `messier.csv`
- `treasure.csv`
- `custom.csv`
- `options.txt`
- immagini oggetti in `objects/<OBJECT_NAME>.bmp`
- mappe stellari `<row>-<col>.bmp`
- log/report osservazioni, oggi parzialmente dentro flussi legacy/BT

Scelta aggiornata: teniamo la SD come storage principale e tagliamo la testa al
toro. Cataloghi, immagini, star map, report e backup/import/export vivono sulla
SD. Le opzioni piccole possono restare in NVS/Preferences per robustezza, con
eventuale export/import su SD.

Resta da valutare solo una eventuale SPI flash esterna come alternativa futura,
ma non entra nel piano iniziale di porting.

Si puo comunque prevedere download WiFi da repository remoto, per esempio GitHub
raw/release asset, salvando gli aggiornamenti direttamente su SD dopo conferma
utente.

Da fare:

- Decidere se usare SD SPI esterna o SD_MMC su ESP32-S3.
- Definire pin CS o bus SD_MMC.
- Portare loader cataloghi con parser robusto CSV/JSON, evitando parsing fragile con
  indici `String` dove possibile.
- Definire struttura directory sulla SD:
  - `/catalogs/messier.csv`
  - `/catalogs/treasure.csv`
  - `/catalogs/custom.csv`
  - `/objects/<OBJECT_NAME>.bmp`
  - `/starmaps/<row>-<col>.bmp`
  - `/reports/...`
- Migrare persistenza opzioni da `options.txt` a NVS/Preferences, con eventuale
  import/export da SD.
- Portare asset BMP necessari o convertirli in un formato piu adatto.
- Aggiungere updater cataloghi:
  - controllo versione remota
  - download su file temporaneo
  - validazione formato
  - conferma overwrite
  - rollback se download o parse fallisce.

### Aggiornamento firmware ESP32 via OTA

La nuova interfaccia grafica dovra permettere anche l'aggiornamento del firmware
ESP32 direttamente dal display, quando il controller e connesso a Internet.

Scelta prevista:

- Usare `safeGitHubOTA` o una libreria equivalente per aggiornare il firmware da
  GitHub release/raw asset in modo controllato.
- Aggiungere in una schermata grafica un pulsante tipo `Controlla aggiornamenti`.
- Mostrare versione installata, versione remota disponibile, changelog sintetico
  se disponibile, dimensione download e richiesta conferma.
- Scaricare e applicare l'update solo con alimentazione stabile e WiFi connesso.
- Prevedere messaggi chiari per:
  - nessuna connessione Internet
  - nessun aggiornamento disponibile
  - download fallito
  - verifica fallita
  - aggiornamento completato e riavvio richiesto
- Conservare un percorso di recovery: niente update automatici silenziosi e
  nessun overwrite se la verifica fallisce.

Da fare:

- Definire repository/URL release firmware.
- Decidere formato metadati update, per esempio JSON con versione, URL binario,
  checksum e changelog breve.
- Integrare controllo aggiornamenti nella UI, probabilmente in Options o in una
  futura schermata System/About.
- Aggiungere watchdog/progress UI durante download e flash.
- Verificare compatibilita con partizioni OTA ESP32-S3 in `platformio.ini`.

### GPS U-Blox NEO

Il legacy usa `TinyGPSPlus` su `Serial2.begin(9600)` e aggiorna:

- latitudine, longitudine, altitudine
- data/ora
- timezone ricavata dalla longitudine
- ora legale/invernale tramite `isSummerTime()`
- passaggio da schermata GPS a schermata orologio

Da fare:

- Definire UART e pin ESP32-S3 per GPS.
- Integrare GPS con il modello tempo gia presente, che oggi usa NTP/LX200.
- Stabilire precedenza sorgenti tempo: LX200 manuale quando impostato
  dall'utente, NTP se disponibile, GPS se ha fix valido, RTC come fallback
  immediato all'avvio.
- Stabilire precedenza posizione: GPS con fix valido, coordinate impostate da UI,
  coordinate LX200/Stellarium, default di config.
- Portare calcolo timezone e DST, o sostituirlo con una logica timezone piu
  esplicita/configurabile.
- Portare schermata GPS con stato satelliti e fallback valori locali.

Anche usando `pool.ntp.org`, la posizione del telescopio serve comunque:

- latitudine e longitudine sono necessarie per LST, HA, ALT/AZ e visibilita
  sopra orizzonte.
- servono per decidere se un oggetto e osservabile.
- servono per meridian flip e calcoli locali.
- Stellarium puo inviare coordinate sito via LX200, ma non sempre e la sorgente
  primaria.
- altitudine non e critica quanto LAT/LON per la maggior parte dei calcoli GOTO,
  ma va mantenuta per completezza, log e future correzioni.

Conclusione consigliata: tenere GPS come sorgente automatica di posizione e come
fallback tempo offline. NTP resta la sorgente tempo piu comoda quando c'e WiFi.

### RTC DS3231 e batteria tampone

Il legacy usa DS3231 via I2C:

- Due: SDA/SCL 20/21.
- Teensy: SDA/SCL 18/19.
- temperatura RTC usata come test presenza.
- `rtc.getTimeStr()`, `rtc.getDateStr()`, `rtc.setTime()`, `rtc.setDate()`.

L'ESP32 ha RTC interno, ma non sostituisce completamente un DS3231 con batteria
tampone:

- L'RTC interno mantiene il tempo solo se il chip resta alimentato o in deep
  sleep con alimentazione presente.
- Non c'e una batteria tampone equivalente integrata nei devkit ESP32-S3 comuni.
- Dopo power-off completo, senza rete e senza GPS, l'ESP32 non sa piu l'ora
  corretta.
- NTP risolve il problema solo quando il WiFi e disponibile.
- GPS risolve il tempo anche offline, ma richiede fix/visibilita satelliti e un
  tempo di acquisizione.

Conclusione consigliata: mantenere un RTC esterno con batteria tampone se
vogliamo avvio affidabile anche offline, senza WiFi e prima del fix GPS. Il
DS3231 resta una buona scelta per precisione e semplicita I2C.

Alternative possibili:

- Solo NTP: hardware minimo, ma richiede rete.
- Solo GPS: indipendente da Internet, ma non immediato e dipende dal segnale.
- RTC interno ESP32 piu salvataggio ultimo epoch in NVS: utile come fallback
  approssimato, non affidabile dopo lunghi spegnimenti.
- DS3231 o equivalente I2C con coin cell/supercap: soluzione piu robusta.

Da fare:

- Definire pin I2C ESP32-S3.
- Scegliere libreria DS3231 mantenuta per ESP32.
- Integrare RTC come sorgente/offline backup rispetto a NTP/GPS.
- Aggiornare RTC quando NTP o GPS sono affidabili.
- Portare schermata clock e modifica manuale data/ora.

### BME280 e ambiente

Il legacy usa DHT22 su `DHTPIN=3`, aggiornato circa ogni 30 secondi:

- temperatura
- umidita
- visualizzazione su main screen
- dati inseriti nei log osservazione

Per la nuova versione sostituiamo DHT22 direttamente con BME280:

- BME280 usa I2C/SPI, e piu stabile e generalmente piu affidabile del DHT22.
- Fornisce temperatura, umidita e pressione atmosferica.
- Mantiene la voce `Hum` del legacy senza cambiare UX.
- La pressione puo essere utile per log ambientale e, in futuro, per correzioni
  atmosferiche/refrazione.
- Aggiornare UI e report per mostrare `Temp`, `Hum` e `Press`.

Da fare:

- Definire bus I2C/SPI condiviso con RTC/touch/display dove sensato.
- Aggiungere libreria BME280 compatibile ESP32.
- Portare lettura sensore in task/timer non bloccante.
- Aggiungere stato sensore non disponibile.
- Collegare valori a UI e log.

### Batteria

Il legacy ha `use_battery_level` e:

- `calculateBatteryLevel()`
- `drawBatteryLevel()`
- curva empirica SLA in `functions.ino`
- icona batteria nella status bar

Da fare:

- Definire ingresso ADC ESP32-S3 e partitore reale.
- Ricalibrare curva tensione-percentuale per la batteria effettiva.
- Proteggere l'ADC con scaling corretto.
- Portare icona batteria e allarmi eventuali.

### Joystick analogico

Il legacy usa:

- `xPin=A1`
- `yPin=A0`
- `Joy_SW=A11`
- calibrazione automatica startup con circa 3 secondi di letture
- deadband circa `+-100`
- `consider_Manual_Move()` per muovere RA/DEC
- microstepping variabile in base alla velocita richiesta

Da fare:

- Definire ingressi ADC ESP32-S3 per joystick.
- Portare calibrazione startup.
- Tradurre manual move in comandi verso STM32, non in pulse diretti ESP32.
- Estendere protocollo Modbus con comandi jog/manual:
  - asse RA/DEC
  - direzione
  - velocita o profilo
  - start/stop
  - eventuale microstepping se ancora gestito lato driver/STM32.

### Buzzer/speaker

Il legacy usa `speakerOut` e `SoundOn(note, duration)`:

- conferma boot con melodia.
- beep su meridian flip.
- beep su conferme GOTO/allineamento.
- opzione Sound ON/OFF.

Da fare:

- Definire pin PWM/DAC ESP32-S3.
- Portare `SoundOn()` usando LEDC/PWM ESP32.
- Rendere i suoni non bloccanti o comunque brevi.
- Collegare opzione Sound ON/OFF.

### Ventole e uscite ausiliarie

Il legacy usa:

- `FAN1=37`
- `FAN2=39`
- toggle da main menu.
- FAN1 anche PWM `analogWrite(FAN1, 100)`.
- stati salvati/caricati in opzioni.

Da fare:

- Definire pin ESP32-S3 per FAN1/FAN2.
- Decidere se sono GPIO on/off o PWM.
- Portare controlli UI e persistenza stati.
- Valutare protezioni hardware se pilotano carichi a 12/13 V.

### Power driver stepper

Il legacy controlla direttamente:

- `POWER_DRV8825=A8`
- `RA_STP`, `RA_DIR`, `DEC_STP`, `DEC_DIR`
- pin microstepping `RA_MODE0/1/2`, `DEC_MODE0/1/2`
- flag `reverse_logic`

Nella nuova architettura questa parte deve stare sullo STM32, non sull'ESP32.

Da fare lato ESP32:

- Esporre solo comandi utente: motors ON/OFF, stop, tracking mode, goto, jog.
- Estendere Modbus per inoltrare questi comandi allo STM32.
- Visualizzare stato ricevuto dallo STM32.

Da fare lato STM32:

- Implementare o verificare power enable driver.
- Implementare modalita tracking sidereal/solar/lunar.
- Implementare manual jog e stop.
- Gestire meridian flip se la decisione resta lato motore.

## Schermate grafiche da portare

Il legacy usa `CURRENT_SCREEN` come stato globale:

| ID | Funzione legacy | Scopo |
| --- | --- | --- |
| 0 | `drawGPSScreen()` | Stato GPS, coordinate, acquisizione posizione |
| 1 | `drawClockScreen()` | Data/ora, modifica manuale, passaggio ad allineamento |
| 3 | `drawSelectAlignment()` | 1-star, iterative align, skip |
| 4 | `drawMainScreen()` | Dashboard osservazione, oggetto, tempo, LST, ambiente |
| 5 | `drawCoordinatesScreen()` | Coordinate correnti da posizione motori |
| 6 | `drawLoadScreen()` | Selezione sorgente oggetti |
| 7 | `drawOptionsScreen()` | Luminosita, timeout, tracking, meridian flip, suono, motori, update |
| 10 | `drawSTATScreen()` | Report sessione osservativa |
| 11 | `drawStarMap()` | Mappa stellare navigabile tramite BMP |
| 12 | `drawStarSyncScreen()` | Scelta stella per allineamento |
| 13 | `drawConstelationScreen()` | Procedura di centratura/allineamento |
| 14 | `drawTFTCalibrationScreen()` | Calibrazione touch |
| 15 | `drawConfirmSunTrack()` | Conferma sicurezza tracking Sole |

Da fare:

- Creare un modulo UI con screen enum al posto di interi globali.
- Portare prima le schermate minime in ordine utile:
  1. boot/status hardware
  2. main screen
  3. options
  4. load objects
  5. GPS/clock
  6. alignment
  7. coordinates/statistics/star map/calibration
- Separare rendering e azioni: il legacy mischia draw, stato e logica.
- Portare `OnScreenMsg()` per messaggi di errore/conferma:
  - meridian flip
  - oggetto sotto orizzonte
  - GOTO completato
  - errori SD/touch/sensori
  - controllo e aggiornamento firmware OTA

## Cataloghi e oggetti

Il legacy gestisce:

- `Messier_Array[112]` da `messier.csv`.
- `Treasure_Array[130]` da `treasure.csv`.
- `custom_Array[100]` da `custom.csv`.
- `Stars[]` in `defines.h` per allineamento.
- Solar System objects da `ss_planet_names`.
- descrizione custom aggiunta a fine riga CSV.
- immagini oggetto da `objects/<nome>.bmp`.

Da fare:

- Definire modello dati comune:
  - nome
  - tipo/catalogo
  - RA
  - DEC
  - costellazione
  - magnitudine
  - dimensione
  - descrizione
  - path immagine opzionale
- Portare parsing di Messier/Treasure/custom.
- Spostare `Stars[]` in una struttura dati piu leggibile.
- Gestire paginazione oggetti come nel legacy:
  - `MESS_PAGER`
  - `TREAS_PAGER`
  - `STARS_PAGER`
  - `CUSTOM_PAGER`
- Verificare se "NGC" era solo etichetta UI o se manca un file catalogo non
  presente nella cartella `fw`.

## Calcoli astronomici

Da portare o consolidare:

- `calculateLST_HA()`: LST, HA, ALT/AZ, visibilita sopra orizzonte.
- `Current_RA_DEC()`: coordinate correnti derivate dai microstep/posizione.
- `planet_pos()`: Sole, Mercurio, Venere, Marte, Giove, Saturno, Urano, Nettuno,
  Plutone, Luna.
- helper astronomici: `J2000()`, `dayno()`, `frange()`, `fkep()`, `fnatan()`,
  `FNdegmin()`.
- gestione oggetto sotto orizzonte: blocco GOTO/tracking se `ALT <= 0`.
- tracking selezionabile:
  - `Tracking_type=1`: sidereal/celestial
  - `Tracking_type=2`: solar
  - `Tracking_type=0`: lunar
- conferma esplicita per tracking Sole.

Da fare:

- Evitare duplicazione con `Telescope::getLST_hours()` gia presente.
- Definire una libreria `Astronomy` testabile su host, senza dipendere da TFT,
  SD o globali Arduino.
- Aggiungere test per LST/HA/ALT/AZ e coordinate pianeti su date note.
- Decidere se aggiornare gli algoritmi lunari, perche il commento legacy dice
  che l'errore della Luna peggiora col tempo.

## Allineamento e GOTO locale

Il legacy implementa:

- skip alignment.
- 1-star alignment basato sulla procedura Ralph Pass.
- iterative alignment.
- selezione stella, slew, centratura manuale, conferma.
- correzioni `delta_a_RA` e `delta_a_DEC`.
- stelle iterative in `Iter_Stars`.
- uso di `ALLIGN_STEP`, `ALLIGN_TYPE`, `SELECTED_STAR`, `Iterative_Star_Index`.

Da fare:

- Portare workflow UI di allineamento.
- Isolare matematica allineamento in modulo testabile.
- Salvare offset allineamento nello stato mount.
- Applicare offset sia a GOTO locali sia a coordinate correnti mostrate.
- Definire come sincronizzare offset con STM32:
  - ESP32 puo correggere target prima di inviarlo.
  - STM32 puo restare ignorante di RA/DEC corrette.
  - oppure STM32 riceve offset/stato mount, scelta piu complessa.

## Meridian flip

Il legacy gestisce:

- `MIN_TO_MERIDIAN_FLIP=2`
- `MIN_SOUND_BEFORE_FLIP=3`
- `IS_MERIDIAN_FLIP_AUTOMATIC`
- `IS_POSIBLE_MERIDIAN_FLIP`
- avviso sonoro prima del flip
- stop tracking
- nuovo slew coordinato
- opzione Auto/OFF

Da fare:

- Decidere proprietario del meridian flip:
  - ESP32 decide in base a LST/HA e invia nuovo GOTO.
  - STM32 gestisce safety e meccanica.
- Estendere Modbus con stato flip/safety se serve.
- Portare UI opzione Auto/OFF.
- Portare warning sonoro e messaggi a schermo.
- Aggiungere blocchi di sicurezza contro flip durante manual move o GOTO gia in
  corso.

## Opzioni persistenti

Il legacy salva in `options.txt`:

- `TFT_Brightness`
- `TFT_Time`
- `Tracking_type`
- `IS_MERIDIAN_FLIP_AUTOMATIC`
- `Fan1_State`
- `Fan2_State`
- `Sound_State`
- `Stepper_State`

Da fare:

- Portare schema opzioni.
- Scegliere backend: SD `options.txt`, ESP32 Preferences/NVS, o entrambi.
- Gestire defaults robusti se il file manca o e corrotto.
- Aggiornare UI appena si modifica un'opzione.

## Sessione osservativa e statistiche

Il legacy mantiene `ObservedObjects[50]`:

- nome oggetto
- dettagli
- ora inizio osservazione
- temperatura/umidita
- HA
- altitudine
- durata osservazione

Da fare:

- Portare `UpdateObservedObjects()`.
- Portare `drawSTATScreen()`.
- Aggiungere export report su SD, esclusa la parte BT.
- Gestire limite 50 oggetti o renderlo configurabile.
- Decidere formato report: testo legacy, CSV o JSON.

## Protocollo ESP32-STM32 da estendere

La mappa Modbus attuale copre:

- target RA/DEC
- comando GOTO/STOP base
- stato
- posizione corrente
- error code

Per portare la ciccia serve aggiungere comandi/register per:

- tracking ON/OFF.
- tracking mode sidereal/solar/lunar.
- manual jog RA/DEC con direzione e velocita.
- motors power ON/OFF.
- meridian flip request/status.
- sync/allineamento o set current coordinate.
- stato oggetto raggiunto, errore meccanico, driver disabled.
- eventuali finecorsa/encoder/hard stop se gestiti dallo STM32.

## Milestone consigliate

### Milestone 0: contratto mount ESP32-STM32

- Estendere la mappa Modbus senza spostare i registri GOTO esistenti.
- Aggiungere comandi high-level per:
  - tracking ON/OFF.
  - tracking mode sidereal/solar/lunar.
  - manual jog RA/DEC con direzione e velocita.
  - motors power ON/OFF.
  - stop prioritario.
- Mantenere compatibilita numerica con lo STM32 attuale:
  - `CMD_SYNC=3`
  - `CMD_FOLLOW_TARGET=4`
  - nuovi comandi da `5` in poi.
- Preferire `CMD_JOG_START`/`CMD_JOG_STOP` dedicati al posto di usare
  `FOLLOW_TARGET` per il manual move: il jog deve restare deterministico lato
  STM32, con rampe, limiti, velocita e safety gestiti dal firmware motori.
- Aggiungere stati STM32 piu espressivi:
  - idle
  - slewing
  - tracking
  - error
  - disabled
  - manual jog
- Tenere ESP32 come mittente di intenti: nessuna generazione step/pulse lato
  ESP32.
- Preparare funzioni ESP32 riusabili dalla futura UI:
  - `requestStop()`
  - `requestTrackingEnabled()`
  - `requestTrackingMode()`
  - `requestMotorsEnabled()`
  - `requestJog()`
  - `requestJogStop()`
- Documentare la nuova boundary nel README per mantenere STM32 e ESP32
  allineati.

### Milestone 1: base display e touch

- Aggiungere librerie display/touch a `platformio.ini`. Fatto con
  `lovyan03/LovyanGFX`.
- Definire pin in `config.h`. Fatto con pin provvisori da verificare sul
  cablaggio finale.
- Mostrare boot screen e main screen statico. Fatto; presente anche init screen.
- Leggere touch e navigare sulla main. Fatto come base: eventi press/release
  debounced, griglia 2x3 con pulsante 5 per ruotare le funzioni e Options
  placeholder raggiungibile dalla pagina System.
- Collegare STOP UI a STM32. Fatto come azione high-level dalla pagina Mount:
  il tasto 1 richiede `CMD_STOP` con priorita anche durante il polling GOTO,
  senza usare `REG_REQ_TRACKING_ENABLE=0` come stop implicito. Con STM32
  `0xB004`, ESP32 considera lo stop completato quando `REG_RES_STATUS` torna
  `IDLE` e conserva la posizione reale letta dai registri `RES_CURRENT_*`.
- Backlight PWM fatto; timeout ancora da implementare.

### Milestone 2: storage e cataloghi

- Inizializzare SD.
- Caricare `messier.csv`, `treasure.csv`, `custom.csv`.
- Mostrare load screen e dettagli oggetto.
- Disegnare BMP oggetto se presente.
- Aggiungere aggiornamento cataloghi via WiFi/GitHub con conferma overwrite.
- Preparare partizioni e prerequisiti per OTA firmware ESP32.

### Milestone 3: astronomia locale

- Portare LST/HA/ALT/AZ.
- Portare pianeti e Luna.
- Portare visibilita sopra orizzonte.
- Collegare scelta oggetto a GOTO gia esistente via Modbus.

### Milestone 4: sensori e tempo

- Portare RTC DS3231.
- Portare GPS TinyGPSPlus.
- Portare BME280 al posto del DHT22 legacy.
- Portare batteria.
- Definire priorita NTP/GPS/RTC/LX200.

### Milestone 5: controllo mount avanzato

- Estendere Modbus per tracking mode, stop, motors on/off, manual jog.
- Portare joystick.
- Portare opzioni motori/tracking.
- Portare meridian flip.

### Milestone 6: allineamento e sessioni

- Portare stelle allineamento.
- Portare 1-star e iterative alignment.
- Portare statistiche e log osservazioni.
- Portare star map BMP navigabile.
- Portare calibrazione touch completa.

### Milestone 7: manutenzione e update

- Aggiungere schermata o sezione UI per versione firmware e aggiornamenti.
- Integrare `safeGitHubOTA` o equivalente.
- Implementare pulsante `Controlla aggiornamenti`.
- Scaricare firmware da GitHub solo dopo conferma utente.
- Verificare checksum/metadati prima del flash.
- Mostrare progress, esito e richiesta di riavvio.

## Dipendenze PlatformIO previste

Da valutare e aggiungere in modo incrementale:

- Driver ILI9488 per ESP32-S3: `TFT_eSPI` o `Arduino_GFX`.
- `XPT2046_Touchscreen`.
- `TinyGPSPlus`.
- Libreria DS3231/RTClib compatibile ESP32.
- Libreria BME280, per esempio Adafruit BME280 o equivalente.
- Driver SD/SdFat se serve piu controllo del filesystem.
- `safeGitHubOTA` o libreria OTA equivalente per update firmware ESP32 da UI.

## Note importanti dal legacy

- Il codice legacy contiene molto stato globale e side effect tra schermate,
  touch e motori. Conviene portare per moduli, non fare copia/incolla massivo.
- Alcuni nomi hanno typo storici (`ALLIGN`, `cosiderSlewTo`) e si possono
  correggere nella nuova architettura.
- Molte funzioni legacy bloccano con `delay()`, loop touch o attese SD; su ESP32
  con WiFi conviene usare task/timer o blocchi molto brevi.
- Il vecchio firmware usa `String` estesamente; su ESP32 e meglio limitare
  frammentazione heap per cataloghi e parser.
- La parte BT va ignorata, ma alcune funzioni richiamate anche da touch/UI
  devono restare: selezione oggetto, osservazioni, GOTO, tracking, suoni.
