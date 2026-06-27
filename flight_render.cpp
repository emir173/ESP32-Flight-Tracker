// ============================================================
//  E-OS FLIGHT TRACKER — UI Render (TFT + OLED)
//
//  TFT 160x128:
//    - Harita arka planı + grid + kıtalar
//    - Uçak noktaları (heading yönünde ok şeklinde)
//    - Status bar (FLIGHT TRACKER + bağlantı + uçak sayısı)
//    - Seçili uçak vurgusu + bilgi kutusu
//
//  OLED 128x64:
//    - Toplam uçak sayısı + son güncelleme
//    - Seçili uçak detayı (callsign, irtifa, hız, heading, ülke)
// ============================================================

#include "flight_internal.h"
#include "flight_plane_frames.h"

// ─── Uçak sprite sistemi (Python ile ön-render edildi) ───
// Döndürme artifact'i YOK — her yön keskin pixel-art.
#define PLANE_SIZE   15

// (buildPlaneSprites fonksiyonu silindi çünkü doğrudan header'dan çiziyoruz)

// ─── Render state ───
static bool s_oledReady = false;
static bool s_dashboardDrawn = false;
// Önceki viewport — değişmediyse haritayı yeniden çizme
static float s_prevCenterLat = 999;
static float s_prevCenterLon = 999;
static int   s_prevZoom = -1;
static int   s_prevSelected = -2;  // seçili uçak değişimi takip
static int   s_prevAircraftCount = -1;

// ══════════════════════════════════════════════════════════════
//  interpolateAircraft — Uçağın mevcut pozisyonunu heading+hız'dan
//  ekstrapole et. OpenSky verisi 120 sn'de bir gelir, ama biz her
//  render'da (100ms) uçağın nereye gittiğini hesaplayıp ekranda
//  kaydırarak "canlı" hareket hissi veririz.
//
//  Mantık:
//    delta_t = now - last_update (saniye)
//    distance = velocity * delta_t (metre)
//    Dünya yarıçapı 6371000m, 1 derece ~111km
//    lat_offset = distance * cos(heading) / 111000
//    lon_offset = distance * sin(heading) / (111000 * cos(lat))
//    interp_lat = lat + lat_offset
//    interp_lon = lon + lon_offset
// ══════════════════════════════════════════════════════════════
static void interpolateAircraft(Aircraft &a) {
    if (a.on_ground || a.velocity < 1.0f) {
        // Yerde veya çok yavaş — interpolasyon yapma
        a.interp_lat = a.lat;
        a.interp_lon = a.lon;
        return;
    }

    uint32_t now = millis();
    float deltaSec = (now - a.last_update_ms) / 1000.0f;
    if (deltaSec > 600) deltaSec = 600;  // 10 dk上限 — çok eski veride uçuş kaybolmuş olabilir

    // Dünya üzerinde hareket — metre → derece
    float distance = a.velocity * deltaSec;  // metre
    float headingRad = a.heading * PI / 180.0f;

    // Kuzey ekseni: heading 0 = kuzey, 90 = doğu
    // lat değişimi: cos(heading) * distance
    // lon değişimi: sin(heading) * distance
    float latDegPerM = 1.0f / 111000.0f;  // 1 derece enlem ~111km
    float lonDegPerM = 1.0f / (111000.0f * cosf(a.lat * PI / 180.0f));

    a.interp_lat = a.lat + cosf(headingRad) * distance * latDegPerM;
    a.interp_lon = a.lon + sinf(headingRad) * distance * lonDegPerM;

    // Sınır kontrolü
    if (a.interp_lat > 90) a.interp_lat = 90;
    if (a.interp_lat < -90) a.interp_lat = -90;
    if (a.interp_lon > 180) a.interp_lon -= 360;
    if (a.interp_lon < -180) a.interp_lon += 360;
}

// ══════════════════════════════════════════════════════════════
//  renderSetup — OLED başlat
// ══════════════════════════════════════════════════════════════
void renderSetup() {
    oled.begin();
    renderMarkOLEDReady();
}

