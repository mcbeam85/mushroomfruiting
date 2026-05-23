/*
 * Pilzzucht-Fruchtkammer-Steuerung
 * ESP32 + SCD41 + ILI9341/ILI9488 Touchscreen + MQTT
 *
 * Regelung: Bang-Bang mit Hysterese
 * Koordination: FAE vor Befeuchtung (spart einen Befeuchtungszyklus)
 *
 * Benoetigte Bibliotheken:
 *   - Sensirion I2C SCD4x          (Sensirion)
 *   - TFT_eSPI                     (Bodmer)
 *   - PubSubClient                 (Nick O'Leary)
 *   - ArduinoJson                  (Benoit Blanchon, v7+)
 *
 * TFT_eSPI User_Setup.h konfigurieren:
 *   #define ILI9488_DRIVER           // oder ILI9341_DRIVER fuer 320x240
 *   #define TFT_MOSI  23
 *   #define TFT_MISO  19
 *   #define TFT_SCLK  18
 *   #define TFT_CS     5
 *   #define TFT_DC     4
 *   #define TFT_RST    2
 *   #define TOUCH_CS   15
 *   #define SPI_FREQUENCY       40000000
 *   #define SPI_TOUCH_FREQUENCY  2500000
 */

#include <Wire.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <SensirionI2CScd4x.h>
#include <TFT_eSPI.h>
#include <Preferences.h>
#include "Config.h"

// ============================================================
//  GLOBALE OBJEKTE
// ============================================================
SensirionI2CScd4x scd41;
TFT_eSPI          tft = TFT_eSPI();
WiFiClient        espClient;
PubSubClient      mqtt(espClient);
Preferences       prefs;

// ============================================================
//  GLOBALER ZUSTAND
// ============================================================
Settings cfg;

// Messwerte
float    curTemp = 0.0f;
float    curHum  = 0.0f;
uint16_t curCO2  = 0;
bool     sensorOk = false;

// Ausgangs-Zustaende
bool outState[5] = {false, false, false, false, false};
// 0=Heizung, 1=Kuehlung, 2=FAE, 3=Befeuchter, 4=Luefter

const uint8_t outPins[5] = {PIN_HEATER, PIN_COOLER, PIN_FAE, PIN_HUMIDIFIER, PIN_FAN};
unsigned long outLastSwitch[5] = {0, 0, 0, 0, 0};

// Koordination FAE → Befeuchter
bool     faeForCO2Active    = false;
bool     humWaitingForFAE   = false;
unsigned long faeStoppedAt  = 0;

// Luefter-Intervall
unsigned long fanCycleStart = 0;

// Timer
unsigned long tSensor   = 0;
unsigned long tDisplay  = 0;
unsigned long tMqtt     = 0;
unsigned long tMqttConn = 0;
unsigned long tControl  = 0;
unsigned long tTouch    = 0;
unsigned long tWifi     = 0;

// Display
ScreenID curScreen         = SCR_MAIN;
bool     screenRedraw      = true;
int      scrW, scrH;

// Vorherige Display-Werte (fuer partielle Updates)
float    prevTemp = -999;
float    prevHum  = -999;
int      prevCO2  = -1;
bool     prevOut[5] = {false, false, false, false, false};

// Touch-Kalibrierung (Standardwerte – ggf. anpassen)
uint16_t touchCal[5] = {395, 3462, 272, 3431, 3};

// ============================================================
//  NVS EINSTELLUNGEN
// ============================================================
void loadSettings() {
  prefs.begin("mushroom", true);
  cfg.tempSetpoint      = prefs.getFloat("tempSP",   DEF_TEMP_SP);
  cfg.tempHysteresis    = prefs.getFloat("tempHY",   DEF_TEMP_HY);
  cfg.humSetpoint       = prefs.getFloat("humSP",    DEF_HUM_SP);
  cfg.humHysteresis     = prefs.getFloat("humHY",    DEF_HUM_HY);
  cfg.co2Setpoint       = prefs.getUShort("co2SP",   DEF_CO2_SP);
  cfg.co2Hysteresis     = prefs.getUShort("co2HY",   DEF_CO2_HY);
  cfg.fanOnSec          = prefs.getULong("fanOn",    DEF_FAN_ON_SEC);
  cfg.fanOffSec         = prefs.getULong("fanOff",   DEF_FAN_OFF_SEC);
  cfg.fanIntervalEnabled = prefs.getBool("fanIntv",  true);
  cfg.fanOnControl      = prefs.getBool("fanCtrl",   true);
  prefs.end();
}

void saveSettings() {
  prefs.begin("mushroom", false);
  prefs.putFloat("tempSP",  cfg.tempSetpoint);
  prefs.putFloat("tempHY",  cfg.tempHysteresis);
  prefs.putFloat("humSP",   cfg.humSetpoint);
  prefs.putFloat("humHY",   cfg.humHysteresis);
  prefs.putUShort("co2SP",  cfg.co2Setpoint);
  prefs.putUShort("co2HY",  cfg.co2Hysteresis);
  prefs.putULong("fanOn",   cfg.fanOnSec);
  prefs.putULong("fanOff",  cfg.fanOffSec);
  prefs.putBool("fanIntv",  cfg.fanIntervalEnabled);
  prefs.putBool("fanCtrl",  cfg.fanOnControl);
  prefs.end();
}

