#include "net_rest.h"
#include "config.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <JPEGDEC.h>
#include "bsp/bsp_display.h"

static JPEGDEC jpg;

String server_ws_base() {
  String base = SERVER_BASE;
  if (base.startsWith("http://")) base = "ws://" + base.substring(7);
  else if (base.startsWith("https://")) base = "wss://" + base.substring(8);
  return base;
}

static bool http_get_json(const String &path, JsonDocument &doc) {
  HTTPClient http;
  http.setTimeout(8000);
  if (!http.begin(String(SERVER_BASE) + path)) return false;
  int code = http.GET();
  if (code != 200) {
    http.end();
    return false;
  }
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  return err == DeserializationError::Ok;
}

bool rest_health() {
  HTTPClient http;
  http.setTimeout(3000);
  if (!http.begin(String(SERVER_BASE) + "/api/health")) return false;
  int code = http.GET();
  http.end();
  if (code == 200) {
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    g_state.server_ok = true;
    xSemaphoreGive(state_mutex);
    return true;
  }
  xSemaphoreTake(state_mutex, portMAX_DELAY);
  g_state.server_ok = false;
  xSemaphoreGive(state_mutex);
  return false;
}

// ---- 头像缓存 ----

#define AVATAR_SIZE 192

struct AvatarEntry {
  String cat_id;
  uint16_t *pixels;  // RGB565 AVATAR_SIZE x AVATAR_SIZE，PSRAM
  lv_img_dsc_t dsc;
};
static std::vector<AvatarEntry> avatars;

void *avatar_img_dsc(const String &cat_id) {
  for (auto &a : avatars) {
    if (a.cat_id == cat_id) return (void *)&a.dsc;
  }
  return nullptr;
}

void avatar_cache_clear() {
  for (auto &a : avatars) {
    if (a.pixels) heap_caps_free(a.pixels);
  }
  avatars.clear();
}

static int avatar_jpeg_draw(JPEGDRAW *d) {
  uint16_t *dst = (uint16_t *)d->pUser;
  for (int y = 0; y < d->iHeight; y++) {
    int dy = d->y + y;
    if (dy >= AVATAR_SIZE) break;
    int copy = d->iWidth;
    if (d->x + copy > AVATAR_SIZE) copy = AVATAR_SIZE - d->x;
    if (copy > 0) memcpy(dst + dy * AVATAR_SIZE + d->x, d->pPixels + y * d->iWidth, copy * 2);
  }
  return 1;
}

static bool decode_avatar(const uint8_t *jpeg, size_t len, uint16_t *out) {
  memset(out, 0, AVATAR_SIZE * AVATAR_SIZE * 2);
  if (!jpg.openRAM((uint8_t *)jpeg, len, avatar_jpeg_draw)) return false;
  jpg.setUserPointer(out);
  // 服务端头像约 192x192；若更大则按比例缩小
  int w = jpg.getWidth(), h = jpg.getHeight();
  int scale = 0;
  while ((w >> (scale + 1)) >= AVATAR_SIZE && (h >> (scale + 1)) >= AVATAR_SIZE) scale++;
  jpg.setPixelType(RGB565_LITTLE_ENDIAN);
  int ok = jpg.decode(0, 0, scale);
  jpg.close();
  return ok == 1;
}

static bool avatar_download_decode(const String &cat_id, uint16_t *out) {
  HTTPClient http;
  http.setTimeout(10000);
  String path = String(SERVER_BASE) + "/api/cats/" + cat_id + "/avatar";
  if (!http.begin(path)) return false;
  int code = http.GET();
  if (code != 200) {
    http.end();
    return false;
  }
  int len = http.getSize();
  WiFiClient *stream = http.getStreamPtr();
  uint8_t *buf = (uint8_t *)heap_caps_malloc(len > 0 ? len : 200 * 1024, MALLOC_CAP_SPIRAM);
  if (!buf) {
    http.end();
    return false;
  }
  size_t got = 0;
  int cap = len > 0 ? len : 200 * 1024;
  uint32_t last_rx = millis();
  while (http.connected() && got < (size_t)cap) {
    size_t avail = stream->available();
    if (avail) {
      int r = stream->read(buf + got, min(avail, (size_t)(cap - got)));
      if (r <= 0) break;
      got += r;
      last_rx = millis();
    } else {
      if (len > 0 && got >= (size_t)len) break;
      if (millis() - last_rx > 5000) break;  // 对端停滞超时
      delay(1);
    }
  }
  http.end();
  bool ok = got > 0 && decode_avatar(buf, got, out);
  if (ok) {
    // 缓存到 LittleFS
    if (!LittleFS.exists("/avatars_v6")) LittleFS.mkdir("/avatars_v6");
    String p = "/avatars_v6/" + cat_id + ".jpg";
    File f = LittleFS.open(p, "w");
    if (f) {
      f.write(buf, got);
      f.close();
    }
  }
  heap_caps_free(buf);
  return ok;
}