// ══════════════════════════════════════════════════════════════
//  renderDrawStatusBar — Üst durum çubuğu
//  Sol: ▶ FLIGHT TRACKER  Sağ: uçak sayısı + bağlantı noktası
// ══════════════════════════════════════════════════════════════
void renderDrawStatusBar() {
    spr.fillRect(0, 0, 160, 12, RGB_FIX(12, 12, 28));

    // Toplam Genişlik: Üçgen (4px) + Boşluk (4px) + "FLIGHT TRACKER" (84px) = 92px
    // 160 - 92 = 68 -> X başlangıç = 34
    
    // Play üçgeni (Ortalanmış grubun solu)
    spr.fillTriangle(34, 3, 34, 9, 38, 6, COL_ACCENT);
    
    // FLIGHT TRACKER yazısı
    spr.setTextSize(1);
    spr.setTextColor(COL_WHITE, RGB_FIX(12, 12, 28));
    spr.setCursor(42, 2);
    spr.print("FLIGHT TRACKER");

    // UPDATING (Yenilenme) göstergesi — en sağda
    static uint32_t lastBlink = 0;
    static bool blinkOn = false;
    
    if (g_polling) {
        // Yenilenirken yanıp sönen kırmızı/turuncu ışık
        if (millis() - lastBlink > 300) {
            lastBlink = millis();
            blinkOn = !blinkOn;
        }
        if (blinkOn) {
            spr.fillRect(150, 3, 4, 6, COL_DANGER);
        } else {
            spr.fillRect(150, 3, 4, 6, RGB_FIX(50, 0, 0));
        }
    } else {
        // Yenilenmiyorken uyku rengi (gri/lacivert)
        spr.fillRect(150, 3, 4, 6, RGB_FIX(30, 40, 50));
    }

    // Accent alt çizgi
    spr.drawFastHLine(0, 12, 160, RGB_FIX(30, 70, 100));
    spr.drawFastHLine(40, 12, 80, COL_ACCENT);
}

// ══════════════════════════════════════════════════════════════
//  renderDrawAircraft — Tek uçağı ekranda çiz
//  Yolcu uçağı silüeti — sivri burun, sweep kanatlar, entegre kuyruk
//  Renk: jet (yüksek irtifa) → sarı, pervaneli (düşük) → yeşil
//         yerde → gri, seçili → kırmızı
// ══════════════════════════════════════════════════════════════
void renderDrawAircraft(const Aircraft &a, bool selected) {
    if (!a.on_screen) return;

    int cx = a.screen_x;
    int cy = a.screen_y;
    if (cx < 0 || cx >= SCR_W || cy < 0 || cy >= SCR_H) return;

    // %100 Orijinal FlightRadar24 Logosu (Yüksek Çözünürlüklü ve Kenar Yumuşatmalı)
    int angle = (int)round(a.heading);
    while (angle < 0) angle += 360;
    while (angle >= 360) angle -= 360;
    
    int frameIdx = angle / 5; // 5 derecelik adımlarla 72 kare (frame)
    if (frameIdx >= 72) frameIdx = 0;

    int w = 15;
    int h = 15;
    int startX = cx - w / 2;
    int startY = cy - h / 2;

    // İrtifaya göre renk seçimi (OpenSky tarzı)
    int colorIdx = 0;
    float altFt = a.altitude * 3.281f; // metre -> feet
    if (altFt >= 35000.0f) colorIdx = 3;      // Mor (> 35k ft)
    else if (altFt >= 25000.0f) colorIdx = 2; // Mavi (25k - 35k ft)
    else if (altFt >= 15000.0f) colorIdx = 1; // Yeşil (15k - 25k ft)
    // else 0 (Sarı, < 15k ft)

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int sx = startX + x;
            int sy = startY + y;
            
            if (sx < 0 || sx >= SCR_W || sy < 0 || sy >= SCR_H) continue;
            
            uint8_t alpha = PLANE_FRAMES_ALPHA[frameIdx][y * w + x];
            if (alpha > 0) {
                uint16_t fgColor = PLANE_FRAMES_RGB[colorIdx][frameIdx][y * w + x];
                if (alpha == 255) {
                    spr.drawPixel(sx, sy, fgColor);
                } else {
                    // Kenar yumuşatma (Anti-Aliasing) için gerçek zamanlı Alpha Blending
                    uint16_t bgColor = spr.readPixel(sx, sy);
                    
                    uint8_t r1 = (bgColor >> 11) & 0x1F;
                    uint8_t g1 = (bgColor >> 5) & 0x3F;
                    uint8_t b1 = bgColor & 0x1F;
                    
                    uint8_t r2 = (fgColor >> 11) & 0x1F;
                    uint8_t g2 = (fgColor >> 5) & 0x3F;
                    uint8_t b2 = fgColor & 0x1F;
                    
                    uint8_t r = (r1 * (255 - alpha) + r2 * alpha) / 255;
                    uint8_t g = (g1 * (255 - alpha) + g2 * alpha) / 255;
                    uint8_t b = (b1 * (255 - alpha) + b2 * alpha) / 255;
                    
                    spr.drawPixel(sx, sy, (r << 11) | (g << 5) | b);
                }
            }
        }
    }

    // Seçili uçak için halka (kanat uçlarının dışında)
    if (selected) {
        spr.drawCircle(cx, cy, 9, COL_SELECTED);
    }
}