// ============================================================
//  SENSOR (SCD41)
// ============================================================
void initSensor() {
  Wire.begin(PIN_SDA, PIN_SCL);
  scd41.begin(Wire);
  scd41.stopPeriodicMeasurement();
  delay(500);
  scd41.startPeriodicMeasurement();
}

void readSensor() {
  uint16_t co2Raw;
  float tempRaw, humRaw;
  bool ready = false;
  scd41.getDataReadyFlag(ready);
  if (!ready) return;
  uint16_t err = scd41.readMeasurement(co2Raw, tempRaw, humRaw);
  if (err || co2Raw == 0) return;
  curCO2  = co2Raw;
  curTemp = tempRaw;
  curHum  = humRaw;
  sensorOk = true;
}

// ============================================================
//  AUSGANGS-STEUERUNG
// ============================================================
void setOutput(uint8_t idx, bool on) {
  if (idx >= 5) return;
  if (outState[idx] == on) return;
  unsigned long now = millis();
  if (now - outLastSwitch[idx] < MIN_SWITCH_TIME_MS) return;
  outState[idx] = on;
  outLastSwitch[idx] = now;
  bool pinLevel = RELAY_ACTIVE_LOW ? !on : on;
  digitalWrite(outPins[idx], pinLevel);
}

void initOutputs() {
  for (int i = 0; i < 5; i++) {
    pinMode(outPins[i], OUTPUT);
    bool pinLevel = RELAY_ACTIVE_LOW ? HIGH : LOW;
    digitalWrite(outPins[i], pinLevel);
  }
}

// ============================================================
//  REGELUNG (Bang-Bang mit Hysterese + Koordination)
// ============================================================
void runControl() {
  if (!sensorOk) return;

  // --- Temperatur ---
  if (curTemp < cfg.tempSetpoint - cfg.tempHysteresis) {
    setOutput(0, true);   // Heizung EIN
    setOutput(1, false);  // Kuehlung AUS
  } else if (curTemp > cfg.tempSetpoint + cfg.tempHysteresis) {
    setOutput(0, false);
    setOutput(1, true);
  } else {
    setOutput(0, false);
    setOutput(1, false);
  }

  // --- CO2 & Feuchte (koordiniert) ---
  bool co2High = curCO2 > cfg.co2Setpoint + cfg.co2Hysteresis;
  bool co2Ok   = curCO2 <= cfg.co2Setpoint - cfg.co2Hysteresis;
  bool humLow  = curHum < cfg.humSetpoint - cfg.humHysteresis;
  bool humOk   = curHum >= cfg.humSetpoint + cfg.humHysteresis;

  // CO2 zu hoch → FAE einschalten
  if (co2High && !faeForCO2Active) {
    faeForCO2Active = true;
    setOutput(2, true);  // FAE EIN
    if (cfg.fanOnControl) setOutput(4, true);
  }

  // CO2 wieder im Sollbereich → FAE aus
  if (faeForCO2Active && co2Ok) {
    faeForCO2Active = false;
    setOutput(2, false);
    faeStoppedAt = millis();
    if (humLow) humWaitingForFAE = true;
  }

  // Feuchte zu niedrig
  if (humLow && !humWaitingForFAE && !faeForCO2Active) {
    setOutput(3, true);  // Befeuchter EIN
    if (cfg.fanOnControl) setOutput(4, true);
  }

  // Koordination: nach FAE-Stopp kurz warten, dann befeuchten
  if (humWaitingForFAE && millis() - faeStoppedAt > 2000) {
    humWaitingForFAE = false;
    if (humLow) {
      setOutput(3, true);
      if (cfg.fanOnControl) setOutput(4, true);
    }
  }

  // Feuchte wieder OK → Befeuchter aus
  if (humOk || (!humLow && outState[3])) {
    if (curHum >= cfg.humSetpoint) {
      setOutput(3, false);
    }
  }

  // FAE nur fuer CO2: wenn CO2 OK und FAE nicht benoetigt
  if (!faeForCO2Active && outState[2]) {
    setOutput(2, false);
  }
}

// ============================================================
//  LUEFTER-INTERVALL
// ============================================================
void handleFanInterval() {
  if (!cfg.fanIntervalEnabled) return;
  unsigned long now = millis();
  unsigned long elapsed = (now - fanCycleStart) / 1000;

  if (outState[4] && elapsed >= cfg.fanOnSec) {
    // Nur ausschalten wenn kein Regelvorgang aktiv
    if (!faeForCO2Active && !outState[3]) {
      setOutput(4, false);
      fanCycleStart = now;
    }
  } else if (!outState[4] && elapsed >= cfg.fanOffSec) {
    setOutput(4, true);
    fanCycleStart = now;
  }
}

