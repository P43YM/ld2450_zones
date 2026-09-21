#include "ld2450_zones.h"
#include "ui.h"

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/components/network/util.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace esphome {
namespace ld2450_zones {

static const char *const TAG = "ld2450_zones";
static const uint16_t BLOB_MAGIC = 0x2451;  // v2: с именем зоны
static const uint16_t SETTINGS_MAGIC = 0x2452;
static const uint8_t FRAME_HEADER[4] = {0xAA, 0xFF, 0x03, 0x00};
static const size_t FRAME_LEN = 30;

// ---------------------------------------------------------------- вспомогательное

static void appendf(std::string &s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void appendf(std::string &s, const char *fmt, ...) {
  char b[160];
  va_list a;
  va_start(a, fmt);
  vsnprintf(b, sizeof(b), fmt, a);
  va_end(a);
  s += b;
}

static void json_escape(std::string &out, const std::string &in) {
  for (unsigned char c : in) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += (char) c;
    } else if (c < 0x20) {
      out += ' ';
    } else {
      out += (char) c;
    }
  }
}

/// Формат LD2450: старший бит = 1 означает положительное значение, 0 - отрицательное.
static int16_t decode_signed(uint8_t lo, uint8_t hi) {
  uint16_t raw = (uint16_t) lo | ((uint16_t) hi << 8);
  int16_t v = raw & 0x7FFF;
  return (raw & 0x8000) ? v : -v;
}

// ---------------------------------------------------------------- настройка

void LD2450Zones::add_zone(const std::string &label, uint32_t hold_s) {
  Zone z;
  z.label = label;
  z.hold_s = hold_s;
  this->zones_.push_back(std::move(z));
}

void LD2450Zones::set_zone_presence(uint8_t i, binary_sensor::BinarySensor *s) {
  if (i < this->zones_.size())
    this->zones_[i].presence_sensor = s;
}

void LD2450Zones::set_zone_count(uint8_t i, sensor::Sensor *s) {
  if (i < this->zones_.size())
    this->zones_[i].count_sensor = s;
}

void LD2450Zones::set_zone_label_sensor(uint8_t i, text_sensor::TextSensor *s) {
  if (i < this->zones_.size())
    this->zones_[i].label_sensor = s;
}

void LD2450Zones::setup() {
  const size_t nbytes = ((size_t) this->cols_ * this->rows_ + 7) / 8;
  for (auto &z : this->zones_)
    z.cells.assign(nbytes, 0);
  this->ignore_.cells.assign(nbytes, 0);
  this->load_zones_();
  this->load_settings_();
  this->fps_t0_ = millis();
  if (this->multi_target_)
    this->set_timeout("radar_cfg", 3000, [this]() { this->configure_radar_(); });
}

void LD2450Zones::dump_config() {
  ESP_LOGCONFIG(TAG, "LD2450 Zones:");
  ESP_LOGCONFIG(TAG, "  Web UI port: %u", this->web_port_);
  ESP_LOGCONFIG(TAG, "  Area: %d x %d mm, cell %d mm (%d x %d)", this->width_mm_, this->depth_mm_, this->cell_mm_,
                this->cols_, this->rows_);
  ESP_LOGCONFIG(TAG, "  Multi target: %s", YESNO(this->multi_target_));
  ESP_LOGCONFIG(TAG, "  Filter: on when seen in %u of last %u frames", this->on_count_, this->window_);
  ESP_LOGCONFIG(TAG, "  Zones: %u", (unsigned) this->zones_.size());
  for ([[maybe_unused]] auto &z : this->zones_)
    ESP_LOGCONFIG(TAG, "    '%s', hold %us", z.label.c_str(), (unsigned) z.hold_s);
}

// ---------------------------------------------------------------- хранение зон

void LD2450Zones::load_zones_() {
  const size_t nbytes = ((size_t) this->cols_ * this->rows_ + 7) / 8;
  auto load_one = [&](Zone &z, uint32_t key, bool named) {
    z.pref = global_preferences->make_preference<ZoneBlob>(key);
    ZoneBlob b;
    if (z.pref.load(&b) && b.magic == BLOB_MAGIC && b.cols == this->cols_ && b.rows == this->rows_) {
      memcpy(z.cells.data(), b.cells, nbytes);
      z.hold_s = b.hold_s;
      b.label[LABEL_MAX - 1] = '\0';
      if (named && b.label[0] != '\0')
        z.label = b.label;
    }
  };
  for (size_t i = 0; i < this->zones_.size(); i++)
    load_one(this->zones_[i], fnv1_hash("ld2450_zones") + (uint32_t) i, true);
  load_one(this->ignore_, fnv1_hash("ld2450_zones_ignore"), false);
}

void LD2450Zones::load_settings_() {
  this->settings_pref_ = global_preferences->make_preference<SettingsBlob>(fnv1_hash("ld2450_zones_cfg"));
  SettingsBlob b;
  if (this->settings_pref_.load(&b) && b.magic == SETTINGS_MAGIC && b.window >= 1 && b.window <= 32 &&
      b.on_count >= 1 && b.on_count <= b.window && b.hold_s <= 3600) {
    this->on_count_ = b.on_count;
    this->window_ = b.window;
    this->global_hold_s_ = b.hold_s;
  }
}

/// Отправляет в HA актуальные названия зон (при старте и после переименования в веб-интерфейсе).
void LD2450Zones::publish_labels_() {
  for (auto &z : this->zones_) {
    if (z.label_sensor == nullptr)
      continue;
    std::string cur;
    {
      std::lock_guard<std::mutex> lk(this->mtx_);
      if (!z.label_dirty)
        continue;
      z.label_dirty = false;
      cur = z.label;
    }
    z.label_sensor->publish_state(cur);
  }
}

void LD2450Zones::save_pending_() {
  bool any = false;
  auto handle = [&](Zone &z) {
    ZoneBlob b{};
    bool go = false;
    {
      std::lock_guard<std::mutex> lk(this->mtx_);
      if (z.save_pending) {
        z.save_pending = false;
        go = true;
        b.magic = BLOB_MAGIC;
        b.cols = this->cols_;
        b.rows = this->rows_;
        b.hold_s = (uint16_t) z.hold_s;
        strncpy(b.label, z.label.c_str(), LABEL_MAX - 1);
        memcpy(b.cells, z.cells.data(), z.cells.size());
      }
    }
    if (go) {
      z.pref.save(&b);
      any = true;
    }
  };
  for (auto &z : this->zones_)
    handle(z);
  handle(this->ignore_);

  SettingsBlob sb{};
  bool save_cfg = false;
  {
    std::lock_guard<std::mutex> lk(this->mtx_);
    if (this->settings_pending_) {
      this->settings_pending_ = false;
      save_cfg = true;
      sb.magic = SETTINGS_MAGIC;
      sb.on_count = this->on_count_;
      sb.window = this->window_;
      sb.hold_s = (uint16_t) this->global_hold_s_;
    }
  }
  if (save_cfg) {
    this->settings_pref_.save(&sb);
    any = true;
  }
  if (any) {
    global_preferences->sync();
    ESP_LOGI(TAG, "Settings saved to flash");
  }
}

bool LD2450Zones::set_zone_cells(int idx, int hold_s, const std::string &name, const std::string &bits) {
  const size_t n = (size_t) this->cols_ * this->rows_;
  if (bits.size() != n)
    return false;
  const bool is_ignore = idx == IGNORE_IDX;
  if (!is_ignore && (idx < 0 || idx >= (int) this->zones_.size()))
    return false;
  std::lock_guard<std::mutex> lk(this->mtx_);
  Zone &z = is_ignore ? this->ignore_ : this->zones_[idx];
  for (size_t i = 0; i < n; i++)
    z.set_cell(i, bits[i] == '1');
  if (hold_s >= 0)
    z.hold_s = (uint32_t) std::min(hold_s, 3600);
  if (!is_ignore && !name.empty()) {
    size_t len = std::min(name.size(), LABEL_MAX - 1);
    while (len > 0 && len < name.size() && (name[len] & 0xC0) == 0x80)  // не резать UTF-8 посередине
      len--;
    if (z.label != name.substr(0, len)) {
      z.label = name.substr(0, len);
      z.label_dirty = true;
    }
  }
  z.save_pending = true;  // запись во flash делает основной поток
  return true;
}

bool LD2450Zones::set_settings(int on_count, int window, int hold_s) {
  if (window < 1 || window > 32 || on_count < 1 || on_count > window || hold_s < 0 || hold_s > 3600)
    return false;
  std::lock_guard<std::mutex> lk(this->mtx_);
  this->on_count_ = (uint8_t) on_count;
  this->window_ = (uint8_t) window;
  this->global_hold_s_ = (uint32_t) hold_s;
  this->settings_pending_ = true;
  return true;
}

// ---------------------------------------------------------------- радар

void LD2450Zones::send_cmd_(uint16_t cmd, const uint8_t *val, uint8_t vlen) {
  uint8_t buf[16];
  size_t n = 0;
  buf[n++] = 0xFD;
  buf[n++] = 0xFC;
  buf[n++] = 0xFB;
  buf[n++] = 0xFA;
  buf[n++] = (uint8_t) (2 + vlen);
  buf[n++] = 0x00;
  buf[n++] = (uint8_t) (cmd & 0xFF);
  buf[n++] = (uint8_t) (cmd >> 8);
  for (uint8_t i = 0; i < vlen; i++)
    buf[n++] = val[i];
  buf[n++] = 0x04;
  buf[n++] = 0x03;
  buf[n++] = 0x02;
  buf[n++] = 0x01;
  this->write_array(buf, n);
}

/// Включаем режим отслеживания нескольких целей: 0x00FF (вход в конфиг) - 0x0090 - 0x00FE (выход).
void LD2450Zones::configure_radar_() {
  ESP_LOGD(TAG, "Sending multi-target command to radar");
  static const uint8_t enable[2] = {0x01, 0x00};
  this->send_cmd_(0x00FF, enable, 2);
  this->set_timeout("radar_cfg2", 150, [this]() { this->send_cmd_(0x0090, nullptr, 0); });
  this->set_timeout("radar_cfg3", 300, [this]() { this->send_cmd_(0x00FE, nullptr, 0); });
}

void LD2450Zones::feed_(uint8_t b) {
  // ищем заголовок AA FF 03 00, ответы на команды (FD FC FB FA ...) просто пропускаются
  if (this->pos_ < 4) {
    if (b == FRAME_HEADER[this->pos_]) {
      this->buf_[this->pos_++] = b;
    } else {
      this->skipped_++;
      this->pos_ = 0;
      if (b == FRAME_HEADER[0])
        this->buf_[this->pos_++] = b;
    }
    return;
  }
  this->buf_[this->pos_++] = b;
  if (this->pos_ == FRAME_LEN) {
    if (this->buf_[28] == 0x55 && this->buf_[29] == 0xCC)
      this->process_frame_(this->buf_);
    else
      this->frames_bad_++;
    this->pos_ = 0;
  }
}

void LD2450Zones::process_frame_(const uint8_t *f) {
  Target t[MAX_TARGETS];
  for (uint8_t i = 0; i < MAX_TARGETS; i++) {
    const uint8_t *p = f + 4 + 8 * i;
    uint16_t xr = (uint16_t) p[0] | ((uint16_t) p[1] << 8);
    uint16_t yr = (uint16_t) p[2] | ((uint16_t) p[3] << 8);
    t[i].x = decode_signed(p[0], p[1]);
    t[i].y = decode_signed(p[2], p[3]);
    t[i].speed = decode_signed(p[4], p[5]);
    t[i].res = (uint16_t) p[6] | ((uint16_t) p[7] << 8);
    t[i].valid = (xr | yr) != 0;  // пустой слот приходит нулями
  }
  this->apply_(t, millis(), true);
}

int LD2450Zones::cell_index_(int x, int y) const {
  const int x0 = x + this->width_mm_ / 2;
  if (y < 0 || y >= this->depth_mm_ || x0 < 0 || x0 >= this->width_mm_)
    return -1;
  return (y / this->cell_mm_) * this->cols_ + (x0 / this->cell_mm_);
}

/// Главная логика: вызывается на каждый кадр радара (~10 Гц), в HA уходят только изменения.
void LD2450Zones::apply_(const Target *t, uint32_t now, bool real_frame) {
  bool zone_on[MAX_ZONES] = {false};
  uint8_t zone_cnt[MAX_ZONES] = {0};
  uint8_t total = 0;
  bool g_on;
  {
    std::lock_guard<std::mutex> lk(this->mtx_);
    for (uint8_t i = 0; i < MAX_TARGETS; i++)
      this->targets_[i] = t[i];

    if (real_frame) {
      if (this->last_frame_ms_ != 0)
        this->gap_cur_ = std::max(this->gap_cur_, elapsed_ms(now, this->last_frame_ms_));
      this->frames_ok_++;
      this->last_frame_ms_ = now;
      this->fps_frames_++;
      if (elapsed_ms(now, this->fps_t0_) >= 1000) {
        this->fps_ = this->fps_frames_ * 1000.0f / elapsed_ms(now, this->fps_t0_);
        this->fps_frames_ = 0;
        this->fps_t0_ = now;
      }
    }

    for (uint8_t i = 0; i < MAX_TARGETS; i++) {
      this->status_[i] = 0;
      if (!t[i].valid)
        continue;
      int c = this->cell_index_(t[i].x, t[i].y);
      if (c < 0) {
        this->status_[i] = 2;
        continue;  // вне области интереса
      }
      if (this->ignore_.has_cell((size_t) c)) {
        this->status_[i] = 3;
        continue;  // в зоне игнорирования
      }
      this->status_[i] = 1;
      total++;
      for (size_t z = 0; z < this->zones_.size(); z++)
        if (this->zones_[z].has_cell((size_t) c))
          zone_cnt[z]++;
    }

    this->counted_ = total;
    this->global_.update(total > 0, this->on_count_, this->window_, now, this->global_hold_s_ * 1000);
    g_on = this->global_.on;
    for (size_t z = 0; z < this->zones_.size(); z++) {
      Zone &zn = this->zones_[z];
      zn.count = zone_cnt[z];
      zn.pres.update(zone_cnt[z] > 0, this->on_count_, this->window_, now, zn.hold_s * 1000);
      zone_on[z] = zn.pres.on;
    }
  }
  this->publish_(now, g_on, total, zone_on, zone_cnt);
}

void LD2450Zones::publish_(uint32_t now, bool g_on, uint8_t total, const bool *zone_on, const uint8_t *zone_cnt) {
  if (this->presence_sensor_ != nullptr && this->pub_global_on_ != (int8_t) g_on) {
    this->pub_global_on_ = g_on;
    this->presence_sensor_->publish_state(g_on);
  }
  if (this->target_count_sensor_ != nullptr && this->pub_total_ != total &&
      (this->pub_total_ < 0 || elapsed_ms(now, this->pub_total_ms_) >= 1000)) {
    this->pub_total_ = total;
    this->pub_total_ms_ = now;
    this->target_count_sensor_->publish_state(total);
  }
  for (size_t i = 0; i < this->zones_.size(); i++) {
    Zone &z = this->zones_[i];
    if (z.presence_sensor != nullptr && z.pub_on != (int8_t) zone_on[i]) {
      z.pub_on = zone_on[i];
      z.presence_sensor->publish_state(zone_on[i]);
    }
    if (z.count_sensor != nullptr && z.pub_count != zone_cnt[i] &&
        (z.pub_count < 0 || elapsed_ms(now, z.pub_count_ms) >= 1000)) {
      z.pub_count = zone_cnt[i];
      z.pub_count_ms = now;
      z.count_sensor->publish_state(zone_cnt[i]);
    }
  }
}

void LD2450Zones::loop() {
  const uint32_t loop_start = millis();
  const uint32_t frames_before = this->frames_ok_;

  // задержка главного цикла: если ESP не успевает, здесь будут большие значения
  if (this->last_loop_ms_ != 0)
    this->lag_cur_ = std::max(this->lag_cur_, elapsed_ms(loop_start, this->last_loop_ms_));
  this->last_loop_ms_ = loop_start;
  if (elapsed_ms(loop_start, this->diag_t0_) >= 10000) {  // максимумы считаем по окнам 10 с
    this->gap_prev_ = this->gap_cur_;
    this->lag_prev_ = this->lag_cur_;
    this->gap_cur_ = this->lag_cur_ = 0;
    this->diag_t0_ = loop_start;
  }

  uint8_t b;
  while (this->available()) {
    if (!this->read_byte(&b))
      break;
    this->feed_(b);
  }

  // Время берём ПОСЛЕ чтения: кадры обрабатываются с более поздней меткой, чем loop_start. Раньше здесь стоял
  // loop_start, и «now - last_frame_ms_» в беззнаковых числах давало ~4 млрд, из-за чего сразу после настоящего
  // кадра запускался ложный пустой кадр: цель «пропадала», присутствие гасло.
  const uint32_t now = millis();

  // радар молчит: продолжаем «тикать» пустыми кадрами, чтобы зоны освобождались по таймауту
  if (this->frames_ok_ == frames_before && elapsed_ms(now, this->last_frame_ms_) > 2000 &&
      elapsed_ms(now, this->last_tick_ms_) >= 100) {
    this->last_tick_ms_ = now;
    Target none[MAX_TARGETS];
    this->apply_(none, now, false);
  }

  this->save_pending_();
  this->publish_labels_();

  if (this->server_ == nullptr && now >= this->next_server_try_ && network::is_connected()) {
    this->next_server_try_ = now + 5000;
    this->start_server_();
  }
}

// ---------------------------------------------------------------- JSON

std::string LD2450Zones::state_json() {
  std::lock_guard<std::mutex> lk(this->mtx_);
  const uint32_t now = millis();
  std::string s;
  s.reserve(560);
  appendf(s, "{\"b\":\"%s\",\"age\":%u,\"fps\":%.1f,\"p\":%d,\"gh\":%u,\"win\":%u,", BUILD_ID,
          (unsigned) elapsed_ms(now, this->last_frame_ms_), this->fps_, this->global_.on ? 1 : 0,
          (unsigned) this->global_.hits, (unsigned) this->window_);
  appendf(s, "\"on\":%u,\"hold\":%u,\"ga\":%d,\"n\":%u,", (unsigned) this->on_count_, (unsigned) this->global_hold_s_,
          this->global_.last_seen ? (int) elapsed_ms(now, this->global_.last_seen) : -1, (unsigned) this->counted_);
  appendf(s, "\"ok\":%u,\"bad\":%u,\"skip\":%u,\"gap\":%u,\"lag\":%u,\"t\":[", (unsigned) this->frames_ok_,
          (unsigned) this->frames_bad_, (unsigned) this->skipped_,
          (unsigned) std::max(this->gap_cur_, this->gap_prev_), (unsigned) std::max(this->lag_cur_, this->lag_prev_));
  for (uint8_t i = 0; i < MAX_TARGETS; i++) {
    const Target &t = this->targets_[i];
    appendf(s, "%s{\"x\":%d,\"y\":%d,\"v\":%d,\"r\":%u,\"a\":%d,\"s\":%u}", i ? "," : "", t.x, t.y, t.speed,
            (unsigned) t.res, t.valid ? 1 : 0, (unsigned) this->status_[i]);
  }
  s += "],\"z\":[";
  for (size_t i = 0; i < this->zones_.size(); i++)
    appendf(s, "%s{\"o\":%d,\"c\":%u,\"h\":%u}", i ? "," : "", this->zones_[i].pres.on ? 1 : 0,
            (unsigned) this->zones_[i].count, (unsigned) this->zones_[i].pres.hits);
  s += "]}";
  return s;
}

std::string LD2450Zones::zones_json() {
  std::lock_guard<std::mutex> lk(this->mtx_);
  const size_t n = (size_t) this->cols_ * this->rows_;
  std::string s;
  s.reserve(120 + this->zones_.size() * (n + 80));
  appendf(s, "{\"cols\":%d,\"rows\":%d,\"cell\":%d,\"w\":%d,\"d\":%d,\"zones\":[", this->cols_, this->rows_,
          this->cell_mm_, this->width_mm_, this->depth_mm_);
  for (size_t i = 0; i < this->zones_.size(); i++) {
    const Zone &z = this->zones_[i];
    if (i)
      s += ",";
    s += "{\"name\":\"";
    json_escape(s, z.label);
    appendf(s, "\",\"hold\":%u,\"cells\":\"", (unsigned) z.hold_s);
    for (size_t c = 0; c < n; c++)
      s += z.has_cell(c) ? '1' : '0';
    s += "\"}";
  }
  s += "],\"ignore\":\"";
  for (size_t c = 0; c < n; c++)
    s += this->ignore_.has_cell(c) ? '1' : '0';
  appendf(s, "\",\"cfg\":{\"on\":%u,\"win\":%u,\"hold\":%u}}", (unsigned) this->on_count_, (unsigned) this->window_,
          (unsigned) this->global_hold_s_);
  return s;
}

// ---------------------------------------------------------------- HTTP

static esp_err_t send_json(httpd_req_t *req, const std::string &s) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, s.c_str(), s.length());
}

