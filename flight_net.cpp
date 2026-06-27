// ============================================================
//  E-OS FLIGHT TRACKER — Ağ İstemcisi (WiFi + OpenSky HTTPS)
//
//  Sorumluluk:
//    - WiFi'ya bağlan / yeniden bağlan
//    - OpenSky Network API'sine HTTPS isteği at
//    - JSON'ı parse edip g_aircraft[] dizisini doldur
//
//  OpenSky API:
//    GET https://opensky-network.org/api/states/all?lamin=..&lomin=..&lamax=..&lomax=..
//    Yanıt: {"time":..., "states":[[icao,callsign,country,...,lon,lat,...], ...]}
//
//  HTTPS gerektirdiği için WiFiClientSecure kullanıyoruz.
//  SSL sertifika doğrulamasını devre dışı bırakıyoruz (setInsecure)
//  — cihazın RTC'si olmadığı için sertifika tarihi kontrol edilemiyor.
//  Veri hassasiyeti olmadığı için bu acceptable.
// ============================================================

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_heap_caps.h>
#include <time.h>
#include "flight_internal.h"

static bool s_wifiConnected = false;
static bool s_ntpSynced = false;
static uint32_t s_lastReconnectAttempt = 0;

// ─── Statik PSRAM buffer'ları — bir kez allocate, tekrar kullan ───
// Her poll'da malloc/free yapmak heap fragmentation'a → crash'e yol açar
static char *s_netBuf = nullptr;     // 500KB HTTP yanıt buffer'ı
static char *s_cleanBuf = nullptr;   // 500KB chunked temizlik buffer'ı
static bool s_buffersAllocated = false;

static bool ensureBuffers() {
    if (s_buffersAllocated) return true;
    s_netBuf = (char*)heap_caps_malloc(500000, MALLOC_CAP_SPIRAM);
    s_cleanBuf = (char*)heap_caps_malloc(500000, MALLOC_CAP_SPIRAM);
    if (!s_netBuf || !s_cleanBuf) {
        Serial.println("[NET] PSRAM buffer allocation BASARISIZ!");
        return false;
    }
    s_buffersAllocated = true;
    Serial.println("[NET] PSRAM buffer'lar allocate edildi (500KB x2)");
    return true;
}

// WiFi olay callback — bağlantı kesilince otomatik yeniden bağlantı işaretler
static void wifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            s_wifiConnected = false;
            break;
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            s_wifiConnected = true;
            break;
        default:
            break;
    }
}

// ══════════════════════════════════════════════════════════════
//  netConnectWiFi — WiFi'ya bağlan (blocking, timeout'lu)
// ══════════════════════════════════════════════════════════════
bool netConnectWiFi() {
    if (s_wifiConnected && WiFi.status() == WL_CONNECTED) return true;

    WiFi.mode(WIFI_STA);
    WiFi.onEvent(wifiEvent);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    uint32_t startMs = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - startMs > WIFI_TIMEOUT_MS) {
            s_wifiConnected = false;
            return false;
        }
        delay(100);
    }
    s_wifiConnected = true;
    Serial.print("[NET] WiFi baglandi. IP: ");
    Serial.println(WiFi.localIP());

    // NTP ile saat senkronize et
    configTime(NTP_TZ_OFFSET * 3600, NTP_TZ_DST * 3600, NTP_SERVER1, NTP_SERVER2);
    Serial.println("[NET] NTP senkronizasyon basladi...");
    return true;
}

// ══════════════════════════════════════════════════════════════
//  netGetTime — Gerçek saati "HH:MM:SS" formatında döndür
//  NTP senkronize değilse "00:00:00" döner
// ══════════════════════════════════════════════════════════════
void netGetTime(char *buf, int len) {
    time_t now = time(nullptr);
    if (now < 1700000000) {  // 2023'ten önce = senkronize değil
        s_ntpSynced = false;
        snprintf(buf, len, "--:--:--");
        return;
    }
    s_ntpSynced = true;
    struct tm *t = localtime(&now);
    snprintf(buf, len, "%02d:%02d:%02d", t->tm_hour, t->tm_min, t->tm_sec);
}