// ============================================================
//  DISPLAY – HILFSFUNKTIONEN
// ============================================================
void drawButton(int x, int y, int w, int h, const char* label, uint16_t bg, uint16_t fg) {
  tft.fillRoundRect(x, y, w, h, 6, bg);
  tft.setTextColor(fg, bg);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(label, x + w / 2, y + h / 2, 2);
}

void drawOutputIndicator(int x, int y, int w, const char* label, bool on) {
  uint16_t bg = on ? CLR_ACTIVE : CLR_INACTIVE;
  uint16_t fg = on ? CLR_BG : CLR_TEXT;
  tft.fillRoundRect(x, y, w, 30, 4, bg);
  tft.setTextColor(fg, bg);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(label, x + w / 2, y + 10, 1);
  tft.drawString(on ? "EIN" : "AUS", x + w / 2, y + 22, 1);
}

// ============================================================
//  DISPLAY – HAUPTBILDSCHIRM
// ============================================================
void drawMainScreen() {
  tft.fillScreen(CLR_BG);

  // Header
  tft.fillRect(0, 0, scrW, 32, CLR_HEADER_BG);
  tft.setTextColor(CLR_TEXT, CLR_HEADER_BG);
  tft.setTextDatum(ML_DATUM);
  tft.drawString("PILZZUCHT-STEUERUNG", 10, 16, 2);

  // Status-Indikatoren
  tft.setTextDatum(MR_DATUM);
  tft.setTextColor(WiFi.status() == WL_CONNECTED ? CLR_GOOD : CLR_BAD, CLR_HEADER_BG);
  tft.drawString("WiFi", scrW - 60, 16, 1);
  tft.setTextColor(mqtt.connected() ? CLR_GOOD : CLR_BAD, CLR_HEADER_BG);
  tft.drawString("MQTT", scrW - 10, 16, 1);

  // Spalten-Header
  tft.setTextColor(CLR_SETPOINT, CLR_BG);
  tft.setTextDatum(ML_DATUM);
  int yBase = 40;
  tft.drawString("Messwert", 160, yBase, 1);
  tft.drawString("Sollwert", 310, yBase, 1);

  // Zeilen: Temperatur, Feuchte, CO2
  const char* labels[] = {"TEMPERATUR", "FEUCHTE", "CO2"};
  int rowY[] = {58, 118, 178};

  for (int i = 0; i < 3; i++) {
    tft.setTextColor(CLR_TEXT, CLR_BG);
    tft.setTextDatum(ML_DATUM);
    tft.drawString(labels[i], 10, rowY[i] + 12, 2);
    tft.drawFastHLine(10, rowY[i] + 38, scrW - 20, CLR_INACTIVE);
  }

  // Messwerte + Sollwerte zeichnen
  updateMainValues(true);

  // Ausgangs-Indikatoren
  int outY = 230;
  int outW = (scrW - 60) / 5;
  const char* outLabels[] = {"Heizung", "Kuehl.", "FAE", "Befeuch.", "Luefter"};
  for (int i = 0; i < 5; i++) {
    drawOutputIndicator(10 + i * (outW + 8), outY, outW, outLabels[i], outState[i]);
  }

  // Einstellungen-Button
  drawButton(scrW / 2 - 80, scrH - 42, 160, 34, "EINSTELLUNGEN", CLR_BTN, CLR_TEXT);
}

