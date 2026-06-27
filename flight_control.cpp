// ============================================================
//  E-OS FLIGHT TRACKER — Kontrol (Joystick + Butonlar)
//
//  Kontroller:
//    Joystick ^v<> : Harita gezme (pan)
//    BTN_A : Zoom in (+)
//    BTN_B : Zoom out (-)
//    BTN_C : En yakın uçağı seç / seçimi kaldır
//    Joystick SW : Reset viewport (varsayılan Türkiye)
//
//  NVS'te son viewport kaydedilir — cihaz açılınca aynı yerde başlasın
// ============================================================

#include "flight_internal.h"

static uint32_t s_lastPanMove = 0;
static bool     s_swPressed = false;

// ══════════════════════════════════════════════════════════════
//  ctrlLoadView — NVS'ten viewport + ses oku
// ══════════════════════════════════════════════════════════════
void ctrlLoadView() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, true);
    g_view.center_lat = prefs.getFloat(NVS_KEY_LAT, DEFAULT_CENTER_LAT);
    g_view.center_lon = prefs.getFloat(NVS_KEY_LON, DEFAULT_CENTER_LON);
    g_view.zoom       = prefs.getInt(NVS_KEY_ZOOM, DEFAULT_ZOOM);
    prefs.end();


    // Sınır kontrolü
    if (g_view.zoom < 1) g_view.zoom = 1;
    if (g_view.zoom > 10) g_view.zoom = 10;
    if (g_view.center_lat < -85) g_view.center_lat = -85;
    if (g_view.center_lat > 85) g_view.center_lat = 85;
    if (g_view.center_lon < -180) g_view.center_lon = -180;
    if (g_view.center_lon > 180) g_view.center_lon = 180;
}

// ══════════════════════════════════════════════════════════════
//  ctrlSaveView — NVS'e viewport kaydet
// ══════════════════════════════════════════════════════════════
void ctrlSaveView() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putFloat(NVS_KEY_LAT, g_view.center_lat);
    prefs.putFloat(NVS_KEY_LON, g_view.center_lon);
    prefs.putInt(NVS_KEY_ZOOM, g_view.zoom);
    // Ses (sound_en) ayarı Launcher'a ait olduğu için burada üzerine yazmıyoruz
    prefs.end();
}

// ══════════════════════════════════════════════════════════════
//  ctrlHandleJoystick — Harita gezme (pan)
//  Joystick X/Y → enlem/boylam merkezini kaydır
//  Debounce: 100ms (akıcı ama çok hızlı değil)
//
//  Pan hızı zoom'a göre ayarlanır — yüksek zoom'da daha küçük adım
// ══════════════════════════════════════════════════════════════
void ctrlHandleJoystick() {
    uint32_t now = millis();
    if (now - s_lastPanMove < 100) return;  // debounce

    int jx = analogRead(JOY_X) - g_joyCenterX;
    int jy = analogRead(JOY_Y) - g_joyCenterY;

    bool moved = false;
    // Pan adımı — zoom'a göre
    // zoom 1: 30°/adım, zoom 10: 0.05°/adım
    float panStep = 30.0f / powf(2.0f, g_view.zoom - 1);
    if (panStep < 0.01f) panStep = 0.01f;

    // X ekseni → boylam (sağ=doğu artı, sol=batı eksi)
    if (abs(jx) > 500) {
        g_view.center_lon += (jx > 0 ? panStep : -panStep);
        // Sarmala -180..180
        if (g_view.center_lon > 180) g_view.center_lon -= 360;
        if (g_view.center_lon < -180) g_view.center_lon += 360;
        moved = true;
    }

    // Y ekseni → enlem (yukarı=kuzey artı, aşağı=güney eksi)
    // TFT'de Y yukarı eksi → analogRead yukarı az değeri
    // Ama joystick merkezi çıkarınca: yukarı → jy negatif
    if (abs(jy) > 500) {
        g_view.center_lat += (jy < 0 ? panStep : -panStep);
        if (g_view.center_lat > 85) g_view.center_lat = 85;
        if (g_view.center_lat < -85) g_view.center_lat = -85;
        moved = true;
    }

    if (moved) {
        s_lastPanMove = now;
        if (g_soundEnabled) tone(BUZZER, 600, 15);
    }

    // Joystick SW (bas) — viewport reset
    if (digitalRead(JOY_SW) == LOW && !s_swPressed) {
        s_swPressed = true;
        g_view.center_lat = DEFAULT_CENTER_LAT;
        g_view.center_lon = DEFAULT_CENTER_LON;
        g_view.zoom = DEFAULT_ZOOM;
        g_selectedIdx = -1;
        if (g_soundEnabled) tone(BUZZER, 800, 40);
        ctrlSaveView();
    }
    if (digitalRead(JOY_SW) == HIGH) s_swPressed = false;
}

// ══════════════════════════════════════════════════════════════
//  ctrlHandleButtons — Buton girişleri
// ══════════════════════════════════════════════════════════════
void ctrlHandleButtons() {
    // BTN_A: Zoom in
    if (!digitalRead(BTN_A)) {
        delay(50);
        if (!digitalRead(BTN_A)) {
            if (g_view.zoom < 10) {
                g_view.zoom++;
                if (g_soundEnabled) tone(BUZZER, 1200, 30);
            }
            ctrlSaveView();
            while (!digitalRead(BTN_A)) delay(30);
        }
    }

    // BTN_B: Zoom out
    if (!digitalRead(BTN_B)) {
        delay(50);
        if (!digitalRead(BTN_B)) {
            if (g_view.zoom > 1) {
                g_view.zoom--;
                if (g_soundEnabled) tone(BUZZER, 900, 30);
            }
            ctrlSaveView();
            while (!digitalRead(BTN_B)) delay(30);
        }
    }

    // BTN_C: En yakın uçağı seç / seçimi kaldır
    if (!digitalRead(BTN_C)) {
        delay(50);
        if (!digitalRead(BTN_C)) {
            if (g_selectedIdx >= 0) {
                // Zaten seçili → kaldır
                g_selectedIdx = -1;
                if (g_soundEnabled) tone(BUZZER, 400, 30);
            } else {
                // En yakını seç
                ctrlSelectNearest();
                if (g_selectedIdx >= 0 && g_soundEnabled) {
                    tone(BUZZER, 1500, 40);
                }
            }
            while (!digitalRead(BTN_C)) delay(30);
        }
    }

}

// ══════════════════════════════════════════════════════════════
//  ctrlSelectNearest — Ekranda ekran merkezine en yakın uçağı seç
// ══════════════════════════════════════════════════════════════
void ctrlSelectNearest() {
    int centerX = SCR_W / 2;
    int centerY = SCR_H / 2;
    int bestIdx = -1;
    int bestDist = 99999;

    for (int i = 0; i < g_aircraftCount; i++) {
        if (!g_aircraft[i].on_screen) continue;
        int dx = g_aircraft[i].screen_x - centerX;
        int dy = g_aircraft[i].screen_y - centerY;
        int dist = dx * dx + dy * dy;
        if (dist < bestDist) {
            bestDist = dist;
            bestIdx = i;
        }
    }

    g_selectedIdx = bestIdx;
}
