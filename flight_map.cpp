// ============================================================
//  E-OS FLIGHT TRACKER — Harita Çizimi + Projeksiyon
//
//  Equirectangular projeksiyon (basit):
//    screen_x = (lon - center_lon) * pixelsPerDegree + SCR_W/2
//    screen_y = SCR_H/2 - (lat - center_lat) * pixelsPerDegree
//
//  Zoom seviyesine göre pixelsPerDegree hesaplanır:
//    zoom 1  = dünya (360° → 160px) → 0.44 px/deg
//    zoom 5  = Türkiye (40° → 160px) → 4 px/deg
//    zoom 10 = şehir (4° → 160px) → 40 px/deg
//
//  Harita: HİBRİT — Regional (Türkiye 2048x1024, YÜKSEK) + World (512x256, DÜŞÜK).
//  İkisi de SD'den açılışta PSRAM'e yüklenir. Viewport'a göre equirectangular
//  kırpma + bilinear ölçekleme: Türkiye'de net, dışında düşük çözünürlük dünya.
// ============================================================

#include "flight_internal.h"
#include "flight_borders.h"

// ─── Renk paleti değişkenleri (global) ───
uint16_t COL_BG;
uint16_t COL_OCEAN;
uint16_t COL_LAND;
uint16_t COL_BORDER;
uint16_t COL_ACCENT;
uint16_t COL_WHITE;
uint16_t COL_GRAY;
uint16_t COL_DARK_GRAY;
uint16_t COL_BLACK;
uint16_t COL_JET;
uint16_t COL_PROP;
uint16_t COL_GROUND;
uint16_t COL_SELECTED;
uint16_t COL_DANGER;

// ══════════════════════════════════════════════════════════════
//  mapSetupColors — Renk paletini başlat (tft.init() sonrası)
// ══════════════════════════════════════════════════════════════
void mapSetupColors() {
    COL_BG         = RGB_FIX(8, 8, 20);
    COL_OCEAN      = RGB_FIX(15, 25, 60);    // Koyu lacivert
    COL_LAND       = RGB_FIX(40, 60, 35);    // Koyu yeşil-kahve
    COL_BORDER     = RGB_FIX(60, 80, 50);    // Açık yeşil-kahve
    COL_ACCENT     = RGB_FIX(80, 180, 255);  // Cyan
    COL_WHITE      = RGB_FIX(255, 255, 255);
    COL_GRAY       = RGB_FIX(120, 120, 120);
    COL_DARK_GRAY  = RGB_FIX(50, 50, 50);
    COL_BLACK      = RGB_FIX(0, 0, 0);
    COL_JET        = RGB_FIX(255, 200, 50);  // Sarı — jet uçağı
    COL_PROP       = RGB_FIX(50, 255, 80);   // Yeşil — pervaneli
    COL_GROUND     = RGB_FIX(120, 120, 120); // Gri — yerde
    COL_SELECTED   = RGB_FIX(255, 50, 50);   // Kırmızı — seçili
    COL_DANGER     = RGB_FIX(255, 50, 50);
}

// ══════════════════════════════════════════════════════════════
//  pixelsPerDegree — zoom seviyesine göre px/degree
//  zoom 1: dünya, zoom 10: şehir
// ══════════════════════════════════════════════════════════════
static float pixelsPerDegree() {
    // zoom 1 = 0.44 px/deg (360° → 160px)
    // her zoom +1 → 2x zoom
    // zoom N = 0.44 * 2^(N-1)
    return 0.44f * powf(2.0f, g_view.zoom - 1);
}

// ══════════════════════════════════════════════════════════════
//  latLonToScreen — enlem/boylam → ekran pikseli
//  Equirectangular projeksiyon
// ══════════════════════════════════════════════════════════════
void latLonToScreen(float lat, float lon, int &sx, int &sy) {
    float ppd = pixelsPerDegree();
    // Boylam → X (sol→sağ, batı→doğu)
    sx = (int)((lon - g_view.center_lon) * ppd + SCR_W / 2.0f);
    // Enlem → Y (yukarı=eksi, aşağı=artı) — TFT'de Y aşağı büyür
    sy = (int)(SCR_H / 2.0f - (lat - g_view.center_lat) * ppd);
}

// ══════════════════════════════════════════════════════════════
//  screenToLatLon — ekran pikseli → enlem/boylam (ters projeksiyon)
// ══════════════════════════════════════════════════════════════
bool screenToLatLon(int sx, int sy, float &lat, float &lon) {
    float ppd = pixelsPerDegree();
    lon = g_view.center_lon + (sx - SCR_W / 2.0f) / ppd;
    lat = g_view.center_lat - (sy - SCR_H / 2.0f) / ppd;
    // Geçerlilik kontrol
    if (lat < -90 || lat > 90 || lon < -180 || lon > 180) return false;
    return true;
}