// ══════════════════════════════════════════════════════════════
//  renderDrawSelectionBox — Seçili uçak için bilgi kutusu
//  Ekranın altında 3 satır: callsign, irtifa, hız
// ══════════════════════════════════════════════════════════════
void renderDrawSelectionBox() {
    if (g_selectedIdx < 0 || g_selectedIdx >= g_aircraftCount) return;

    const Aircraft &a = g_aircraft[g_selectedIdx];

    // Alt bilgi kutusu — y=95'ten başla
    int boxY = 95;
    int boxH = 33;  // 95-127

    // Arka plan
    spr.fillRect(0, boxY, 160, boxH, RGB_FIX(15, 15, 35));
    // Üst ayraç
    spr.drawFastHLine(0, boxY, 160, COL_ACCENT);

    spr.setTextSize(1);
    
    // Satır 1 (Y=97): Callsign (Sol) | Ülke (Sağ)
    spr.setTextColor(COL_ACCENT, RGB_FIX(15, 15, 35));
    spr.setCursor(4, boxY + 2);
    spr.print(a.callsign[0] ? a.callsign : "UNKNOWN");
    
    // Ülke adını kırp (maks 12 karakter sığar)
    char cBuf[13];
    strncpy(cBuf, a.country, 12);
    cBuf[12] = '\0';
    spr.setTextColor(COL_JET, RGB_FIX(15, 15, 35)); // Uçakla aynı renk
    spr.setCursor(84, boxY + 2);
    spr.print(cBuf);

    // Satır 2 (Y=107): İrtifa (Sol) | Hız (Sağ)
    spr.setTextColor(COL_WHITE, RGB_FIX(15, 15, 35));
    int altFt = (int)(a.altitude * 3.281f);
    char altBuf[20];
    snprintf(altBuf, sizeof(altBuf), "ALT: %dft", altFt);
    spr.setCursor(4, boxY + 12);
    spr.print(altBuf);

    int spdKmh = (int)(a.velocity * 3.6f);
    char spdBuf[20];
    if (a.on_ground) {
        snprintf(spdBuf, sizeof(spdBuf), "YERDE");
    } else {
        snprintf(spdBuf, sizeof(spdBuf), "SPD: %d kmh", spdKmh);
    }
    spr.setCursor(84, boxY + 12);
    spr.print(spdBuf);

    // Satır 3 (Y=117): Heading (Sol) | ICAO Kodu (Sağ)
    spr.setTextColor(COL_GRAY, RGB_FIX(15, 15, 35));
    char hdgBuf[20];
    snprintf(hdgBuf, sizeof(hdgBuf), "HDG: %d", (int)a.heading);
    spr.setCursor(4, boxY + 22);
    spr.print(hdgBuf);

    char icaoBuf[20];
    snprintf(icaoBuf, sizeof(icaoBuf), "ICAO: %s", a.icao24);
    spr.setCursor(84, boxY + 22);
    spr.print(icaoBuf);
}