void updateMainValues(bool force) {
  char buf[20];
  int rowY[] = {58, 118, 178};

  // Temperatur
  if (force || curTemp != prevTemp) {
    tft.setTextDatum(ML_DATUM);
    tft.fillRect(160, rowY[0], 130, 30, CLR_BG);
    uint16_t col = CLR_GOOD;
    if (curTemp < cfg.tempSetpoint - cfg.tempHysteresis || curTemp > cfg.tempSetpoint + cfg.tempHysteresis)
      col = CLR_WARN;
    tft.setTextColor(col, CLR_BG);
    snprintf(buf, sizeof(buf), "%.1f C", curTemp);
    tft.drawString(buf, 160, rowY[0] + 14, 4);

    tft.fillRect(310, rowY[0], 120, 30, CLR_BG);
    tft.setTextColor(CLR_SETPOINT, CLR_BG);
    snprintf(buf, sizeof(buf), "%.1f C", cfg.tempSetpoint);
    tft.drawString(buf, 310, rowY[0] + 14, 2);
    prevTemp = curTemp;
  }

  // Feuchte
  if (force || curHum != prevHum) {
    tft.setTextDatum(ML_DATUM);
    tft.fillRect(160, rowY[1], 130, 30, CLR_BG);
    uint16_t col = CLR_GOOD;
    if (curHum < cfg.humSetpoint - cfg.humHysteresis)
      col = CLR_WARN;
    tft.setTextColor(col, CLR_BG);
    snprintf(buf, sizeof(buf), "%.1f %%", curHum);
    tft.drawString(buf, 160, rowY[1] + 14, 4);

    tft.fillRect(310, rowY[1], 120, 30, CLR_BG);
    tft.setTextColor(CLR_SETPOINT, CLR_BG);
    snprintf(buf, sizeof(buf), "%.1f %%", cfg.humSetpoint);
    tft.drawString(buf, 310, rowY[1] + 14, 2);
    prevHum = curHum;
  }

  // CO2
  if (force || (int)curCO2 != prevCO2) {
    tft.setTextDatum(ML_DATUM);
    tft.fillRect(160, rowY[2], 130, 30, CLR_BG);
    uint16_t col = CLR_GOOD;
    if (curCO2 > cfg.co2Setpoint + cfg.co2Hysteresis)
      col = CLR_WARN;
    if (curCO2 > cfg.co2Setpoint + cfg.co2Hysteresis * 2)
      col = CLR_BAD;
    tft.setTextColor(col, CLR_BG);
    snprintf(buf, sizeof(buf), "%u ppm", curCO2);
    tft.drawString(buf, 160, rowY[2] + 14, 4);

    tft.fillRect(310, rowY[2], 120, 30, CLR_BG);
    tft.setTextColor(CLR_SETPOINT, CLR_BG);
    snprintf(buf, sizeof(buf), "%u ppm", cfg.co2Setpoint);
    tft.drawString(buf, 310, rowY[2] + 14, 2);
    prevCO2 = curCO2;
  }

  // Ausgangs-Indikatoren (nur bei Aenderung)
  int outY = 230;
  int outW = (scrW - 60) / 5;
  const char* outLabels[] = {"Heizung", "Kuehl.", "FAE", "Befeuch.", "Luefter"};
  for (int i = 0; i < 5; i++) {
    if (force || outState[i] != prevOut[i]) {
      drawOutputIndicator(10 + i * (outW + 8), outY, outW, outLabels[i], outState[i]);
      prevOut[i] = outState[i];
    }
  }

  // WiFi/MQTT Status im Header aktualisieren
  tft.setTextDatum(MR_DATUM);
  tft.fillRect(scrW - 90, 4, 85, 24, CLR_HEADER_BG);
  tft.setTextColor(WiFi.status() == WL_CONNECTED ? CLR_GOOD : CLR_BAD, CLR_HEADER_BG);
  tft.drawString("WiFi", scrW - 60, 16, 1);
  tft.setTextColor(mqtt.connected() ? CLR_GOOD : CLR_BAD, CLR_HEADER_BG);
  tft.drawString("MQTT", scrW - 10, 16, 1);
}

// ============================================================
//  DISPLAY – EINSTELLUNGEN
// ============================================================
void drawSettingsScreen() {
  tft.fillScreen(CLR_BG);
  tft.fillRect(0, 0, scrW, 32, CLR_HEADER_BG);
  tft.setTextColor(CLR_TEXT, CLR_HEADER_BG);
  tft.setTextDatum(ML_DATUM);
  tft.drawString("EINSTELLUNGEN", 10, 16, 2);

  drawButton(scrW - 90, 4, 80, 24, "ZURUECK", CLR_BTN, CLR_TEXT);

  drawSettingsValues();

  // Luefter-Einstellungen Button
  drawButton(scrW / 2 - 90, scrH - 42, 180, 34, "LUEFTER-EINST.", CLR_BTN, CLR_TEXT);
}