static esp_err_t h_root(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  return httpd_resp_send(req, UI_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_state(httpd_req_t *req) {
  auto *self = static_cast<LD2450Zones *>(req->user_ctx);
  return send_json(req, self->state_json());
}

static esp_err_t h_zones(httpd_req_t *req) {
  auto *self = static_cast<LD2450Zones *>(req->user_ctx);
  return send_json(req, self->zones_json());
}

static bool read_body(httpd_req_t *req, size_t max_len, std::string &body) {
  const size_t len = req->content_len;
  if (len == 0 || len > max_len) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad length");
    return false;
  }
  body.assign(len, '\0');
  size_t got = 0;
  int retries = 0;
  while (got < len) {
    int r = httpd_req_recv(req, &body[got], len - got);
    if (r == HTTPD_SOCK_ERR_TIMEOUT && ++retries < 5)
      continue;
    if (r <= 0) {
      httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "recv failed");
      return false;
    }
    got += r;
  }
  return true;
}

/// POST /api/zone, тело: "<индекс>,<hold с>\n<имя>\n<строка из 0 и 1 по всем клеткам>"
/// Индекс 255 = маска игнорирования.
static esp_err_t h_zone_post(httpd_req_t *req) {
  auto *self = static_cast<LD2450Zones *>(req->user_ctx);
  std::string body;
  if (!read_body(req, 2048, body))
    return ESP_FAIL;
  size_t nl1 = body.find('\n');
  size_t nl2 = nl1 == std::string::npos ? nl1 : body.find('\n', nl1 + 1);
  int idx = -1, hold = -1;
  if (nl2 == std::string::npos || sscanf(body.c_str(), "%d,%d", &idx, &hold) < 1) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad header");
    return ESP_FAIL;
  }
  if (!self->set_zone_cells(idx, hold, body.substr(nl1 + 1, nl2 - nl1 - 1), body.substr(nl2 + 1))) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad zone or size");
    return ESP_FAIL;
  }
  return send_json(req, "{\"ok\":1}");
}

