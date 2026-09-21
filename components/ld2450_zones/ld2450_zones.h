#pragma once

#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"

#include <esp_http_server.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace esphome {
namespace ld2450_zones {

static const uint8_t MAX_ZONES = 8;
static const uint8_t MAX_TARGETS = 3;
static const size_t MAX_CELLS = 1024;  // синхронизировано с __init__.py
static const size_t MAX_CELL_BYTES = MAX_CELLS / 8;
static const size_t LABEL_MAX = 48;      // байт вместе с завершающим нулём
static const char *const BUILD_ID = "v2.3-i18n";
static const int IGNORE_IDX = 255;       // «номер зоны» для маски игнорирования в веб-API

/// Сколько миллисекунд прошло с момента then. Устойчиво к переполнению millis() (раз в ~49 суток), а если then
/// оказался чуть «в будущем» относительно now (кадр обработан после того, как now уже взяли), возвращает 0.
/// Обычное вычитание uint32_t в этом случае давало ~4 миллиарда и вызывало ложное «радар молчит».
inline uint32_t elapsed_ms(uint32_t now, uint32_t then) {
  int32_t d = (int32_t) (now - then);
  return d > 0 ? (uint32_t) d : 0;
}

/// Одна цель из кадра радара. Координаты в мм: x вправо/влево от оси, y вперёд от радара.
struct Target {
  int16_t x{0};
  int16_t y{0};
  int16_t speed{0};  // см/с
  uint16_t res{0};   // разрешение по дальности, мм
  bool valid{false};
};

/// Фильтр присутствия.
/// Вход: цель замечена минимум on_count раз в последних window кадрах (скользящее окно),
///       поэтому одиночные шумовые кадры не включают зону, а прерывистые цели включают.
/// Выход: ни одной цели в течение hold. Любое появление цели в занятой зоне продлевает удержание,
///        так что пропадание неподвижного человека на несколько кадров зону не гасит.
struct Presence {
  uint32_t hist{0};  // бит 0 = последний кадр, 1 = цель была
  uint32_t last_seen{0};
  bool on{false};
  uint8_t hits{0};  // сколько кадров из окна содержали цель (для отладки в UI)

  void update(bool seen, uint8_t on_count, uint8_t window, uint32_t now, uint32_t hold_ms) {
    this->hist = (this->hist << 1) | (seen ? 1u : 0u);
    const uint32_t mask = window >= 32 ? 0xFFFFFFFFu : ((1u << window) - 1u);
    this->hits = (uint8_t) __builtin_popcount(this->hist & mask);
    if (this->on) {
      if (seen) {
        this->last_seen = now;
      } else if (elapsed_ms(now, this->last_seen) > hold_ms) {
        this->on = false;
        this->hist = 0;
        this->hits = 0;
      }
    } else if (this->hits >= on_count) {
      this->on = true;
      this->last_seen = now;
    }
  }
};

struct Zone {
  std::string label;
  uint32_t hold_s{5};
  std::vector<uint8_t> cells;  // битовая маска клеток поля
  Presence pres;
  uint8_t count{0};

  binary_sensor::BinarySensor *presence_sensor{nullptr};
  sensor::Sensor *count_sensor{nullptr};
  text_sensor::TextSensor *label_sensor{nullptr};
  bool label_dirty{true};  // название нужно (пере)отправить в HA
  ESPPreferenceObject pref;
  bool save_pending{false};

  // что уже отправлено в HA
  int8_t pub_on{-1};
  int16_t pub_count{-1};
  uint32_t pub_count_ms{0};

