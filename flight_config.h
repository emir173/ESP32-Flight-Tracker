#pragma once
// ============================================================
//  E-OS FLIGHT TRACKER — Yapılandırma Dosyası
//
//  OpenSky Network API'si kullanır — ücretsiz, API key gerekmez.
//  Gerçek uçak verisi çeker: callsign, enlem/boylam, irtifa, hız, heading.
//
//  KÜTÜPHANE GEREKSİNİMİ:
//    ArduinoJson (Benoit Blanchon) v7.x — monitor projesiyle aynı
//
//  OPENSKY API RATE LIMIT:
//    - Anahtarsız: 100 istek/gün (10 sn'de bir önerilir)
//    - Anahtarla (ücretsiz kayıt): 4000 istek/gün
//    https://opensky-network.org/index.php?option=com_users&view=registration
//
//  ÇALIŞTIRMA:
//    1. WiFi bilgilerini doldur (monitor_config.h ile aynı ağ)
//    2. flight.ino'yu derle → flight.bin → SD karta at
//    3. Launcher'dan FLIGHT'ı seç (eklenecek)
// ============================================================

// ─── WiFi Yapılandırması ───
#define WIFI_SSID       "YOUR_WIFI_SSID"
#define WIFI_PASSWORD   "YOUR_WIFI_PASSWORD"
#define WIFI_TIMEOUT_MS 15000
#define WIFI_RETRY_MS   3000

// ─── OpenSky Network API ───
// Ücretsiz endpoint — anahtar gerekmez
#define OPENSKY_HOST     "opensky-network.org"
#define OPENSKY_PORT     443
#define OPENSKY_PATH     "/api/states/all"
// HTTPS kullanıyoruz (port 443) — SSL gereklidir
// OpenSky hesabı (4000 kredi/gün) — Basic Auth
// clientId = kullanıcı adı, clientSecret = şifre
#define OPENSKY_USER     "YOUR_OPENSKY_USERNAME"
#define OPENSKY_PASS     "YOUR_OPENSKY_PASSWORD"

// ─── Polling Aralığı ───
// OpenSky'den veri çekme sıklığı (ms)
// Hesaplı (4000 kredi/gün): 120 sn = 720/gün (çok güvenli)
// OpenSky saatlik limit de var (~400/saat), 120 sn = 30/saat güvenli
#define POLL_INTERVAL_MS 120000    // 2 dakika — güvenli
// Harita render aralığı (ms) — joystick ile gezme akıcı
// 50ms çok agresif → 100ms (10 FPS) yeterli, uçuş verisi yavaş değişir
#define RENDER_INTERVAL_MS 100

// ─── Varsayılan Harita Bölgesi ───
// Türkiye + Avrupa civarı (İstanbul merkezli)
// OpenSky bbox parametreleri: lamin, lomin, lamax, lomax (enlem/boylam)
// Türkiye: enlem 36-42, boylam 26-45
#define DEFAULT_CENTER_LAT  39.5f   // Türkiye merkezi enlem
#define DEFAULT_CENTER_LON  32.0f   // Türkiye merkezi boylam (daha dar)
#define DEFAULT_ZOOM        6       // 1=dünya, 10=şehir (6 Türkiye için dar)

// ─── Veri Bölgesi (API'den çekeceğimiz bbox) ───
// SABIT bölge — viewport'tan bağımsız, Türkiye + çevresi
// Kullanıcı zoom yapsa da aynı veriyi kullanır, ekranda görünen kısmı çizer
// Bu sayede zoom değişince uçak sayısı değişmez
#define DATA_BBOX_LAT_MIN  33.0f   // Türkiye güneyi + Doğu Akdeniz
#define DATA_BBOX_LAT_MAX  44.0f   // Türkiye kuzeyi + Karadeniz
#define DATA_BBOX_LON_MIN  23.0f   // Türkiye batısı + Ege
#define DATA_BBOX_LON_MAX  47.0f   // Türkiye doğusu + Kafkasya
#define MAX_AIRCRAFT       200     // 200 uçak (internal RAM yeter)

// ─── Ekran Sabitleri ───
#define SCR_W 160
#define SCR_H 128

// ─── Pinler (hardware_config.h'den gelir) ───
// TFT_CS, SD_CS, JOY_X, JOY_Y, JOY_SW, BTN_A..D, BUZZER, I2C_SDA/SCL

// ─── NVS Anahtarları ───
// Son harita konumu/zoom kaydedilir — cihaz açılınca aynı yerde başlasın
#define NVS_NAMESPACE     "flight"
#define NVS_KEY_LAT       "lat"
#define NVS_KEY_LON       "lon"
#define NVS_KEY_ZOOM      "zoom"
#define NVS_KEY_SOUND     "snd"

// ─── NTP (Gerçek Saat) ───
// WiFi bağlanınca NTP sunucusundan saat çekilir
// ESP32'de RTC yok, ama WiFi varken NTP ile senkronize eder
#define NTP_SERVER1     "pool.ntp.org"
#define NTP_SERVER2     "time.google.com"
#define NTP_TZ_OFFSET   3       // Türkiye UTC+3 (saat)
#define NTP_TZ_DST      0       // Yaz saati uygulaması yok (0)

// ─── Versiyon ───
#define FLIGHT_VERSION "v1.0"