bool rest_fetch_cats() {
  JsonDocument doc;
  if (!http_get_json("/api/cats", doc)) return false;
  if (!doc.is<JsonArray>()) return false;

  xSemaphoreTake(state_mutex, portMAX_DELAY);
  std::vector<String> old_paths;
  std::map<String, int> old_counts;
  for (auto &c : g_state.cats) old_counts[c.id] = c.today_count;
  g_state.cats.clear();
  for (JsonObject c : doc.as<JsonArray>()) {
    CatInfo info;
    info.id = c["id"].as<String>();
    info.name = c["name"].as<String>();
    info.avatar_path = "/avatars_v6/" + info.id + ".jpg";
    auto it = old_counts.find(info.id);
    if (it != old_counts.end()) info.today_count = it->second;
    g_state.cats.push_back(info);
  }
  xSemaphoreGive(state_mutex);

  // 解码头像（新猫或尚未解码的）
  std::vector<String> ids;
  xSemaphoreTake(state_mutex, portMAX_DELAY);
  for (auto &c : g_state.cats) ids.push_back(c.id);
  xSemaphoreGive(state_mutex);

  for (auto &id : ids) {
    if (avatar_img_dsc(id)) continue;
    uint16_t *pixels = (uint16_t *)heap_caps_malloc(AVATAR_SIZE * AVATAR_SIZE * 2, MALLOC_CAP_SPIRAM);
    if (!pixels) continue;
    bool ok = false;
    // 优先 LittleFS 缓存
    String p = "/avatars_v6/" + id + ".jpg";  // v6：啵啵再换新头像
    if (LittleFS.exists(p)) {
      File f = LittleFS.open(p, "r");
      size_t len = f.size();
      uint8_t *buf = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
      if (buf) {
        f.read(buf, len);
        ok = decode_avatar(buf, len, pixels);
        heap_caps_free(buf);
      }
      f.close();
    }
    if (!ok) ok = avatar_download_decode(id, pixels);
    if (ok) {
      AvatarEntry e;
      e.cat_id = id;
      e.pixels = pixels;
      e.dsc.header.always_zero = 0;
      e.dsc.header.w = AVATAR_SIZE;
      e.dsc.header.h = AVATAR_SIZE;
      e.dsc.data_size = AVATAR_SIZE * AVATAR_SIZE * 2;
      e.dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
      e.dsc.data = (const uint8_t *)pixels;
      avatars.push_back(e);
    } else {
      heap_caps_free(pixels);
    }
  }
  return true;
}

bool rest_fetch_today() {
  JsonDocument doc;
  if (!http_get_json("/api/today", doc)) return false;
  std::map<String, int> counts;
  String date = doc["date"].as<String>();
  JsonObject c = doc["counts"].as<JsonObject>();
  for (JsonPair kv : c) counts[kv.key().c_str()] = kv.value().as<int>();
  state_set_counts(date, counts);
  xSemaphoreTake(state_mutex, portMAX_DELAY);
  g_state.events_today = doc["events_today"] | g_state.events_today;
  xSemaphoreGive(state_mutex);
  return true;
}

bool rest_fetch_clips(const String &cat_id, const String &date, std::vector<ClipInfo> &out) {
  JsonDocument doc;
  String q = "/api/clips?cat_id=" + cat_id + "&date=" + date;
  if (!http_get_json(q, doc)) return false;
  if (!doc.is<JsonArray>()) return false;
  out.clear();
  for (JsonObject e : doc.as<JsonArray>()) {
    ClipInfo ci;
    ci.id = e["id"] | 0L;
    ci.cat_id = e["cat_id"].as<String>();
    ci.start_ts = e["start_ts"] | 0.0;
    ci.duration_s = e["duration_s"] | 0.0;
    ci.status = e["status"].as<String>();
    out.push_back(ci);
  }
  return true;
}