// ══════════════════════════════════════════════════════════════
//  renderDrawAll — Ana ekran çizimi (optimize + interpolation)
//  - Uçaklar interpolate edildiği için her render'da çizim yapılır
//  - Ama harita sadece viewport değişince yeniden çizilir
//  - Uçaklar haritanın üzerine çizilir (harita cached kabul edilir)
// ══════════════════════════════════════════════════════════════
void renderDrawAll() {
    bool viewportChanged = (s_prevCenterLat != g_view.center_lat ||
                            s_prevCenterLon != g_view.center_lon ||
                            s_prevZoom != g_view.zoom);

    // Harita sadece viewport değişince çiz (ağır işlem)
    // Yalnızca grid viewport değişince güncelleniyordu ama şimdilik her karede arka planı sıfırlıyoruz.
    if (viewportChanged || !s_dashboardDrawn) {
        s_prevCenterLat = g_view.center_lat;
        s_prevCenterLon = g_view.center_lon;
        s_prevZoom = g_view.zoom;
        s_dashboardDrawn = true;
    }

    // ─── Uçakları her render'da çiz (interpolated) ───
    // Önce eski uçakları sil — haritayı yeniden çizmek yerine
    // sadece uçakların bulunduğu bölgeyi temizle
    // Ama harita kompleks olduğu için en güvenli yol: haritayı yeniden çiz
    // Performance: mapDrawBackground ~50ms, uçuşlar ~5ms
    // Yani her render ~60ms = 16 FPS — kabul edilebilir
    mapDrawBackground();
    mapDrawGrid();

    if (g_aircraftMutex != NULL) {
        xSemaphoreTake(g_aircraftMutex, portMAX_DELAY);
    }

    for (int i = 0; i < g_aircraftCount; i++) {
        Aircraft &a = g_aircraft[i];
        interpolateAircraft(a);
        latLonToScreen(a.interp_lat, a.interp_lon, a.screen_x, a.screen_y);
        a.on_screen = (a.screen_x >= 0 && a.screen_x < SCR_W &&
                       a.screen_y >= 0 && a.screen_y < SCR_H);
        
        // Önce seçili olmayanları çiz
        if (i != g_selectedIdx) {
            renderDrawAircraft(a, false);
        }
    }

    // Seçili uçağı en üstte (en son) çiz
    if (g_selectedIdx >= 0 && g_selectedIdx < g_aircraftCount) {
        renderDrawAircraft(g_aircraft[g_selectedIdx], true);
    }

    renderDrawStatusBar();
    renderDrawSelectionBox();
    
    s_prevAircraftCount = g_aircraftCount;
    s_prevSelected = g_selectedIdx;

    if (g_aircraftMutex != NULL) {
        xSemaphoreGive(g_aircraftMutex);
    }

    // Ekrana bas!
    spr.pushSprite(0, 0);
}

// ══════════════════════════════════════════════════════════════
//  renderDrawConnecting — "Bağlanıyor..." ekranı
// ══════════════════════════════════════════════════════════════
void renderDrawConnecting() {
    spr.fillSprite(COL_BG);
    spr.setTextSize(1);
    spr.setTextColor(COL_ACCENT, COL_BG);
    const char* msg = "WiFi Baglaniyor...";
    int w = strlen(msg) * 6;
    spr.setCursor((160 - w) / 2, 50);
    spr.print(msg);

    spr.setTextColor(COL_GRAY, COL_BG);
    int ssidW = strlen(WIFI_SSID) * 6;
    int ssidX = (160 - ssidW) / 2;
    spr.setCursor(ssidX > 0 ? ssidX : 0, 66);
    spr.print(WIFI_SSID);

    // Animasyonlu noktalar
    static uint8_t dots = 0;
    dots = (dots + 1) % 4;
    spr.setTextColor(COL_WHITE, COL_BG);
    spr.setCursor(75, 82);
    for (int i = 0; i < 3; i++) {
        spr.print(i < dots ? "." : " ");
    }
    
    spr.pushSprite(0, 0);
}