void drawSettingsValues() {
  char buf[30];
  int y0 = 50;
  int rowH = 52;

  struct SettingsRow {
    const char* label;
    char valBuf[16];
    char hystBuf[16];
  };

  // Temperatur
  tft.setTextColor(CLR_TEXT, CLR_BG);
  tft.setTextDatum(ML_DATUM);
  tft.drawString("Temperatur:", 10, y0 + 12, 2);
  drawButton(155, y0, 30, 28, "-", CLR_BTN, CLR_TEXT);
  tft.fillRect(190, y0, 80, 28, CLR_BG);
  tft.setTextColor(CLR_TEXT, CLR_BG);
  tft.setTextDatum(MC_DATUM);
  snprintf(buf, sizeof(buf), "%.1f C", cfg.tempSetpoint);
  tft.drawString(buf, 230, y0 + 14, 2);
  drawButton(275, y0, 30, 28, "+", CLR_BTN, CLR_TEXT);
  tft.setTextColor(CLR_SETPOINT, CLR_BG);
  tft.setTextDatum(ML_DATUM);
  tft.drawString("Hyst:", 320, y0 + 4, 1);
  drawButton(320, y0 + 16, 24, 22, "-", CLR_BTN, CLR_TEXT);
  tft.fillRect(348, y0 + 16, 50, 22, CLR_BG);
  tft.setTextColor(CLR_SETPOINT, CLR_BG);
  tft.setTextDatum(MC_DATUM);
  snprintf(buf, sizeof(buf), "%.1f", cfg.tempHysteresis);
  tft.drawString(buf, 373, y0 + 27, 1);
  drawButton(402, y0 + 16, 24, 22, "+", CLR_BTN, CLR_TEXT);

  // Feuchte
  int y1 = y0 + rowH;
  tft.setTextColor(CLR_TEXT, CLR_BG);
  tft.setTextDatum(ML_DATUM);
  tft.drawString("Feuchte:", 10, y1 + 12, 2);
  drawButton(155, y1, 30, 28, "-", CLR_BTN, CLR_TEXT);
  tft.fillRect(190, y1, 80, 28, CLR_BG);
  tft.setTextColor(CLR_TEXT, CLR_BG);
  tft.setTextDatum(MC_DATUM);
  snprintf(buf, sizeof(buf), "%.1f %%", cfg.humSetpoint);
  tft.drawString(buf, 230, y1 + 14, 2);
  drawButton(275, y1, 30, 28, "+", CLR_BTN, CLR_TEXT);
  tft.setTextColor(CLR_SETPOINT, CLR_BG);
  tft.setTextDatum(ML_DATUM);
  tft.drawString("Hyst:", 320, y1 + 4, 1);
  drawButton(320, y1 + 16, 24, 22, "-", CLR_BTN, CLR_TEXT);
  tft.fillRect(348, y1 + 16, 50, 22, CLR_BG);
  tft.setTextColor(CLR_SETPOINT, CLR_BG);
  tft.setTextDatum(MC_DATUM);
  snprintf(buf, sizeof(buf), "%.1f", cfg.humHysteresis);
  tft.drawString(buf, 373, y1 + 27, 1);
  drawButton(402, y1 + 16, 24, 22, "+", CLR_BTN, CLR_TEXT);

  // CO2
  int y2 = y0 + rowH * 2;
  tft.setTextColor(CLR_TEXT, CLR_BG);
  tft.setTextDatum(ML_DATUM);
  tft.drawString("CO2:", 10, y2 + 12, 2);
  drawButton(155, y2, 30, 28, "-", CLR_BTN, CLR_TEXT);
  tft.fillRect(190, y2, 80, 28, CLR_BG);
  tft.setTextColor(CLR_TEXT, CLR_BG);
  tft.setTextDatum(MC_DATUM);
  snprintf(buf, sizeof(buf), "%u ppm", cfg.co2Setpoint);
  tft.drawString(buf, 230, y2 + 14, 2);
  drawButton(275, y2, 30, 28, "+", CLR_BTN, CLR_TEXT);
  tft.setTextColor(CLR_SETPOINT, CLR_BG);
  tft.setTextDatum(ML_DATUM);
  tft.drawString("Hyst:", 320, y2 + 4, 1);
  drawButton(320, y2 + 16, 24, 22, "-", CLR_BTN, CLR_TEXT);
  tft.fillRect(348, y2 + 16, 50, 22, CLR_BG);
  tft.setTextColor(CLR_SETPOINT, CLR_BG);
  tft.setTextDatum(MC_DATUM);
  snprintf(buf, sizeof(buf), "%u", cfg.co2Hysteresis);
  tft.drawString(buf, 373, y2 + 27, 1);
  drawButton(402, y2 + 16, 24, 22, "+", CLR_BTN, CLR_TEXT);
}

// ============================================================
//  DISPLAY – LUEFTER-EINSTELLUNGEN
// ============================================================
void drawFanScreen() {
  tft.fillScreen(CLR_BG);
  tft.fillRect(0, 0, scrW, 32, CLR_HEADER_BG);
  tft.setTextColor(CLR_TEXT, CLR_HEADER_BG);
  tft.setTextDatum(ML_DATUM);
  tft.drawString("LUEFTER-EINSTELLUNGEN", 10, 16, 2);

  drawButton(scrW - 90, 4, 80, 24, "ZURUECK", CLR_BTN, CLR_TEXT);

  drawFanValues();
}