// ══════════════════════════════════════════════════════════════
//  mapGetViewBounds — mevcut viewport'un enlem/boylam sınırları
// ══════════════════════════════════════════════════════════════
void mapGetViewBounds(float &latMin, float &latMax, float &lonMin, float &lonMax) {
    float ppd = pixelsPerDegree();
    float halfLatRange = (SCR_H / 2.0f) / ppd;
    float halfLonRange = (SCR_W / 2.0f) / ppd;
    latMin = g_view.center_lat - halfLatRange;
    latMax = g_view.center_lat + halfLatRange;
    lonMin = g_view.center_lon - halfLonRange;
    lonMax = g_view.center_lon + halfLonRange;
}

// ══════════════════════════════════════════════════════════════
//  drawPolygon — enlem/boylam noktalarından polygon çiz
//  Basit kıta çizimi için — polygon fill değil, outline only
//  pts: {lat1,lon1, lat2,lon2, ...} float dizisi
//  n: nokta sayısı
// ══════════════════════════════════════════════════════════════
static void drawPolygon(const float *pts, int n, uint16_t color) {
    if (n <= 0) return;
    int prevX = -100, prevY = -100;
    bool penDown = false;
    
    for (int i = 0; i < n; i++) {
        float lat = pts[i * 2];
        float lon = pts[i * 2 + 1];
        
        if (lat >= 900.0f) { // 999.0 means break (pen up)
            penDown = false;
            continue;
        }
        
        int x, y;
        latLonToScreen(lat, lon, x, y);
        
        if (penDown) {
            // Sadece ekranda görünen veya yakın çizgileri çiz
            if ((x >= -50 && x <= SCR_W + 50 && y >= -50 && y <= SCR_H + 50) ||
                (prevX >= -50 && prevX <= SCR_W + 50 && prevY >= -50 && prevY <= SCR_H + 50)) {
                spr.drawLine(prevX, prevY, x, y, color);
            }
        }
        prevX = x;
        prevY = y;
        penDown = true;
    }
}

// ══════════════════════════════════════════════════════════════

// ══════════════════════════════════════════════════════════════
//  mapDrawBackground — HİBRİT PSRAM Harita Sistemi
//
//  Regional: 2048x1024, lat 25..55, lon 10..60 (Türkiye + çevresi, YÜKSEK)
//  World:    512x256,  lat -90..90, lon -180..180 (tüm dünya, DÜŞÜK)
//
//  Ekran pikseli regional bölgedeyse regional'dan, dışındaysa
//  world'den bilinear interpolation ile okunur. İkisi de PSRAM'de.
//  SD kart çizim sırasında DOKUNULMAZ (setup'ta yüklendi, SD.end()).
// ══════════════════════════════════════════════════════════════
void mapDrawBackground() {
    #define REG_W 2560
    #define REG_H 1280
    #define REG_LAT_MIN 25.0f
    #define REG_LAT_MAX 55.0f
    #define REG_LON_MIN 10.0f
    #define REG_LON_MAX 60.0f
    #define WLD_W 512
    #define WLD_H 256

    float ppd = pixelsPerDegree();
    uint16_t rowBuf[SCR_W];

    auto interpolate = [](const unsigned short* mapData, int mapW, int mapH, float fX, float fY) -> uint16_t {
        int x1 = (int)floorf(fX);
        int y1 = (int)floorf(fY);
        if (x1 < 0) x1 = 0; if (x1 >= mapW - 1) x1 = mapW - 2;
        if (y1 < 0) y1 = 0; if (y1 >= mapH - 1) y1 = mapH - 2;

        uint16_t c00 = mapData[y1 * mapW + x1];
        uint16_t c10 = mapData[y1 * mapW + (x1 + 1)];
        uint16_t c01 = mapData[(y1 + 1) * mapW + x1];
        uint16_t c11 = mapData[(y1 + 1) * mapW + (x1 + 1)];

        float dx = fX - x1;
        float dy = fY - y1;

        int r00 = (c00 >> 11) & 0x1F, g00 = (c00 >> 5) & 0x3F, b00 = c00 & 0x1F;
        int r10 = (c10 >> 11) & 0x1F, g10 = (c10 >> 5) & 0x3F, b10 = c10 & 0x1F;
        int r01 = (c01 >> 11) & 0x1F, g01 = (c01 >> 5) & 0x3F, b01 = c01 & 0x1F;
        int r11 = (c11 >> 11) & 0x1F, g11 = (c11 >> 5) & 0x3F, b11 = c11 & 0x1F;

        float r_top = r00 + dx * (r10 - r00);
        float g_top = g00 + dx * (g10 - g00);
        float b_top = b00 + dx * (b10 - b00);

        float r_bot = r01 + dx * (r11 - r01);
        float g_bot = g01 + dx * (g11 - g01);
        float b_bot = b01 + dx * (b11 - b01);

        int r = (int)(r_top + dy * (r_bot - r_top));
        int g = (int)(g_top + dy * (g_bot - g_top));
        int b = (int)(b_top + dy * (b_bot - b_top));

        return (r << 11) | (g << 5) | b;
    };

    for (int sy = 0; sy < SCR_H; sy++) {
        float lat = g_view.center_lat - (sy - SCR_H / 2.0f) / ppd;
        if (lat < -90) lat = -90;
        if (lat > 90) lat = 90;

        for (int sx = 0; sx < SCR_W; sx++) {
            float lon = g_view.center_lon + (sx - SCR_W / 2.0f) / ppd;
            if (lon < -180) lon += 360;
            if (lon > 180) lon -= 360;

            // Regional bölgede mi? (Türkiye + çevresi, yüksek çözünürlük)
            if (g_psramMap &&
                lat >= REG_LAT_MIN && lat <= REG_LAT_MAX &&
                lon >= REG_LON_MIN && lon <= REG_LON_MAX) {
                // lat 55->y0, 25->y1023 ; lon 10->x0, 60->x2047
                float fX = ((lon - REG_LON_MIN) / (REG_LON_MAX - REG_LON_MIN) * REG_W) - 0.5f;
                float fY = ((REG_LAT_MAX - lat) / (REG_LAT_MAX - REG_LAT_MIN) * REG_H) - 0.5f;
                rowBuf[sx] = interpolate(g_psramMap, REG_W, REG_H, fX, fY);
            } else if (g_psramMapWorld) {
                // Dünya düşük çözünürlük: lat 90->y0, -90->y255 ; lon -180->x0, 180->x511
                float fX = ((lon + 180.0f) / 360.0f * WLD_W) - 0.5f;
                float fY = ((90.0f - lat) / 180.0f * WLD_H) - 0.5f;
                rowBuf[sx] = interpolate(g_psramMapWorld, WLD_W, WLD_H, fX, fY);
            } else {
                rowBuf[sx] = COL_OCEAN;
            }
        }

        spr.pushImage(0, sy, SCR_W, 1, rowBuf);

        // PSRAM'den yogun okuma sirasinda WDT crash'ini onle
        if ((sy & 31) == 0) yield();
    }
}