// ══════════════════════════════════════════════════════════════
//  renderDrawOffline — Çevrimdışı mod
// ══════════════════════════════════════════════════════════════
void renderDrawOffline() {
    spr.fillSprite(COL_BG);
    renderDrawStatusBar();

    spr.setTextSize(2);
    spr.setTextColor(COL_DANGER, COL_BG);
    const char* offTxt = "OFFLINE";
    int offW = strlen(offTxt) * 12;
    spr.setCursor((160 - offW) / 2, 25);
    spr.print(offTxt);

    spr.setTextSize(1);
    spr.setTextColor(COL_GRAY, COL_BG);
    const char* msg1 = "OpenSky ulasilamiyor";
    int msg1W = strlen(msg1) * 6;
    spr.setCursor((160 - msg1W) / 2, 50);
    spr.print(msg1);

    const char* msg2 = "Baglanti bekleniyor...";
    int msg2W = strlen(msg2) * 6;
    spr.setCursor((160 - msg2W) / 2, 62);
    spr.print(msg2);

    if (g_lastUpdate_ms > 0) {
        uint32_t ago = (millis() - g_lastUpdate_ms) / 1000;
        char buf[32];
        snprintf(buf, sizeof(buf), "Son veri: %lu sn once", (unsigned long)ago);
        int bw = strlen(buf) * 6;
        spr.setTextColor(COL_DARK_GRAY, COL_BG);
        spr.setCursor((160 - bw) / 2, 78);
        spr.print(buf);
    } else {
        const char* nv = "Son veri yok";
        int nvW = strlen(nv) * 6;
        spr.setTextColor(COL_DARK_GRAY, COL_BG);
        spr.setCursor((160 - nvW) / 2, 78);
        spr.print(nv);
    }

    const char* retry = "Otomatik yeniden deneme";
    int retryW = strlen(retry) * 6;
    spr.setTextColor(COL_DARK_GRAY, COL_BG);
    spr.setCursor((160 - retryW) / 2, 105);
    spr.print(retry);

    const char* exitTxt = "D: Cikis";
    int exitW = strlen(exitTxt) * 6;
    spr.setTextColor(COL_DARK_GRAY, COL_BG);
    spr.setCursor((160 - exitW) / 2, 118);
    spr.print(exitTxt);
    
    spr.pushSprite(0, 0);
}

// ══════════════════════════════════════════════════════════════
//  renderOLEDInfo — artık renderOLEDSummary ile aynı
//  Uçak seçilse bile OLED genel durum gösterir (TFT'de zaten detay var)
// ══════════════════════════════════════════════════════════════
void renderOLEDInfo() {
    renderOLEDSummary();
}

// ══════════════════════════════════════════════════════════════
//  renderOLEDSummary — OLED'de genel durum (her zaman)
//  1. Satır: Saat (HH:MM:SS) + bağlantı rozeti
//  2. Satır: Toplam uçak sayısı
//  3. Satır: Zoom seviyesi
//  4. Satır: Merkez koordinat
//  5. Satır: Son güncelleme süresi
// ══════════════════════════════════════════════════════════════
void renderOLEDSummary() {
    if (!s_oledReady) return;

    oled.setPowerSave(0);
    oled.clearBuffer();
    oled.setFont(u8g2_font_6x10_tr);

    // Satır 1 — Gerçek saat (NTP) + bağlantı rozeti
    char timeBuf[24];
    netGetTime(timeBuf, sizeof(timeBuf));
    oled.drawStr(0, 9, timeBuf);

    // Bağlantı rozeti — sağ üst
    if (netIsConnected()) oled.drawBox(118, 2, 6, 6);
    else                  oled.drawFrame(118, 2, 6, 6);

    // Satır 2 — Toplam uçak
    char countBuf[24];
    snprintf(countBuf, sizeof(countBuf), "Aircraft: %u", g_aircraftCount);
    oled.drawStr(0, 22, countBuf);

    // Satır 3 — Zoom
    char zoomBuf[24];
    snprintf(zoomBuf, sizeof(zoomBuf), "Zoom: %d", g_view.zoom);
    oled.drawStr(0, 35, zoomBuf);

    // Satır 4 — Merkez koordinat
    char centerBuf[24];
    snprintf(centerBuf, sizeof(centerBuf), "%.2f,%.2f", g_view.center_lat, g_view.center_lon);
    oled.drawStr(0, 48, centerBuf);

    // Satır 5 — Son güncelleme
    char updBuf[24];
    if (g_lastUpdate_ms > 0) {
        uint32_t ago = (millis() - g_lastUpdate_ms) / 1000;
        snprintf(updBuf, sizeof(updBuf), "Update: %lu sn", (unsigned long)ago);
    } else {
        strcpy(updBuf, "Update: yok");
    }
    oled.drawStr(0, 61, updBuf);

    oled.sendBuffer();
}

void renderOLEDOff() {
    if (s_oledReady) oled.setPowerSave(1);
}

void renderMarkOLEDReady() {
    s_oledReady = true;
}