void drawFanValues() {
  char buf[20];
  int y0 = 55;

  // EIN-Dauer
  tft.setTextColor(CLR_TEXT, CLR_BG);
  tft.setTextDatum(ML_DATUM);
  tft.drawString("Intervall EIN:", 10, y0 + 12, 2);
  drawButton(190, y0, 30, 28, "-", CLR_BTN, CLR_TEXT);
  tft.fillRect(225, y0, 100, 28, CLR_BG);
  tft.setTextColor(CLR_TEXT, CLR_BG);
  tft.setTextDatum(MC_DATUM);
  snprintf(buf, sizeof(buf), "%lu:%02lu min", cfg.fanOnSec / 60, cfg.fanOnSec % 60);
  tft.drawString(buf, 275, y0 + 14, 2);
  drawButton(330, y0, 30, 28, "+", CLR_BTN, CLR_TEXT);

  // AUS-Dauer
  int y1 = y0 + 50;
  tft.setTextColor(CLR_TEXT, CLR_BG);
  tft.setTextDatum(ML_DATUM);
  tft.drawString("Intervall AUS:", 10, y1 + 12, 2);
  drawButton(190, y1, 30, 28, "-", CLR_BTN, CLR_TEXT);
  tft.fillRect(225, y1, 100, 28, CLR_BG);
  tft.setTextColor(CLR_TEXT, CLR_BG);
  tft.setTextDatum(MC_DATUM);
  snprintf(buf, sizeof(buf), "%lu:%02lu min", cfg.fanOffSec / 60, cfg.fanOffSec % 60);
  tft.drawString(buf, 275, y1 + 14, 2);
  drawButton(330, y1, 30, 28, "+", CLR_BTN, CLR_TEXT);

  // Intervall-Modus Toggle
  int y2 = y1 + 55;
  tft.setTextColor(CLR_TEXT, CLR_BG);
  tft.setTextDatum(ML_DATUM);
  tft.drawString("Intervall-Modus:", 10, y2 + 12, 2);
  drawButton(210, y2, 80, 28, cfg.fanIntervalEnabled ? "EIN" : "AUS",
             cfg.fanIntervalEnabled ? CLR_ACTIVE : CLR_INACTIVE,
             cfg.fanIntervalEnabled ? CLR_BG : CLR_TEXT);

  // Bei Regelung aktiv Toggle
  int y3 = y2 + 45;
  tft.setTextColor(CLR_TEXT, CLR_BG);
  tft.setTextDatum(ML_DATUM);
  tft.drawString("Bei Regelung:", 10, y3 + 12, 2);
  drawButton(210, y3, 80, 28, cfg.fanOnControl ? "EIN" : "AUS",
             cfg.fanOnControl ? CLR_ACTIVE : CLR_INACTIVE,
             cfg.fanOnControl ? CLR_BG : CLR_TEXT);
}

// ============================================================
//  TOUCH-VERARBEITUNG
// ============================================================
bool inRect(uint16_t tx, uint16_t ty, int x, int y, int w, int h) {
  return tx >= x && tx <= x + w && ty >= y && ty <= y + h;
}

void handleTouch() {
  uint16_t tx, ty;
  if (!tft.getTouch(&tx, &ty)) return;
  unsigned long now = millis();
  if (now - tTouch < TOUCH_DEBOUNCE_MS) return;
  tTouch = now;

  switch (curScreen) {
    case SCR_MAIN:
      handleTouchMain(tx, ty);
      break;
    case SCR_SETTINGS:
      handleTouchSettings(tx, ty);
      break;
    case SCR_FAN:
      handleTouchFan(tx, ty);
      break;
  }
}

void handleTouchMain(uint16_t tx, uint16_t ty) {
  // Einstellungen-Button
  if (inRect(tx, ty, scrW / 2 - 80, scrH - 42, 160, 34)) {
    curScreen = SCR_SETTINGS;
    screenRedraw = true;
  }
}

void handleTouchSettings(uint16_t tx, uint16_t ty) {
  int y0 = 50;
  int rowH = 52;
  bool changed = false;

  // Zurueck-Button
  if (inRect(tx, ty, scrW - 90, 4, 80, 24)) {
    curScreen = SCR_MAIN;
    screenRedraw = true;
    saveSettings();
    return;
  }

  // Luefter-Button
  if (inRect(tx, ty, scrW / 2 - 90, scrH - 42, 180, 34)) {
    curScreen = SCR_FAN;
    screenRedraw = true;
    return;
  }

  // Temperatur Sollwert -/+
  if (inRect(tx, ty, 155, y0, 30, 28)) { cfg.tempSetpoint = max(5.0f, cfg.tempSetpoint - 0.5f); changed = true; }
  if (inRect(tx, ty, 275, y0, 30, 28)) { cfg.tempSetpoint = min(35.0f, cfg.tempSetpoint + 0.5f); changed = true; }
  // Temperatur Hysterese -/+
  if (inRect(tx, ty, 320, y0 + 16, 24, 22)) { cfg.tempHysteresis = max(0.1f, cfg.tempHysteresis - 0.1f); changed = true; }
  if (inRect(tx, ty, 402, y0 + 16, 24, 22)) { cfg.tempHysteresis = min(3.0f, cfg.tempHysteresis + 0.1f); changed = true; }

  // Feuchte Sollwert -/+
  int y1 = y0 + rowH;
  if (inRect(tx, ty, 155, y1, 30, 28)) { cfg.humSetpoint = max(50.0f, cfg.humSetpoint - 1.0f); changed = true; }
  if (inRect(tx, ty, 275, y1, 30, 28)) { cfg.humSetpoint = min(99.0f, cfg.humSetpoint + 1.0f); changed = true; }
  // Feuchte Hysterese -/+
  if (inRect(tx, ty, 320, y1 + 16, 24, 22)) { cfg.humHysteresis = max(0.5f, cfg.humHysteresis - 0.5f); changed = true; }
  if (inRect(tx, ty, 402, y1 + 16, 24, 22)) { cfg.humHysteresis = min(10.0f, cfg.humHysteresis + 0.5f); changed = true; }

  // CO2 Sollwert -/+
  int y2 = y0 + rowH * 2;
  if (inRect(tx, ty, 155, y2, 30, 28)) { cfg.co2Setpoint = max((uint16_t)400, (uint16_t)(cfg.co2Setpoint - 50)); changed = true; }
  if (inRect(tx, ty, 275, y2, 30, 28)) { cfg.co2Setpoint = min((uint16_t)2000, (uint16_t)(cfg.co2Setpoint + 50)); changed = true; }
  // CO2 Hysterese -/+
  if (inRect(tx, ty, 320, y2 + 16, 24, 22)) { cfg.co2Hysteresis = max((uint16_t)25, (uint16_t)(cfg.co2Hysteresis - 25)); changed = true; }
  if (inRect(tx, ty, 402, y2 + 16, 24, 22)) { cfg.co2Hysteresis = min((uint16_t)500, (uint16_t)(cfg.co2Hysteresis + 25)); changed = true; }

  if (changed) drawSettingsValues();
}