struct City {
    float lat;
    float lon;
    const char* name;
};

static const City CITIES[] = {
    {41.0, 28.97, "IST"},
    {39.93, 32.85, "ANK"},
    {38.42, 27.14, "IZM"},
    {36.9, 30.7,  "ANT"},
    {41.3, 36.3,  "SAM"},
    {37.0, 35.3,  "ADA"},
    {38.7, 35.5,  "KAY"},
    {37.9, 40.2,  "GZT"},
    {39.9, 41.3,  "ERZ"},
    {38.5, 43.1,  "VAN"},
    {40.15, 29.98, "BRS"},
    {40.78, 30.38, "EKS"},
};
static const int NUM_CITIES = (int)(sizeof(CITIES) / sizeof(CITIES[0]));

void mapDrawGrid() {
    if (g_view.zoom >= 3) {
        uint16_t borderColor = RGB_FIX(150, 150, 150); // Şeffaf gri (Eski haline benzer ama göz yormaz)
        int n_am = sizeof(AM_POLYGON)/sizeof(AM_POLYGON[0])/2; drawPolygon(AM_POLYGON, n_am, borderColor);
        int n_bg = sizeof(BG_POLYGON)/sizeof(BG_POLYGON[0])/2; drawPolygon(BG_POLYGON, n_bg, borderColor);
        int n_cy = sizeof(CY_POLYGON)/sizeof(CY_POLYGON[0])/2; drawPolygon(CY_POLYGON, n_cy, borderColor);
        int n_ge = sizeof(GE_POLYGON)/sizeof(GE_POLYGON[0])/2; drawPolygon(GE_POLYGON, n_ge, borderColor);
        int n_gr = sizeof(GR_POLYGON)/sizeof(GR_POLYGON[0])/2; drawPolygon(GR_POLYGON, n_gr, borderColor);
        int n_iq = sizeof(IQ_POLYGON)/sizeof(IQ_POLYGON[0])/2; drawPolygon(IQ_POLYGON, n_iq, borderColor);
        int n_ir = sizeof(IR_POLYGON)/sizeof(IR_POLYGON[0])/2; drawPolygon(IR_POLYGON, n_ir, borderColor);
        int n_sy = sizeof(SY_POLYGON)/sizeof(SY_POLYGON[0])/2; drawPolygon(SY_POLYGON, n_sy, borderColor);
        int n_tr = sizeof(TR_POLYGON)/sizeof(TR_POLYGON[0])/2; drawPolygon(TR_POLYGON, n_tr, borderColor);
    }
    
    if (g_view.zoom >= 4) {
        spr.setTextColor(COL_WHITE);
        spr.setTextSize(1);
        spr.setTextWrap(false, false); // Ekranın sağına taşan yazılar sola atlamasın
        for (int i = 0; i < NUM_CITIES; i++) {
            int cx, cy;
            latLonToScreen(CITIES[i].lat, CITIES[i].lon, cx, cy);
            if (cx >= 0 && cx < SCR_W && cy >= 0 && cy < SCR_H) {
                spr.fillCircle(cx, cy, 2, COL_BLACK); // Siyah iç
                spr.drawCircle(cx, cy, 2, COL_WHITE); // Beyaz dış çerçeve
                spr.setCursor(cx + 4, cy - 3);
                spr.print(CITIES[i].name);
            }
        }
    }
}
