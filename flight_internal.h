#pragma once
// ============================================================
//  E-OS FLIGHT TRACKER — Paylaşılan State (Internal Header)
//
//  Tüm .cpp dosyaları bu header'ı include eder.
//  Global değişkenler flight.ino içinde tanımlanır,
//  burada sadece extern bildirimleri ve veri yapıları var.
// ============================================================

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <TFT_eSPI.h>
#include <U8g2lib.h>
#include <Preferences.h>
#include "flight_config.h"
#include "hardware_config.h"

// ─── RGB_FIX (launcher ile aynı BGR düzeltmesi) ───
extern TFT_eSPI tft;
extern TFT_eSprite spr;
extern uint16_t* g_psramMap;          // regional (2048x1024, Türkiye)
extern uint16_t* g_psramMapWorld;     // world (512x256, tüm dünya düşük)
extern U8G2_SH1106_128X64_NONAME_F_HW_I2C oled;

inline uint16_t RGB_FIX(uint8_t r, uint8_t g, uint8_t b) {
    return tft.color565(b, g, r);  // BGR swap
}

// ══════════════════════════════════════════════════════════════
//  VERİ YAPILARI
// ══════════════════════════════════════════════════════════════

// Tek bir uçağın verisi — OpenSky states dizisinden çıkarılır
// OpenSky states[i] sırası:
//   [0] icao24, [1] callsign, [2] origin_country,
//   [5] longitude, [6] latitude, [7] baro_altitude,
//   [8] on_ground, [9] velocity, [10] true_track, [13] geo_altitude
struct Aircraft {
    char     callsign[10];   // "THY3BE " → trim'lenmiş "THY3BE"
    char     icao24[8];      // "4bb245" — transponder adresi
    char     country[16];    // "Turkey" — uçuş kökeni
    float    lon;            // Boylam (-180..180) — OpenSky'dan alınan
    float    lat;            // Enlem (-90..90) — OpenSky'dan alınan
    float    altitude;       // İrtifa (metre)
    float    velocity;       // Hız (m/s)
    float    heading;        // Yön (0-359°, kuzey=0)
    bool     on_ground;      // Yerde mi?
    int      screen_x;       // Ekrandaki X piksel (render sırasında hesaplanır)
    int      screen_y;       // Ekrandaki Y piksel
    bool     on_screen;      // Ekranda görünür mü?
    // ─── Interpolation için ───
    float    interp_lat;     // Interpolated enlem (render'da hesaplanır)
    float    interp_lon;     // Interpolated boylam
    uint32_t last_update_ms; // Bu uçağın son OpenSky veri zamanı (millis)
};

// Harita viewport durumu — kullanıcı joystick ile gezer
struct Viewport {
    float center_lat;        // Merkez enlem
    float center_lon;        // Merkez boylam
    int   zoom;              // Zoom seviyesi (1-10)
};

// ══════════════════════════════════════════════════════════════
//  GLOBAL STATE (flight.ino içinde tanımlı, extern burada)
// ══════════════════════════════════════════════════════════════

extern SemaphoreHandle_t g_aircraftMutex; // Mutex for dual-core safe access
extern Aircraft  g_aircraft[];     // Uçak dizisi (MAX_AIRCRAFT)
extern int       g_aircraftCount;  // Kaç uçak var
extern Viewport  g_view;           // Mevcut viewport
extern bool      g_soundEnabled;   // Ses açık mı (NVS)
extern int       g_selectedIdx;    // Seçili uçak index (-1 = hiçbiri)
extern int       g_joyCenterX;     // Joystick kalibrasyon
extern int       g_joyCenterY;
extern bool      g_dataValid;      // OpenSky'dan veri gelmiş mi
extern uint32_t  g_lastUpdate_ms;  // Son başarılı poll zamanı
extern bool      g_polling;        // Şu an poll yapılıyor mu (UPDATING göstergesi)

// ─── Renk paleti (flight_map.cpp setupColors()'ta atanır) ───
extern uint16_t COL_BG;
extern uint16_t COL_OCEAN;
extern uint16_t COL_LAND;
extern uint16_t COL_BORDER;
extern uint16_t COL_ACCENT;
extern uint16_t COL_WHITE;
extern uint16_t COL_GRAY;
extern uint16_t COL_DARK_GRAY;
extern uint16_t COL_BLACK;
extern uint16_t COL_JET;
extern uint16_t COL_PROP;
extern uint16_t COL_GROUND;
extern uint16_t COL_SELECTED;
extern uint16_t COL_DANGER;

// ══════════════════════════════════════════════════════════════
//  MODÜL API'LERİ
// ══════════════════════════════════════════════════════════════

// flight_net.cpp
bool netConnectWiFi();
bool netIsConnected();
void netMaintain();
bool netFetchAircraft();              // OpenSky'den uçakları çek + parse
void netGetTime(char *buf, int len);  // Gerçek saat "HH:MM:SS"
bool netIsNTPSynced();                // NTP senkronize mi

// flight_map.cpp
void mapSetupColors();
void mapDrawBackground();             // Okyanus + kıtalar (basit)
void mapDrawGrid();                   // Enlem/boylam ızgarası
void latLonToScreen(float lat, float lon, int &sx, int &sy);  // projeksiyon
bool screenToLatLon(int sx, int sy, float &lat, float &lon);  // ters
void mapGetViewBounds(float &latMin, float &latMax, float &lonMin, float &lonMax);

// flight_render.cpp
void renderSetup();
void renderDrawAll();                 // TFT: harita + uçaklar + UI
void renderDrawAircraft(const Aircraft &a, bool selected);
void renderDrawStatusBar();
void renderDrawSelectionBox();        // Seçili uçak için vurgu
void renderDrawConnecting();
void renderDrawOffline();
void renderOLEDInfo();                // OLED: seçili uçak detayı
void renderOLEDSummary();             // OLED: toplam uçak sayısı
void renderOLEDOff();
void renderMarkOLEDReady();

// flight_control.cpp
void ctrlLoadView();                  // NVS'ten viewport + ses oku
void ctrlSaveView();                  // NVS'e viewport kaydet
void ctrlHandleJoystick();            // Joystick ile harita gezme
void ctrlHandleButtons();             // BTN_A/B/C/D işle
void ctrlSelectNearest();             // Ekranda en yakın uçağı seç

// ─── Yardımcı: callsign trim (boşlukları temizle) ───
inline void trimCallsign(char *dst, const char *src, int len) {
    int j = 0;
    for (int i = 0; src[i] && j < len - 1; i++) {
        if (src[i] != ' ') dst[j++] = src[i];
    }
    dst[j] = '\0';
}