void handleTouchFan(uint16_t tx, uint16_t ty) {
  int y0 = 55;
  bool changed = false;

  // Zurueck
  if (inRect(tx, ty, scrW - 90, 4, 80, 24)) {
    curScreen = SCR_SETTINGS;
    screenRedraw = true;
    saveSettings();
    return;
  }

  // EIN-Dauer -/+
  if (inRect(tx, ty, 190, y0, 30, 28)) { cfg.fanOnSec = max(30UL, cfg.fanOnSec - 30); changed = true; }
  if (inRect(tx, ty, 330, y0, 30, 28)) { cfg.fanOnSec = min(1800UL, cfg.fanOnSec + 30); changed = true; }

  // AUS-Dauer -/+
  int y1 = y0 + 50;
  if (inRect(tx, ty, 190, y1, 30, 28)) { cfg.fanOffSec = max(60UL, cfg.fanOffSec - 60); changed = true; }
  if (inRect(tx, ty, 330, y1, 30, 28)) { cfg.fanOffSec = min(7200UL, cfg.fanOffSec + 60); changed = true; }

  // Intervall-Modus Toggle
  int y2 = y1 + 55;
  if (inRect(tx, ty, 210, y2, 80, 28)) { cfg.fanIntervalEnabled = !cfg.fanIntervalEnabled; changed = true; }

  // Bei Regelung Toggle
  int y3 = y2 + 45;
  if (inRect(tx, ty, 210, y3, 80, 28)) { cfg.fanOnControl = !cfg.fanOnControl; changed = true; }

  if (changed) drawFanValues();
}

// ============================================================
//  MQTT
// ============================================================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  char val[32];
  int len = min((unsigned int)31, length);
  memcpy(val, payload, len);
  val[len] = '\0';

  float fVal = atof(val);
  int iVal = atoi(val);

  if (strcmp(topic, TOPIC_SET_TEMP) == 0)     { cfg.tempSetpoint = constrain(fVal, 5.0f, 35.0f); }
  else if (strcmp(topic, TOPIC_SET_HUM) == 0)  { cfg.humSetpoint = constrain(fVal, 50.0f, 99.0f); }
  else if (strcmp(topic, TOPIC_SET_CO2) == 0)  { cfg.co2Setpoint = constrain(iVal, 400, 2000); }
  else if (strcmp(topic, TOPIC_SET_TEMP_HY) == 0) { cfg.tempHysteresis = constrain(fVal, 0.1f, 3.0f); }
  else if (strcmp(topic, TOPIC_SET_HUM_HY) == 0)  { cfg.humHysteresis = constrain(fVal, 0.5f, 10.0f); }
  else if (strcmp(topic, TOPIC_SET_CO2_HY) == 0)  { cfg.co2Hysteresis = constrain(iVal, 25, 500); }
  else if (strcmp(topic, TOPIC_SET_FAN_ON) == 0)   { cfg.fanOnSec = constrain((unsigned long)iVal, 30UL, 1800UL); }
  else if (strcmp(topic, TOPIC_SET_FAN_OFF) == 0)  { cfg.fanOffSec = constrain((unsigned long)iVal, 60UL, 7200UL); }
  else if (strcmp(topic, TOPIC_SET_FAN_INTV) == 0) { cfg.fanIntervalEnabled = (iVal != 0); }
  else if (strcmp(topic, TOPIC_SET_FAN_CTRL) == 0) { cfg.fanOnControl = (iVal != 0); }

  saveSettings();
  if (curScreen == SCR_SETTINGS) screenRedraw = true;
  if (curScreen == SCR_FAN) screenRedraw = true;
}

void mqttSubscribe() {
  mqtt.subscribe(TOPIC_SET_TEMP);
  mqtt.subscribe(TOPIC_SET_HUM);
  mqtt.subscribe(TOPIC_SET_CO2);
  mqtt.subscribe(TOPIC_SET_TEMP_HY);
  mqtt.subscribe(TOPIC_SET_HUM_HY);
  mqtt.subscribe(TOPIC_SET_CO2_HY);
  mqtt.subscribe(TOPIC_SET_FAN_ON);
  mqtt.subscribe(TOPIC_SET_FAN_OFF);
  mqtt.subscribe(TOPIC_SET_FAN_INTV);
  mqtt.subscribe(TOPIC_SET_FAN_CTRL);
}

