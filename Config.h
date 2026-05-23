#ifndef CONFIG_H
#define CONFIG_H

// ============================================================
// WLAN & MQTT – HIER ANPASSEN
// ============================================================
#define WIFI_SSID       "DEIN_WLAN_NAME"
#define WIFI_PASSWORD   "DEIN_WLAN_PASSWORT"
#define MQTT_BROKER     "192.168.1.100"
#define MQTT_PORT       1883
#define MQTT_USER       ""
#define MQTT_PASSWORD   ""
#define MQTT_CLIENT_ID  "pilzzucht"

// ============================================================
// PIN-BELEGUNG
// ============================================================
// I2C fuer SCD41
#define PIN_SDA         21
#define PIN_SCL         22

// Relais-Ausgaenge
#define RELAY_ACTIVE_LOW  true   // true fuer Low-Level-Trigger-Relais
#define PIN_HEATER      32
#define PIN_COOLER      33
#define PIN_FAE         25       // Frischluftaustausch
#define PIN_HUMIDIFIER  26       // Luftbefeuchter
#define PIN_FAN         14       // Umluftluefter

// ============================================================
// MQTT TOPICS
// ============================================================
#define MQTT_BASE           "pilzzucht"
// Sensordaten (publish)
#define TOPIC_SENSOR_TEMP   MQTT_BASE "/sensor/temperatur"
#define TOPIC_SENSOR_HUM    MQTT_BASE "/sensor/feuchte"
#define TOPIC_SENSOR_CO2    MQTT_BASE "/sensor/co2"
// Ausgangs-Status (publish)
#define TOPIC_OUT_HEATER    MQTT_BASE "/ausgang/heizung"
#define TOPIC_OUT_COOLER    MQTT_BASE "/ausgang/kuehlung"
#define TOPIC_OUT_FAE       MQTT_BASE "/ausgang/luftaustausch"
#define TOPIC_OUT_HUM       MQTT_BASE "/ausgang/befeuchter"
#define TOPIC_OUT_FAN       MQTT_BASE "/ausgang/luefter"
// Sollwerte (subscribe) – Payload: Zahlenwert als String
#define TOPIC_SET_TEMP      MQTT_BASE "/sollwert/temperatur"
#define TOPIC_SET_HUM       MQTT_BASE "/sollwert/feuchte"
#define TOPIC_SET_CO2       MQTT_BASE "/sollwert/co2"
#define TOPIC_SET_TEMP_HY   MQTT_BASE "/sollwert/temp_hysterese"
#define TOPIC_SET_HUM_HY    MQTT_BASE "/sollwert/feuchte_hysterese"
#define TOPIC_SET_CO2_HY    MQTT_BASE "/sollwert/co2_hysterese"
#define TOPIC_SET_FAN_ON    MQTT_BASE "/sollwert/luefter_ein_sek"
#define TOPIC_SET_FAN_OFF   MQTT_BASE "/sollwert/luefter_aus_sek"
#define TOPIC_SET_FAN_INTV  MQTT_BASE "/sollwert/luefter_intervall"
#define TOPIC_SET_FAN_CTRL  MQTT_BASE "/sollwert/luefter_regelung"
// Gesamtstatus (publish, JSON)
#define TOPIC_STATUS        MQTT_BASE "/status"

// ============================================================
// ZEITINTERVALLE (ms)
// ============================================================
#define SENSOR_READ_MS      5000
#define DISPLAY_UPDATE_MS   1000
#define MQTT_PUBLISH_MS     10000
#define MQTT_RECONNECT_MS   5000
#define CONTROL_RUN_MS      2000
#define TOUCH_DEBOUNCE_MS   300
#define MIN_SWITCH_TIME_MS  10000
#define WIFI_RECONNECT_MS   10000

// ============================================================
// STANDARD-SOLLWERTE
// ============================================================
#define DEF_TEMP_SP         20.0f
#define DEF_TEMP_HY         0.5f
#define DEF_HUM_SP          90.0f
#define DEF_HUM_HY          3.0f
#define DEF_CO2_SP          800
#define DEF_CO2_HY          100
#define DEF_FAN_ON_SEC      300
#define DEF_FAN_OFF_SEC     1800

// ============================================================
// DISPLAY-FARBEN (RGB565)
// ============================================================
#define CLR_BG          0x0000
#define CLR_TEXT         0xFFFF
#define CLR_HEADER_BG   0x000F
#define CLR_GOOD         0x07E0
#define CLR_WARN         0xFFE0
#define CLR_BAD          0xF800
#define CLR_INACTIVE     0x4208
#define CLR_ACTIVE       0x07FF
#define CLR_BTN          0x2945
#define CLR_BTN_HL       0x4A69
#define CLR_SETPOINT     0xBDF7

// ============================================================
// DATENSTRUKTUREN
// ============================================================
struct Settings {
  float    tempSetpoint;
  float    tempHysteresis;
  float    humSetpoint;
  float    humHysteresis;
  uint16_t co2Setpoint;
  uint16_t co2Hysteresis;
  uint32_t fanOnSec;
  uint32_t fanOffSec;
  bool     fanIntervalEnabled;
  bool     fanOnControl;
};

enum ScreenID { SCR_MAIN = 0, SCR_SETTINGS, SCR_FAN };

#endif
