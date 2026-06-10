#include <Arduino.h>
#include "secret.h"
#include <WiFi.h>
#include <WiFiClient.h>
#include <PubSubClient.h>
#include "HX711.h"

// =========================
// WIFI / MQTT
// =========================
const char *ssid = WIFI_SSID;
const char *password = WIFI_PASSWORD;

const char *mqtt_server = "maisonneuve.aws.thinger.io";
const uint16_t mqtt_port = 1883;
const char *mqtt_user = "Keeg_00";
const char *mqtt_pass = "esp32_inv3050";

#define CLIENT_ID "esp32_inv"

// =========================
// HX711
// =========================
#define HX711_DOUT 5
#define HX711_SCK  6

HX711 scale;
float calibration_factor = 203.6;

// =========================
// LCD UART
// =========================
HardwareSerial lcdSerial(1);
#define LCD_TX_PIN 17
#define LCD_BAUD   9600

// =========================
// Boutons
// =========================
#define BTN_TARE  7
#define BTN_UNIT  8

volatile bool tareRequest = false;
volatile bool unitRequest = false;

volatile unsigned long lastTareInterrupt = 0;
volatile unsigned long lastUnitInterrupt = 0;

const unsigned long interruptDebounceUs = 200000; // 200 ms

// =========================
// LED SYSTEME ACTIVE LOW
// =========================
#define LED_PIN 9

// =========================
// COMPTAGE
// =========================
const float ITEM_WEIGHT_G = 3.50;
const float ZERO_THRESHOLD_G = 1.5;

bool showKg = false;

// =========================
// MQTT
// =========================
WiFiClient espClient;
PubSubClient mqttclient(espClient);

unsigned long lastSensorPublish = 0;
const unsigned long sensorInterval = 5000;

// =========================
// INTERRUPTIONS BOUTONS
// =========================
void IRAM_ATTR tareISR()
{
  unsigned long now = micros();

  if (now - lastTareInterrupt > interruptDebounceUs)
  {
    tareRequest = true;
    lastTareInterrupt = now;
  }
}

void IRAM_ATTR unitISR()
{
  unsigned long now = micros();

  if (now - lastUnitInterrupt > interruptDebounceUs)
  {
    unitRequest = true;
    lastUnitInterrupt = now;
  }
}

// =========================
// LCD FUNCTIONS
// =========================
void lcdCommand(uint8_t cmd)
{
  lcdSerial.write(0xFE);
  lcdSerial.write(cmd);
  delay(5);
}

void lcdClear()
{
  lcdCommand(0x51);
  delay(10);
}

void lcdSetCursor(uint8_t pos)
{
  lcdSerial.write(0xFE);
  lcdSerial.write(0x45);
  lcdSerial.write(pos);
  delay(5);
}

uint8_t lcdPos(uint8_t col, uint8_t row)
{
  static const uint8_t row_offsets[4] = {0x00, 0x40, 0x14, 0x54};
  return (uint8_t)(col + row_offsets[row]);
}

void lcdPrintAt(uint8_t col, uint8_t row, const String &text)
{
  lcdSetCursor(lcdPos(col, row));
  lcdSerial.print(text);
}

void lcdPrintFixed(uint8_t col, uint8_t row, String text, uint8_t width = 20)
{
  if (text.length() > width)
  {
    text = text.substring(0, width);
  }
  else
  {
    while (text.length() < width)
    {
      text += ' ';
    }
  }

  lcdPrintAt(col, row, text);
}

void initLCD()
{
  lcdSerial.begin(LCD_BAUD, SERIAL_8N1, -1, LCD_TX_PIN);
  delay(300);

  lcdClear();
  delay(20);

  lcdPrintFixed(0, 0, "Demarrage...");
  lcdPrintFixed(0, 1, "Init balance");
}

// =========================
// HX711
// =========================
void initHX711()
{
  scale.begin(HX711_DOUT, HX711_SCK);

  while (!scale.is_ready())
  {
    delay(50);
  }

  scale.set_scale(calibration_factor);
  scale.tare(20);

  lcdClear();
  lcdPrintFixed(0, 0, "Mesure balance");
}

void doTare()
{
  lcdPrintFixed(0, 0, "Tare...");
  lcdPrintFixed(0, 1, "Ne rien poser");
  scale.tare(20);

  lcdClear();
  lcdPrintFixed(0, 0, "Mesure balance");
}

// =========================
// MQTT CALLBACK
// =========================
void mqttCallback(char *topic, byte *payload, unsigned int length)
{
  Serial.print("MQTT msg on [");
  Serial.print(topic);
  Serial.print("]: ");

  for (unsigned int i = 0; i < length; i++)
  {
    Serial.write(payload[i]);
  }

  Serial.println();
}

// =========================
// WIFI
// =========================
void connectWiFi()
{
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  Serial.print("Connexion WiFi");

  while (WiFi.status() != WL_CONNECTED)
  {
    Serial.print(".");
    delay(500);
  }

  Serial.println();
  Serial.println("WiFi CONNECTE");
  Serial.println(WiFi.localIP());
}