void mqttPublish() {
  if (!mqtt.connected()) return;
  char buf[16];

  snprintf(buf, sizeof(buf), "%.1f", curTemp);
  mqtt.publish(TOPIC_SENSOR_TEMP, buf, true);
  snprintf(buf, sizeof(buf), "%.1f", curHum);
  mqtt.publish(TOPIC_SENSOR_HUM, buf, true);
  snprintf(buf, sizeof(buf), "%u", curCO2);
  mqtt.publish(TOPIC_SENSOR_CO2, buf, true);

  mqtt.publish(TOPIC_OUT_HEATER, outState[0] ? "ON" : "OFF", true);
  mqtt.publish(TOPIC_OUT_COOLER, outState[1] ? "ON" : "OFF", true);
  mqtt.publish(TOPIC_OUT_FAE,    outState[2] ? "ON" : "OFF", true);
  mqtt.publish(TOPIC_OUT_HUM,    outState[3] ? "ON" : "OFF", true);
  mqtt.publish(TOPIC_OUT_FAN,    outState[4] ? "ON" : "OFF", true);

  // JSON Status
  JsonDocument doc;
  doc["temperatur"] = curTemp;
  doc["feuchte"]    = curHum;
  doc["co2"]        = curCO2;
  doc["heizung"]    = outState[0];
  doc["kuehlung"]   = outState[1];
  doc["fae"]        = outState[2];
  doc["befeuchter"] = outState[3];
  doc["luefter"]    = outState[4];
  JsonObject sp = doc["sollwerte"].to<JsonObject>();
  sp["temperatur"]      = cfg.tempSetpoint;
  sp["feuchte"]         = cfg.humSetpoint;
  sp["co2"]             = cfg.co2Setpoint;
  sp["temp_hysterese"]  = cfg.tempHysteresis;
  sp["feuchte_hysterese"] = cfg.humHysteresis;
  sp["co2_hysterese"]   = cfg.co2Hysteresis;
  char jsonBuf[512];
  serializeJson(doc, jsonBuf, sizeof(jsonBuf));
  mqtt.publish(TOPIC_STATUS, jsonBuf, true);
}

void mqttReconnect() {
  if (WiFi.status() != WL_CONNECTED) return;
  unsigned long now = millis();
  if (now - tMqttConn < MQTT_RECONNECT_MS) return;
  tMqttConn = now;

  if (strlen(MQTT_USER) > 0) {
    mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD);
  } else {
    mqtt.connect(MQTT_CLIENT_ID);
  }
  if (mqtt.connected()) {
    mqttSubscribe();
    mqttPublish();
  }
}

// ============================================================
//  WIFI
// ============================================================
void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void handleWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  unsigned long now = millis();
  if (now - tWifi < WIFI_RECONNECT_MS) return;
  tWifi = now;
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  Serial.println("Pilzzucht-Steuerung startet...");

  loadSettings();
  initOutputs();
  initSensor();

  // Display
  tft.init();
  tft.setRotation(1);
  tft.setTouch(touchCal);
  scrW = tft.width();
  scrH = tft.height();
  tft.fillScreen(CLR_BG);
  tft.setTextColor(CLR_TEXT, CLR_BG);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Pilzzucht-Steuerung", scrW / 2, scrH / 2 - 20, 4);
  tft.drawString("Initialisierung...", scrW / 2, scrH / 2 + 20, 2);

  // WiFi
  connectWifi();
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 8000) {
    delay(250);
    tft.drawString("WiFi verbinden...", scrW / 2, scrH / 2 + 40, 1);
  }

  // MQTT
  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(1024);
  mqttReconnect();

  fanCycleStart = millis();
  screenRedraw = true;
  Serial.println("Bereit.");
}

// ============================================================
//  LOOP
// ============================================================
void loop() {
  unsigned long now = millis();

  // WiFi
  handleWifi();

  // MQTT
  if (mqtt.connected()) {
    mqtt.loop();
  } else {
    mqttReconnect();
  }

  // Sensor lesen
  if (now - tSensor >= SENSOR_READ_MS) {
    tSensor = now;
    readSensor();
  }

  // Regelung
  if (now - tControl >= CONTROL_RUN_MS) {
    tControl = now;
    runControl();
    handleFanInterval();
  }

  // MQTT publishen
  if (now - tMqtt >= MQTT_PUBLISH_MS) {
    tMqtt = now;
    mqttPublish();
  }

  // Display
  if (screenRedraw) {
    screenRedraw = false;
    prevTemp = -999;
    prevHum  = -999;
    prevCO2  = -1;
    memset(prevOut, 0, sizeof(prevOut));
    switch (curScreen) {
      case SCR_MAIN:     drawMainScreen();     break;
      case SCR_SETTINGS: drawSettingsScreen();  break;
      case SCR_FAN:      drawFanScreen();       break;
    }
  } else if (curScreen == SCR_MAIN && now - tDisplay >= DISPLAY_UPDATE_MS) {
    tDisplay = now;
    updateMainValues(false);
  }

  // Touch
  handleTouch();
}
