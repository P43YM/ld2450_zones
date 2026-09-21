"""ld2450_zones: обработка кадров LD2450 на ESP32 с зонами произвольной формы и веб-редактором."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor, esp32, sensor, text_sensor, uart
from esphome.const import (
    CONF_ID,
    DEVICE_CLASS_OCCUPANCY,
    STATE_CLASS_MEASUREMENT,
)

CODEOWNERS = []
DEPENDENCIES = ["uart"]
AUTO_LOAD = ["binary_sensor", "sensor", "text_sensor", "network"]

CONF_ZONES = "zones"
CONF_LABEL = "label"
CONF_HOLD = "hold"
CONF_COUNT = "count"
CONF_LABEL_SENSOR = "label_sensor"
CONF_PRESENCE = "presence"
CONF_TARGET_COUNT = "target_count"
CONF_ENTER_FRAMES = "enter_frames"
CONF_ENTER_WINDOW = "enter_window"
CONF_WEB_PORT = "web_port"
CONF_MULTI_TARGET = "multi_target"
CONF_AREA_WIDTH = "area_width"
CONF_AREA_DEPTH = "area_depth"
CONF_CELL_SIZE = "cell_size"

MAX_ZONES = 8
MAX_CELLS = 1024  # должно совпадать с MAX_CELLS в ld2450_zones.h

ld2450_zones_ns = cg.esphome_ns.namespace("ld2450_zones")
LD2450Zones = ld2450_zones_ns.class_("LD2450Zones", cg.Component, uart.UARTDevice)

ZONE_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_LABEL): cv.string,
        cv.Optional(CONF_HOLD, default="5s"): cv.positive_time_period_seconds,
        cv.Optional(CONF_PRESENCE): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_OCCUPANCY
        ),
        cv.Optional(CONF_COUNT): sensor.sensor_schema(
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
            icon="mdi:account-multiple",
        ),
        # текстовый сенсор с текущим названием зоны: обновляется в HA сразу после переименования в веб-интерфейсе
        cv.Optional(CONF_LABEL_SENSOR): text_sensor.text_sensor_schema(icon="mdi:tag-text"),
    }
)


def _validate_grid(config):
    w, d, c = config[CONF_AREA_WIDTH], config[CONF_AREA_DEPTH], config[CONF_CELL_SIZE]
    if w % c or d % c:
        raise cv.Invalid("area_width и area_depth должны делиться на cell_size нацело")
    if (w // c) * (d // c) > MAX_CELLS:
        raise cv.Invalid(
            f"Слишком много клеток: {(w // c) * (d // c)} (максимум {MAX_CELLS}). "
            "Увеличьте cell_size или уменьшите область."
        )
    if config[CONF_ENTER_FRAMES] > config[CONF_ENTER_WINDOW]:
        raise cv.Invalid("enter_frames не может быть больше enter_window")
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(LD2450Zones),
            cv.Optional(CONF_WEB_PORT, default=8080): cv.port,
            cv.Optional(CONF_MULTI_TARGET, default=True): cv.boolean,
            # поле для зон: ширина симметрична относительно радара, глубина от радара вперёд
            cv.Optional(CONF_AREA_WIDTH, default=6000): cv.int_range(min=1000, max=10000),
            cv.Optional(CONF_AREA_DEPTH, default=6000): cv.int_range(min=1000, max=8000),
            cv.Optional(CONF_CELL_SIZE, default=200): cv.int_range(min=50, max=1000),
            # зона включается, если цель замечена минимум enter_frames раз в последних enter_window кадрах
            # (радар шлёт ~10 кадров/с). Значения можно менять потом в веб-интерфейсе.
            cv.Optional(CONF_ENTER_FRAMES, default=3): cv.int_range(min=1, max=32),
            cv.Optional(CONF_ENTER_WINDOW, default=10): cv.int_range(min=1, max=32),
            # общее присутствие (любая цель в поле зрения)
            cv.Optional(CONF_HOLD, default="5s"): cv.positive_time_period_seconds,
            cv.Optional(CONF_PRESENCE): binary_sensor.binary_sensor_schema(
                device_class=DEVICE_CLASS_OCCUPANCY
            ),
            cv.Optional(CONF_TARGET_COUNT): sensor.sensor_schema(
                accuracy_decimals=0,
                state_class=STATE_CLASS_MEASUREMENT,
                icon="mdi:account-multiple",
            ),
            cv.Optional(CONF_ZONES, default=[]): cv.All(
                cv.ensure_list(ZONE_SCHEMA), cv.Length(max=MAX_ZONES)
            ),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(uart.UART_DEVICE_SCHEMA),
    _validate_grid,
    cv.only_with_framework("esp-idf"),
)

FINAL_VALIDATE_SCHEMA = uart.final_validate_device_schema(
    "ld2450_zones", baud_rate=256000, require_rx=True, require_tx=True
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)

    # веб-редактор работает на esp_http_server из ESP-IDF
    if hasattr(esp32, "include_builtin_idf_component"):
        esp32.include_builtin_idf_component("esp_http_server")

    cg.add(var.set_web_port(config[CONF_WEB_PORT]))
    cg.add(var.set_multi_target(config[CONF_MULTI_TARGET]))
    cg.add(
        var.set_grid(
            config[CONF_AREA_WIDTH], config[CONF_AREA_DEPTH], config[CONF_CELL_SIZE]
        )
    )
    cg.add(var.set_enter_frames(config[CONF_ENTER_FRAMES]))
    cg.add(var.set_enter_window(config[CONF_ENTER_WINDOW]))
    cg.add(var.set_global_hold(int(config[CONF_HOLD].total_seconds)))

    if CONF_PRESENCE in config:
        s = await binary_sensor.new_binary_sensor(config[CONF_PRESENCE])
        cg.add(var.set_presence_sensor(s))
    if CONF_TARGET_COUNT in config:
        s = await sensor.new_sensor(config[CONF_TARGET_COUNT])
        cg.add(var.set_target_count_sensor(s))

    for i, zone in enumerate(config[CONF_ZONES]):
        cg.add(var.add_zone(zone[CONF_LABEL], int(zone[CONF_HOLD].total_seconds)))
        if CONF_PRESENCE in zone:
            s = await binary_sensor.new_binary_sensor(zone[CONF_PRESENCE])
            cg.add(var.set_zone_presence(i, s))
        if CONF_COUNT in zone:
            s = await sensor.new_sensor(zone[CONF_COUNT])
            cg.add(var.set_zone_count(i, s))
        if CONF_LABEL_SENSOR in zone:
            s = await text_sensor.new_text_sensor(zone[CONF_LABEL_SENSOR])
            cg.add(var.set_zone_label_sensor(i, s))