  bool has_cell(size_t idx) const { return (this->cells[idx >> 3] >> (idx & 7)) & 1; }
  void set_cell(size_t idx, bool v) {
    if (v)
      this->cells[idx >> 3] |= (1 << (idx & 7));
    else
      this->cells[idx >> 3] &= ~(1 << (idx & 7));
  }
};

/// Формат хранения зоны во flash (NVS), версия 2: добавлено имя.
struct ZoneBlob {
  uint16_t magic;
  uint16_t cols;
  uint16_t rows;
  uint16_t hold_s;
  char label[LABEL_MAX];
  uint8_t cells[MAX_CELL_BYTES];
};

/// Настройки фильтра, меняются в веб-интерфейсе.
struct SettingsBlob {
  uint16_t magic;
  uint8_t on_count;
  uint8_t window;
  uint16_t hold_s;
};

class LD2450Zones : public Component, public uart::UARTDevice {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_web_port(uint16_t p) { this->web_port_ = p; }
  void set_multi_target(bool v) { this->multi_target_ = v; }
  void set_grid(int width_mm, int depth_mm, int cell_mm) {
    this->width_mm_ = width_mm;
    this->depth_mm_ = depth_mm;
    this->cell_mm_ = cell_mm;
    this->cols_ = width_mm / cell_mm;
    this->rows_ = depth_mm / cell_mm;
  }
  void set_enter_frames(uint8_t n) { this->on_count_ = n; }
  void set_enter_window(uint8_t n) { this->window_ = n; }
  void set_global_hold(uint32_t s) { this->global_hold_s_ = s; }
  void set_presence_sensor(binary_sensor::BinarySensor *s) { this->presence_sensor_ = s; }
  void set_target_count_sensor(sensor::Sensor *s) { this->target_count_sensor_ = s; }
  void add_zone(const std::string &label, uint32_t hold_s);
  void set_zone_presence(uint8_t i, binary_sensor::BinarySensor *s);
  void set_zone_count(uint8_t i, sensor::Sensor *s);
  void set_zone_label_sensor(uint8_t i, text_sensor::TextSensor *s);

  // Вызывается из потока HTTP-сервера
  std::string state_json();
  std::string zones_json();
  bool set_zone_cells(int idx, int hold_s, const std::string &name, const std::string &bits);
  bool set_settings(int on_count, int window, int hold_s);

 protected:
  void feed_(uint8_t b);
  void process_frame_(const uint8_t *f);
  void apply_(const Target *t, uint32_t now, bool real_frame);
  void publish_(uint32_t now, bool g_on, uint8_t total, const bool *zone_on, const uint8_t *zone_cnt);
  void send_cmd_(uint16_t cmd, const uint8_t *val, uint8_t vlen);
  void configure_radar_();
  void load_zones_();
  void load_settings_();
  void save_pending_();
  void publish_labels_();
  void start_server_();
  int cell_index_(int x, int y) const;

  // настройки
  uint16_t web_port_{8080};
  bool multi_target_{true};
  int width_mm_{6000}, depth_mm_{6000}, cell_mm_{200};
  int cols_{30}, rows_{30};
  uint8_t on_count_{3};
  uint8_t window_{10};
  uint32_t global_hold_s_{5};

  binary_sensor::BinarySensor *presence_sensor_{nullptr};
  sensor::Sensor *target_count_sensor_{nullptr};

  // состояние (защищено mtx_, т.к. читается из потока HTTP)
  std::mutex mtx_;
  std::vector<Zone> zones_;
  Zone ignore_;  // клетки, в которых цели игнорируются (вентилятор, занавеска)
  bool settings_pending_{false};
  ESPPreferenceObject settings_pref_;
  Target targets_[MAX_TARGETS];
  Presence global_;
  float fps_{0};
  uint32_t fps_t0_{0};
  uint16_t fps_frames_{0};
  uint32_t last_frame_ms_{0};

  // диагностика (пишет основной поток, читает HTTP; 32-битные значения, гонка безвредна)
  uint32_t frames_ok_{0}, frames_bad_{0}, skipped_{0};
  uint32_t gap_cur_{0}, gap_prev_{0}, lag_cur_{0}, lag_prev_{0};
  uint32_t diag_t0_{0}, last_loop_ms_{0};
  uint8_t counted_{0};
  uint8_t status_[MAX_TARGETS]{0, 0, 0};  // 0 пусто, 1 учтена, 2 вне области, 3 в зоне игнора

  // только основной поток
  uint8_t buf_[30];
  uint8_t pos_{0};
  uint32_t last_tick_ms_{0};
  int8_t pub_global_on_{-1};
  int16_t pub_total_{-1};
  uint32_t pub_total_ms_{0};

  httpd_handle_t server_{nullptr};
  uint32_t next_server_try_{0};
};

}  // namespace ld2450_zones
}  // namespace esphome