// =========================
// MQTT
// =========================
bool connectMQTT()
{
  mqttclient.setServer(mqtt_server, mqtt_port);
  mqttclient.setCallback(mqttCallback);

  Serial.println("Connexion MQTT...");

  if (mqttclient.connect(CLIENT_ID, mqtt_user, mqtt_pass))
  {
    Serial.println("MQTT CONNECTE !");
    return true;
  }
  else
  {
    Serial.print("MQTT FAIL state = ");
    Serial.println(mqttclient.state());
    return false;
  }
}

// =========================
// BOUTONS
// =========================
void handleButtons()
{
  if (tareRequest)
  {
    tareRequest = false;
    doTare();
  }

  if (unitRequest)
  {
    unitRequest = false;
    showKg = !showKg;
  }
}

// =========================
// CALIBRATION
// =========================
void Calibration()
{
  Serial.println("Calibration: entre le poids connu en grammes:");

  while (!Serial.available())
  {
    delay(10);
  }

  float knownWeight = Serial.parseFloat();

  if (knownWeight <= 0)
  {
    Serial.println("Poids invalide");
    return;
  }

  float measured = scale.get_units(20);

  calibration_factor = calibration_factor * (measured / knownWeight);
  scale.set_scale(calibration_factor);

  Serial.print("Nouveau facteur: ");
  Serial.println(calibration_factor, 6);
}

// =========================
// SETUP
// =========================
void setup()
{
  Serial.begin(115200);
  delay(500);

  pinMode(BTN_TARE, INPUT_PULLUP);
  pinMode(BTN_UNIT, INPUT_PULLUP);

  attachInterrupt(digitalPinToInterrupt(BTN_TARE), tareISR, FALLING);
  attachInterrupt(digitalPinToInterrupt(BTN_UNIT), unitISR, FALLING);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW); // LED ON systeme allume, active LOW

  initLCD();
  initHX711();

  connectWiFi();
  connectMQTT();

  lcdClear();
  lcdPrintFixed(0, 0, "Mesure balance");
}

// =========================
// LOOP
// =========================
void loop()
{
  handleButtons();

  if (Serial.available())
  {
    char c = Serial.read();

    if (c == 'c' || c == 'C')
    {
      Calibration();

      while (Serial.available())
      {
        Serial.read();
      }
    }
  }

  if (!mqttclient.connected())
  {
    static unsigned long lastAttempt = 0;

    if (millis() - lastAttempt > 5000)
    {
      lastAttempt = millis();
      connectMQTT();
    }
  }
  else
  {
    mqttclient.loop();
  }

  if (!scale.is_ready())
  {
    Serial.println("HX711 not ready");
    delay(200);
    return;
  }

  float grams = scale.get_units(20);
  float kg = grams / 1000.0;

  float grams_display = round(grams * 10.0) / 10.0;

  // =========================
  // STABILISATION ZERO + PAS DE NEGATIF
  // =========================
  if (grams_display < ZERO_THRESHOLD_G)
  {
    grams_display = 0.0;
  }

  kg = grams_display / 1000.0;
  float kg_display = round(kg * 1000.0) / 1000.0;

  // =========================
  // CALCUL QUANTITE
  // =========================
  int quantity = 0;

  if (grams_display > ZERO_THRESHOLD_G)
  {
    quantity = (grams_display + ITEM_WEIGHT_G / 2) / ITEM_WEIGHT_G;
  }

  // =========================
  // LCD
  // =========================
  lcdPrintFixed(0, 0, "Mesure balance");

  if (showKg)
  {
    lcdPrintFixed(0, 1, "Poids: " + String(kg_display, 3) + " kg");
  }
  else
  {
    lcdPrintFixed(0, 1, "Poids: " + String(grams_display, 1) + " g");
  }

  lcdPrintFixed(0, 2, "Quantite: " + String(quantity));

  // =========================
  // SERIAL
  // =========================
  Serial.print("Poids: ");
  Serial.print(grams_display, 1);
  Serial.print(" g | ");
  Serial.print(kg_display, 3);
  Serial.print(" kg | Quantite: ");
  Serial.println(quantity);

  // =========================
  // MQTT PUBLISH
  // =========================
  if (millis() - lastSensorPublish >= sensorInterval)
  {
    lastSensorPublish = millis();

    char txt[16];
    snprintf(txt, sizeof(txt), "%.3f", kg);
    mqttclient.publish("poids", txt);

    char qtyTxt[16];
    snprintf(qtyTxt, sizeof(qtyTxt), "%d", quantity);
    mqttclient.publish("quantite", qtyTxt);

    char alert[120];

    if (kg < 0.100f && kg >= 0.003f)
    {
      Serial.println("ALERTE: poids inferieur a 0.100 kg");

      snprintf(alert, sizeof(alert),
               "{\"type\":\"poids\",\"niveau\":\"ALERTE niveau est bas\",\"val\":%.3f,\"seuil\":0.100,\"unite\":\"kg\"}",
               kg);

      mqttclient.publish("etat", "ALERTE niveau est bas");
    }
    else
    {
      snprintf(alert, sizeof(alert),
               "{\"type\":\"poids\",\"niveau\":\"\",\"val\":%.3f,\"seuil\":0.100,\"unite\":\"kg\"}",
               kg);

      mqttclient.publish("etat", "OK");
    }

    mqttclient.publish("poids/alerte", alert);
  }

  delay(300);
} // test interru