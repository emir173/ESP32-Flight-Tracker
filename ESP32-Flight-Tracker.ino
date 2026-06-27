// ============================================================
//  E-OS FLIGHT TRACKER — OpenSky Network Uçak Takibi
//  ESP32-S3 N16R8 · 160x128 TFT + 128x64 OLED
//
//  Bu firmware E-OS launcher'dan OTA ile yüklenir (flight.bin).
//  OpenSky Network API'sinden gerçek uçak verisi çeker.
//
//  Kontroller:
//    Joystick <> : Haritayı doğu/batı kaydır (pan)
//    Joystick ^v : Haritayı kuzey/güney kaydır (pan)
//    Joystick SW : Reset viewport (Türkiye'ye dön)
//    BTN_A : Zoom in (+)
//    BTN_B : Zoom out (-)
//    BTN_C : En yakın uçağı seç / seçimi kaldır
//  Gereksinim: ArduinoJson kütüphanesi (Benoit Blanchon v7.x)
// ============================================================

#include "flight_internal.h"
#include <SPI.h>
#include <SD.h>

// ══════════════════════════════════════════════════════════════
//  GLOBAL TANIMLAMALAR
// ══════════════════════════════════════════════════════════════
TFT_eSPI tft = TFT_eSPI();
TFT_eSprite spr = TFT_eSprite(&tft);
uint16_t* g_psramMap = NULL;
uint16_t* g_psramMapWorld = NULL;
U8G2_SH1106_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);

Aircraft  g_aircraft[MAX_AIRCRAFT];
int       g_aircraftCount = 0;
Viewport  g_view;
bool      g_soundEnabled = true;
int       g_selectedIdx = -1;
int       g_joyCenterX = 0;
int       g_joyCenterY = 0;
bool      g_dataValid = false;
uint32_t  g_lastUpdate_ms = 0;
bool      g_polling = false;       // Poll sırasında UPDATING göstergesi
SemaphoreHandle_t g_aircraftMutex = NULL;
TaskHandle_t g_fetchTaskHandle = NULL;

// ─── Poll zamanlayıcı ───
static uint32_t s_lastPoll = 0;
static bool     s_firstPollDone = false;

// ─── Render zamanlayıcı ───
static uint32_t s_lastRender = 0;

void netFetchTask(void *pvParameters) {
    while (1) {
        if (millis() - s_lastPoll > POLL_INTERVAL_MS && netIsConnected()) {
            g_polling = true;
            bool ok = netFetchAircraft();
            g_polling = false;
            s_lastPoll = millis();
            if (ok) {
                s_firstPollDone = true;
            }
        }
        vTaskDelay(100 / portTICK_PERIOD_MS); // 100ms'de bir kontrol et
    }
}