/// POST /api/settings, тело: "<on_count>,<window>,<hold с>"
static esp_err_t h_settings_post(httpd_req_t *req) {
  auto *self = static_cast<LD2450Zones *>(req->user_ctx);
  std::string body;
  if (!read_body(req, 64, body))
    return ESP_FAIL;
  int on = 0, win = 0, hold = 0;
  if (sscanf(body.c_str(), "%d,%d,%d", &on, &win, &hold) != 3 || !self->set_settings(on, win, hold)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad settings");
    return ESP_FAIL;
  }
  return send_json(req, "{\"ok\":1}");
}

void LD2450Zones::start_server_() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = this->web_port_;
  cfg.ctrl_port = 32768 + 2 + (this->web_port_ % 1000);  // не пересекаться с другими httpd
  cfg.stack_size = 6144;
  cfg.max_uri_handlers = 8;
  cfg.max_open_sockets = 4;
  cfg.lru_purge_enable = true;
  cfg.recv_wait_timeout = 5;
  cfg.send_wait_timeout = 5;

  if (httpd_start(&this->server_, &cfg) != ESP_OK) {
    ESP_LOGW(TAG, "Could not start web UI on port %u (busy?), will retry", this->web_port_);
    this->server_ = nullptr;
    return;
  }

  struct Route {
    const char *uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t *);
  };
  const Route routes[] = {
      {"/", HTTP_GET, h_root},
      {"/api/state", HTTP_GET, h_state},
      {"/api/zones", HTTP_GET, h_zones},
      {"/api/zone", HTTP_POST, h_zone_post},
      {"/api/settings", HTTP_POST, h_settings_post},
  };
  for (const auto &r : routes) {
    httpd_uri_t u{};
    u.uri = r.uri;
    u.method = r.method;
    u.handler = r.handler;
    u.user_ctx = this;
    httpd_register_uri_handler(this->server_, &u);
  }
  ESP_LOGI(TAG, "Web UI: http://<device-ip>:%u/", this->web_port_);
}

}  // namespace ld2450_zones
}  // namespace esphome