bool netIsNTPSynced() {
    return s_ntpSynced;
}

bool netIsConnected() {
    return s_wifiConnected && WiFi.status() == WL_CONNECTED;
}

// ══════════════════════════════════════════════════════════════
//  netMaintain — loop'ta çağrılır, bağlantı kopmuşsa yeniden dene
// ══════════════════════════════════════════════════════════════
void netMaintain() {
    if (s_wifiConnected && WiFi.status() == WL_CONNECTED) return;
    uint32_t now = millis();
    if (now - s_lastReconnectAttempt < WIFI_RETRY_MS) return;
    s_lastReconnectAttempt = now;
    s_wifiConnected = false;
    WiFi.disconnect(true, false);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 3000) {
        delay(50);
    }
    if (WiFi.status() == WL_CONNECTED) s_wifiConnected = true;
}

// ══════════════════════════════════════════════════════════════
//  netFetchAircraft — OpenSky'den uçakları çek + parse
//
//  Akış:
//    1. Mevcut viewport'tan bbox hesapla (görünen bölge + margin)
//    2. HTTPS GET isteği at: /api/states/all?lamin=..&lomin=..&lamax=..&lomax=..
//    3. JSON'ı parse et — states dizisindeki her uçağı Aircraft'e çevir
//    4. g_aircraft[] dizisini doldur, g_aircraftCount güncelle
//
//  Return: true = başarılı, false = hata
// ══════════════════════════════════════════════════════════════
bool netFetchAircraft() {
    if (!netIsConnected()) {
        Serial.println("[NET] WiFi yok, fetch atlandi");
        return false;
    }

    // ─── 1. Bbox — SABIT Türkiye bölgesi (viewport'tan bağımsız) ───
    // Kullanıcı zoom/pan yapsa da aynı veriyi çeker — tutarlı uçak sayısı
    float latMin = DATA_BBOX_LAT_MIN;
    float latMax = DATA_BBOX_LAT_MAX;
    float lonMin = DATA_BBOX_LON_MIN;
    float lonMax = DATA_BBOX_LON_MAX;

    // URL oluştur — bbox parametreleri
    char path[256];
    snprintf(path, sizeof(path),
             "%s?lamin=%.4f&lomin=%.4f&lamax=%.4f&lomax=%.4f",
             OPENSKY_PATH, latMin, lonMin, latMax, lonMax);

    Serial.print("[NET] OpenSky istek: ");
    Serial.println(path);

    // ─── 2. HTTPS GET — WiFiClientSecure ───
    WiFiClientSecure client;
    client.setInsecure();  // Sertifika doğrulaması kapalı (RTC yok)
    client.setTimeout(15000);

    HTTPClient http;
    http.setTimeout(15000);
    http.setConnectTimeout(10000);

    if (!http.begin(client, OPENSKY_HOST, OPENSKY_PORT, path)) {
        Serial.println("[NET] http.begin BASARISIZ");
        return false;
    }

    // Basic Auth (varsa — OpenSky hesabı olanlar için)
    if (strlen(OPENSKY_USER) > 0) {
        http.setAuthorization(OPENSKY_USER, OPENSKY_PASS);
    }

    int code = http.GET();
    Serial.print("[NET] HTTP kod: ");
    Serial.println(code);

    if (code == 429) {
        Serial.println("[NET] 429 - Rate limit! 5 dakika bekleniyor...");
        http.end();
        // 5 dakika bekle — limit sıfırlansın
        delay(300000);
        return false;
    }

    if (code != HTTP_CODE_OK) {
        Serial.print("[NET] HTTP hata: ");
        Serial.println(http.errorToString(code));
        http.end();
        return false;
    }

    // ─── 3. Yanıtı PSRAM'de buffer'a çek (stream'den parça parça) ───
    // 166KB yanıt — internal RAM'e sığmaz, PSRAM (8MB) kullan.
    // Statik buffer kullan — her poll'da malloc/free yapma (fragmentation → crash)
    if (!ensureBuffers()) {
        Serial.println("[NET] Buffer yok, poll atlandi");
        http.end();
        return false;
    }
    Serial.println("[NET] PSRAM buffer'a yukleniyor...");

    // Statik buffer'ı kullan
    size_t bufSize = 500000;
    char *buf = s_netBuf;

    // Stream'den parça parça oku
    WiFiClient *stream = http.getStreamPtr();
    size_t totalRead = 0;
    uint8_t tmp[2048];
    uint32_t timeout = millis() + 10000;  // 10 sn timeout
    while (http.connected() && totalRead < bufSize - 1 && millis() < timeout) {
        size_t avail = stream->available();
        if (avail == 0) {
            delay(1);
            continue;
        }
        size_t toRead = (avail < sizeof(tmp)) ? avail : sizeof(tmp);
        if (totalRead + toRead > bufSize - 1) toRead = bufSize - 1 - totalRead;
        size_t bytesRead = stream->readBytes((uint8_t*)(buf + totalRead), toRead);
        totalRead += bytesRead;
        if (bytesRead == 0) break;
    }
    buf[totalRead] = '\0';  // Null-terminate — kritik!
    http.end();

    Serial.print("[NET] Toplam okunan: ");
    Serial.print(totalRead);
    Serial.print(" byte, buf[0..40]: ");
    if (totalRead > 40) {
        char dbg[41];
        memcpy(dbg, buf, 40);
        dbg[40] = '\0';
        Serial.println(dbg);
    } else {
        Serial.println(buf);
    }

    // ─── Chunked encoding temizliği ───
    // Eğer buffer chunked ise başında "e68\r\n" gibi hex boyut + CRLF vardır.
    // HTTP/1.1 chunked: <hex size>\r\n<data>\r\n<hex size>\r\n<data>\r\n...0\r\n\r\n
    // Chunk header'larını ve CRLF'leri temizle, sadece data'yı birleştir.
    if (totalRead > 0 && (buf[0] == '\n' || buf[0] == '\r' ||
        (buf[0] >= '0' && buf[0] <= '9') || (buf[0] >= 'a' && buf[0] <= 'f'))) {
        // Chunked olabilir — ilk satır hex boyut mu kontrol et
        char *firstCRLF = strstr(buf, "\r\n");
        if (firstCRLF) {
            *firstCRLF = '\0';
            bool isHex = true;
            for (char *p = buf; *p; p++) {
                if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F'))) {
                    isHex = false;
                    break;
                }
            }
            *firstCRLF = '\r';  // geri al

            if (isHex) {
                // Chunked! Temizle — statik buffer kullan
                Serial.println("[NET] Chunked encoding tespit edildi, temizleniyor...");
                char *cleanBuf = s_cleanBuf;
                if (cleanBuf) {
                    size_t cleanLen = 0;
                    char *src = buf;
                    while (*src && cleanLen < totalRead - 1) {
                        // Hex boyut satırını oku
                        char *crlf = strstr(src, "\r\n");
                        if (!crlf) break;
                        *crlf = '\0';
                        long chunkSize = strtol(src, nullptr, 16);
                        *crlf = '\r';
                        if (chunkSize == 0) break;  // Son chunk
                        src = crlf + 2;  // CRLF sonrası data başı
                        // Data'yı kopyala
                        if (cleanLen + chunkSize > totalRead - 1) chunkSize = totalRead - 1 - cleanLen;
                        memcpy(cleanBuf + cleanLen, src, chunkSize);
                        cleanLen += chunkSize;
                        src += chunkSize;
                        // Data sonrası CRLF'yi atla
                        if (*src == '\r' && *(src + 1) == '\n') src += 2;
                    }
                    cleanBuf[cleanLen] = '\0';
                    // Temiz veriyi buf'a geri kopyala
                    memcpy(buf, cleanBuf, cleanLen + 1);
                    totalRead = cleanLen;
                    // cleanBuf statik — free etme
                    Serial.print("[NET] Chunked temizlendi, yeni boyut: ");
                    Serial.print(totalRead);
                    Serial.print(" byte, basi: ");
                    if (totalRead > 40) {
                        char dbg[41];
                        memcpy(dbg, buf, 40);
                        dbg[40] = '\0';
                        Serial.println(dbg);
                    }
                }
            }
        }
    }

    // ─── 4. Manuel JSON parser — ArduinoJson yok, çok daha az RAM ───
    // OpenSky formatı: {"time":...,"states":[["icao","callsign","country",...,lon,lat,...],...]}
    // Her uçağın states[i] array'ini manuel olarak split edip parse ederiz.
    // JsonDocument'in internal RAM kullanımını bypass eder.
    Serial.println("[NET] Manuel parser basliyor...");

    static Aircraft s_tempAircraft[MAX_AIRCRAFT];
    int count = 0;

    // "states":[ kısmını bul
    char *statesStart = strstr(buf, "\"states\":[");
    if (!statesStart) {
        Serial.println("[NET] 'states' dizisi bulunamadi");
        // buf statik — free etme
        return false;
    }
    statesStart += 10;  // "states":[ sonrası

    // Her [[...] array'ini sırayla işle
    char *p = statesStart;
    while (*p && count < MAX_AIRCRAFT) {
        // Sıradaki [ ara — yeni uçağın başı
        while (*p && *p != '[') p++;
        if (*p != '[') break;
        p++;  // [ sonrası

        // Tüm array'i kopyala (geçici buffer'a)
        char *arrayEnd = strchr(p, ']');
        if (!arrayEnd) break;
        int arrayLen = arrayEnd - p;
        if (arrayLen > 511) arrayLen = 511;  // güvenlik

        char arrBuf[512];
        strncpy(arrBuf, p, arrayLen);
        arrBuf[arrayLen] = '\0';
        
        p = arrayEnd + 1; // Sonraki uçağa geç

        // Array'i parse et — virgülle ayrılmış değerler
        // [0] icao24, [1] callsign, [2] country, [5] lon, [6] lat, [7] alt, [8] ground, [9] vel, [10] track
        // Manuel parse: virgülle split, null değerleri atla
        char *tokStart = arrBuf;
        int fieldIdx = 0;
        Aircraft &a = s_tempAircraft[count];

        // Tüm string değerleri boş başlat
        a.icao24[0] = '\0';
        a.callsign[0] = '\0';
        a.country[0] = '\0';
        a.lat = 0; a.lon = 0; a.altitude = 0; a.velocity = 0; a.heading = 0;
        a.on_ground = false;

        bool skip = false;
        while (tokStart && *tokStart && fieldIdx <= 10) {
            // Sıradaki token başı — " veya sayı veya null veya true/false
            while (*tokStart == ' ' || *tokStart == ',') tokStart++;
            if (*tokStart == '\0') break;

            char *tokEnd;
            if (*tokStart == '"') {
                // String değer
                tokStart++;
                tokEnd = strchr(tokStart, '"');
                if (!tokEnd) break;
                int slen = tokEnd - tokStart;
                if (slen < 0) slen = 0;
                switch (fieldIdx) {
                    case 0: { // icao24
                        if (slen > 6) slen = 6;
                        strncpy(a.icao24, tokStart, slen);
                        a.icao24[slen] = '\0';
                        break;
                    }
                    case 1: { // callsign — trim spaces
                        int j = 0;
                        for (int k = 0; k < slen && j < 9; k++) {
                            if (tokStart[k] != ' ') a.callsign[j++] = tokStart[k];
                        }
                        a.callsign[j] = '\0';
                        if (j == 0) strcpy(a.callsign, "----");
                        break;
                    }
                    case 2: { // country
                        if (slen > 15) slen = 15;
                        strncpy(a.country, tokStart, slen);
                        a.country[slen] = '\0';
                        break;
                    }
                }
                tokStart = tokEnd + 1;
            } else {
                // Sayı, null, true, false
                tokEnd = strchr(tokStart, ',');
                if (!tokEnd) tokEnd = strchr(tokStart, '\0');
                int slen = tokEnd - tokStart;
                char valBuf[32];
                if (slen > 30) slen = 30;
                strncpy(valBuf, tokStart, slen);
                valBuf[slen] = '\0';

                if (strncmp(valBuf, "null", 4) == 0) {
                    // null — atla
                } else if (strncmp(valBuf, "true", 4) == 0) {
                    if (fieldIdx == 8) a.on_ground = true;
                } else if (strncmp(valBuf, "false", 5) == 0) {
                    if (fieldIdx == 8) a.on_ground = false;
                } else {
                    // Sayı
                    float val = strtof(valBuf, nullptr);
                    switch (fieldIdx) {
                        case 5:  a.lon = val; break;
                        case 6:  a.lat = val; break;
                        case 7:  a.altitude = val; break;
                        case 9:  a.velocity = val; break;
                        case 10: a.heading = val; break;
                    }
                }
                tokStart = tokEnd;
            }
            fieldIdx++;
        }

        // Pozisyonu var mı? (lat/lon null olamaz)
        if (a.lat != 0 || a.lon != 0) {
            a.on_screen = false;
            a.last_update_ms = millis();  // Interpolation için zaman damgası
            count++;
        }

        p = arrayEnd + 1;
        // Sıradaki virgülü atla
        while (*p && *p != ',' && *p != ']') p++;
        if (*p == ',') p++;
    }

    // buf statik — free etme, bir sonraki poll'de tekrar kullan

    if (g_aircraftMutex != NULL) {
        if (xSemaphoreTake(g_aircraftMutex, portMAX_DELAY) == pdTRUE) {
            // Seçili uçağın ICAO kodunu kaydet
            char selectedIcao[20] = {0};
            if (g_selectedIdx >= 0 && g_selectedIdx < g_aircraftCount) {
                strncpy(selectedIcao, g_aircraft[g_selectedIdx].icao24, 19);
            }

            // Yeni listeyi kopyala
            memcpy(g_aircraft, s_tempAircraft, count * sizeof(Aircraft));
            g_aircraftCount = count;
            g_dataValid = true;
            g_lastUpdate_ms = millis();

            // Yeni listede eski uçağı bul ve seçimi güncelle
            int newSelectedIdx = -1;
            if (selectedIcao[0] != '\0') {
                for (int i = 0; i < count; i++) {
                    if (strcmp(g_aircraft[i].icao24, selectedIcao) == 0) {
                        newSelectedIdx = i;
                        break;
                    }
                }
            }
            g_selectedIdx = newSelectedIdx;

            xSemaphoreGive(g_aircraftMutex);
        }
    } else {
        // Fallback (Mutex yoksa)
        char selectedIcao[20] = {0};
        if (g_selectedIdx >= 0 && g_selectedIdx < g_aircraftCount) {
            strncpy(selectedIcao, g_aircraft[g_selectedIdx].icao24, 19);
        }
        memcpy(g_aircraft, s_tempAircraft, count * sizeof(Aircraft));
        g_aircraftCount = count;
        g_dataValid = true;
        g_lastUpdate_ms = millis();
        int newSelectedIdx = -1;
        if (selectedIcao[0] != '\0') {
            for (int i = 0; i < count; i++) {
                if (strcmp(g_aircraft[i].icao24, selectedIcao) == 0) {
                    newSelectedIdx = i;
                    break;
                }
            }
        }
        g_selectedIdx = newSelectedIdx;
    }

    Serial.print("[NET] Manuel parser: ");
    Serial.print(count);
    Serial.println(" ucak alindi");
    return true;
}