// ══════════════════════════════════════════════════════════════
//  setup — Donanım başlatma + WiFi + ilk veri
// ══════════════════════════════════════════════════════════════
void setup() {
    g_aircraftMutex = xSemaphoreCreateMutex();
    
    Serial.begin(115200);
    delay(500);
    Serial.println();
    Serial.println("==================================");
    Serial.println("  E-OS FLIGHT TRACKER starting...");
    Serial.println("==================================");
    Serial.flush();

    // ─── Pin modları ───
    Serial.println("[SETUP] Pin modlari...");
    Serial.flush();
    pinMode(BTN_A, INPUT_PULLUP);
    pinMode(BTN_B, INPUT_PULLUP);
    pinMode(BTN_C, INPUT_PULLUP);
    pinMode(JOY_SW, INPUT_PULLUP);
    pinMode(BUZZER, OUTPUT);
    digitalWrite(BUZZER, LOW);
    pinMode(TFT_CS, OUTPUT); digitalWrite(TFT_CS, HIGH);
    
    // ─── SD Kart Başlatma ───
    pinMode(SD_CS, OUTPUT);
    digitalWrite(SD_CS, HIGH); // Önce HIGH yapıp TFT'nin SPI'ı kullanmasını güvenceye alalım

    Serial.println("[SETUP] Pin modlari OK");
    Serial.flush();

    // ─── I2C (OLED) ───
    Serial.println("[SETUP] I2C...");
    Serial.flush();
    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(400000);
    Serial.println("[SETUP] I2C OK");
    Serial.flush();

    SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, -1);
    delay(50);
    
    // SD Kart Başlatma & PSRAM'e Yükleme (Hata durumunda 3 kez tekrar dener)
    bool sdOk = false;
    for (int i = 0; i < 3; i++) {
        if (SD.begin(SD_CS, SPI, 10000000)) {
            sdOk = true;
            break;
        }
        delay(300); // Başarısız olursa 300ms bekle ve tekrar dene
    }

    if (sdOk) {
        Serial.println("[SETUP] SD Card OK");
        // Regional harita (Türkiye 2048x1024, 4MB)
        if (SD.exists("/regional.bin")) {
            File f = SD.open("/regional.bin", FILE_READ);
            size_t mapSize = f.size();
            Serial.printf("[SETUP] Regional harita: %d bytes\n", mapSize);
            g_psramMap = (uint16_t*)heap_caps_malloc(mapSize, MALLOC_CAP_SPIRAM);
            if (g_psramMap) {
                f.read((uint8_t*)g_psramMap, mapSize);
                Serial.println("[SETUP] Regional harita PSRAM'e yuklendi.");
            } else {
                Serial.println("[SETUP] HATA: Regional PSRAM yetersiz!");
            }
            f.close();
        } else {
            Serial.println("[SETUP] /regional.bin bulunamadi!");
        }
        // Dünya haritası (512x256, 256KB)
        if (SD.exists("/world.bin")) {
            File f = SD.open("/world.bin", FILE_READ);
            size_t mapSize = f.size();
            Serial.printf("[SETUP] World harita: %d bytes\n", mapSize);
            g_psramMapWorld = (uint16_t*)heap_caps_malloc(mapSize, MALLOC_CAP_SPIRAM);
            if (g_psramMapWorld) {
                f.read((uint8_t*)g_psramMapWorld, mapSize);
                Serial.println("[SETUP] World harita PSRAM'e yuklendi.");
            } else {
                Serial.println("[SETUP] HATA: World PSRAM yetersiz!");
            }
            f.close();
        } else {
            Serial.println("[SETUP] /world.bin bulunamadi!");
        }
        // SPI çakışmasını önlemek için SD kartı güvenlice kapat
        SD.end(); 
        pinMode(SD_CS, OUTPUT);
        digitalWrite(SD_CS, HIGH);
    } else {
        Serial.println("[SETUP] SD Card FAILED (harita yok)");
    }
    
    delay(50);
    tft.init();
    tft.setRotation(1);
    tft.setSwapBytes(true);
    tft.fillScreen(TFT_BLACK);

    // Sprite oluştur (160x128 = 40KB RAM)
    spr.createSprite(160, 128);
    spr.setSwapBytes(true); 
    
    Serial.println("[SETUP] TFT OK");
    Serial.flush();

    // ─── Renk paleti ───
    mapSetupColors();
    Serial.println("[SETUP] Renk paleti OK");
    Serial.flush();

    // ─── NVS'ten viewport + ses oku ───
    Serial.println("[SETUP] NVS...");
    Serial.flush();
    ctrlLoadView();
    Serial.print("[SETUP] Viewport: lat=");
    Serial.print(g_view.center_lat);
    Serial.print(" lon=");
    Serial.print(g_view.center_lon);
    Serial.print(" zoom=");
    Serial.println(g_view.zoom);
    Serial.println("[SETUP] NVS OK");
    Serial.flush();

    // ─── Joystick kalibrasyon ───
    Serial.println("[SETUP] Joystick kalibrasyon...");
    Serial.flush();
    delay(100);
    g_joyCenterX = analogRead(JOY_X);
    delay(10);
    g_joyCenterY = analogRead(JOY_Y);
    Serial.print("[SETUP] Joystick OK (X=");
    Serial.print(g_joyCenterX);
    Serial.print(" Y=");
    Serial.print(g_joyCenterY);
    Serial.println(")");
    Serial.flush();

    // ─── "Bağlanıyor" ekranı ───
    Serial.println("[SETUP] Connecting ekrani...");
    Serial.flush();
    renderDrawConnecting();
    Serial.println("[SETUP] Connecting OK");
    Serial.flush();

    // ─── WiFi ───
    Serial.println("[SETUP] WiFi baglaniliyor...");
    Serial.flush();
    bool wifiOk = netConnectWiFi();
    Serial.print("[SETUP] WiFi sonuc: ");
    Serial.println(wifiOk ? "BAGLANDI" : "BASARISIZ");
    Serial.flush();

    // ─── OLED başlat ───
    Serial.println("[SETUP] OLED baslatiliyor...");
    Serial.flush();
    renderSetup();
    Serial.println("[SETUP] OLED OK");
    Serial.flush();

    // ─── İlk poll ───
    if (netIsConnected()) {
        Serial.println("[SETUP] Ilk OpenSky poll yapiliyor...");
        Serial.flush();
        if (netFetchAircraft()) {
            s_firstPollDone = true;
            Serial.println("[SETUP] Ilk poll BASARILI");
        } else {
            Serial.println("[SETUP] Ilk poll BASARISIZ");
        }
    } else {
        Serial.println("[SETUP] WiFi yok, poll atlandi");
    }
    Serial.flush();

    // ─── İlk ekran ───
    Serial.println("[SETUP] Ilk render...");
    Serial.flush();
    if (s_firstPollDone) {
        renderDrawAll();
    } else {
        renderDrawOffline();
    }
    Serial.println("[SETUP] TAMAMLANDI - loop basliyor");
    Serial.println("==================================");

    s_lastPoll = millis();
    s_lastRender = millis();

    // Çekirdek 0 üzerinde arka plan fetch görevini başlat
    xTaskCreatePinnedToCore(
        netFetchTask,
        "NetFetchTask",
        16384, // WiFiClientSecure için büyük yığın belleği (Stack) gerekiyor!
        NULL,
        1,    // Düşük öncelik
        &g_fetchTaskHandle,
        0     // Core 0 (Ağ işlemleri)
    );
}

// ══════════════════════════════════════════════════════════════
//  loop — Ana döngü
//  1. WiFi maintain
//  2. Joystick + Buton girişleri
//  3. Render (zamanlı — akıcı joystick için 50ms)
//  4. OLED güncelle
// ══════════════════════════════════════════════════════════════
void loop() {
    uint32_t now = millis();

    // ─── 1. WiFi maintain ───
    netMaintain();

    // ─── 2. Joystick + Buton girişleri ───
    ctrlHandleJoystick();
    ctrlHandleButtons();

    // ─── 4. Render (50ms = 20 FPS) ───
    if (now - s_lastRender > RENDER_INTERVAL_MS) {
        s_lastRender = now;
        if (s_firstPollDone) {
            renderDrawAll();
        } else {
            renderDrawOffline();
        }
    }

    // ─── 5. OLED güncelle (her 800ms) — her zaman genel durum ───
    static uint32_t lastOLED = 0;
    if (now - lastOLED > 800) {
        lastOLED = now;
        renderOLEDSummary();
    }

    // Frame limiting
    delay(10);
}
