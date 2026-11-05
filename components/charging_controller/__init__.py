from pathlib import Path
import gzip

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import ble_client, sensor, switch, time, text_sensor
from esphome.const import CONF_ID

DEPENDENCIES = ["esp32", "ble_client", "web_server", "time"]
AUTO_LOAD = ["json"]
ns = cg.esphome_ns.namespace("charging_controller")
Controller = ns.class_("Controller", cg.Component, ble_client.BLEClientNode)
CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(Controller),
    cv.Optional("daily_history_days", default=0): cv.All(cv.int_, cv.one_of(0, 180)),
    cv.Required("relay_id"): cv.use_id(switch.Switch),
    cv.Required("power_id"): cv.use_id(sensor.Sensor),
    cv.Required("clock_id"): cv.use_id(time.RealTimeClock),
    cv.Required("status_id"): cv.use_id(text_sensor.TextSensor),
}).extend(ble_client.BLE_CLIENT_SCHEMA).extend(cv.COMPONENT_SCHEMA)

async def to_code(config):
    cg.add_define("CHARGING_DAILY_HISTORY_DAYS", config["daily_history_days"])
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await ble_client.register_ble_node(var, config)
    for key in ["relay", "power", "clock", "status"]:
        ref = await cg.get_variable(config[f"{key}_id"])
        cg.add(getattr(var, f"set_{key}")(ref))
    # Reproducible, entirely local UI asset. The generated header lives in the
    # ESPHome build tree rather than modifying the source package during builds.
    asset = gzip.compress(Path(__file__).with_name("dashboard.html").read_bytes(), mtime=0)
    cg.add_global(cg.RawStatement("namespace esphome::charging_controller {\n"
        "const uint8_t DASHBOARD[] = {" + ",".join(map(str, asset)) + "};\n"
        f"const size_t DASHBOARD_SIZE = {len(asset)};\n" + "}"))
