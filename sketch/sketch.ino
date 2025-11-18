/*
UNIVERSIDAD DEL VALLE DE GUATEMALA
PROGRAMACION DE MICROPROCESADORES
Estación Meteorológica con Geolocalización WiFi
*/

#include <Wire.h>
#include <BH1750.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_BMP280.h>
#include <Adafruit_Sensor.h>
#include <DHT.h>
#include <Servo.h>
#include <ESP8266WiFi.h>  
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

char ssid[] = "Saritah";
char pass[] = "guate2000.@@";
const String server = "https://cc-3086-estacion-meteorologica-back.vercel.app/api";

// Google Geolocation API
const char* googleHost = "www.googleapis.com";
const String geoEndpoint = "/geolocation/v1/geolocate?key=";
const String apiKey = "AIzaSyAbGG-Gg66vr_BqLRnElDW97G_wGyHzE1M";

// ---------------- PINES ----------------
#define PIN_DHT 14   // D5
#define DHTTYPE DHT11
#define FC37_PIN 12   // D6
#define MQ3_PIN A0
#define BUTTON_PIN 13   // D7
#define BUZZER_PIN 15   // D8
#define SERVO_PIN 2    // D4

BH1750 lightMeter;
LiquidCrystal_I2C lcd(0x27, 16, 2);
Adafruit_BMP280 bmp;
DHT dht(PIN_DHT, DHTTYPE);
Servo gateServo;

// ---------------- TIMERS ----------------
unsigned long lastSend = 0;
const unsigned long interval = 3000; // cada 3 segundos (sincronizado con geolocalización)

unsigned long lastLCD = 0;
int lcdPage = 0;

// ---------------- VARIABLES DE GEOLOCALIZACIÓN ----------------
double currentLatitude = 0.0;
double currentLongitude = 0.0;
double currentAccuracy = 0.0;
bool geoDataAvailable = false;

// Variables para mantener la última ubicación válida
double lastValidLatitude = 0.0;
double lastValidLongitude = 0.0;
double lastValidAccuracy = 0.0;

// ---------------- MQ3  ----------------
float m_alcohol = -0.60, b_alcohol = 1.70;
float m_H2 = -0.35, b_H2 = 0.25;
float m_CO = -0.50, b_CO = 0.10;
float m_propano = -0.45, b_propano = 1.00;
float m_CH4 = -0.38, b_CH4 = 0.80;

float R0 = 0.74433;

float safeFloat(float v) {
  if (isnan(v) || isinf(v)) return 0.0;
  return v;
}

// ---------------- FUNCIONES MQ3 ----------------
float MQ3_PPM(float ratio, float m, float b) {
  return pow(10, (log10(ratio) - b) / m);
}

void readMQ3Gases(float &ppm_alc, float &ppm_H2, float &ppm_CO,
                  float &ppm_prop, float &ppm_CH4) {

  int raw = analogRead(MQ3_PIN);
  float V = raw * (3.3 / 1023.0);
  if (V < 0.01) V = 0.01;

  float Rs_mq = (3.3 - V) / V;
  float ratio = Rs_mq / R0;

  ppm_alc  = MQ3_PPM(ratio, m_alcohol, b_alcohol);
  ppm_H2   = MQ3_PPM(ratio, m_H2, b_H2);
  ppm_CO   = MQ3_PPM(ratio, m_CO, b_CO);
  ppm_prop = MQ3_PPM(ratio, m_propano, b_propano);
  ppm_CH4  = MQ3_PPM(ratio, m_CH4, b_CH4);
}

// ---------------- FUNCIÓN DE GEOLOCALIZACIÓN ----------------
bool updateGeolocation() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi no conectado para geolocalización");
    return false;
  }

  Serial.println("🔎 Escaneando redes WiFi para geolocalización...");
  int n = WiFi.scanNetworks();

  if (n < 2) {
    Serial.println("⚠️ No se encontraron suficientes redes para geolocalización.");
    return false;
  }

  Serial.print("📶 Redes detectadas: ");
  Serial.println(n);

  DynamicJsonDocument doc(2048);
  
  JsonArray wifiAccessPoints = doc.createNestedArray("wifiAccessPoints");
  for (int j = 0; j < n; ++j) {
    JsonObject wifiObject = wifiAccessPoints.createNestedObject();
    wifiObject["macAddress"] = WiFi.BSSIDstr(j);
    wifiObject["signalStrength"] = WiFi.RSSI(j);
  }

  String jsonString;
  serializeJson(doc, jsonString);

  Serial.println("📦 JSON enviado a Google:");
  Serial.println(jsonString);

  // Conexión HTTPS
  WiFiClientSecure client;
  client.setInsecure();

  Serial.println("🌐 Conectando a Google Geolocation API...");
  if (!client.connect(googleHost, 443)) {
    Serial.println("❌ Error al conectar con Google API.");
    return false;
  }

  // Petición POST
  String fullEndpoint = geoEndpoint + apiKey;
  client.println("POST " + fullEndpoint + " HTTP/1.1");
  client.println("Host: " + String(googleHost));
  client.println("Content-Type: application/json");
  client.print("Content-Length: ");
  client.println(jsonString.length());
  client.println();
  client.print(jsonString);

  // Espera de respuesta
  String response = "";
  unsigned long timeout = millis();
  while (!client.available()) {
    if (millis() - timeout > 10000) {
      Serial.println("⏳ Tiempo de espera agotado.");
      client.stop();
      return false;
    }
  }

  // Lectura de la respuesta
  response = client.readString();
  client.stop();

  // Obteniendo el JSON
  int jsonStart = response.indexOf('{');
  if (jsonStart == -1) {
    Serial.println("⚠️ No se encontró contenido JSON en la respuesta.");
    return false;
  }
  String jsonPart = response.substring(jsonStart);

  Serial.println("📄 Respuesta JSON de Google:");
  Serial.println(jsonPart);

  // Análisis del JSON
  DynamicJsonDocument responseDoc(1024);
  DeserializationError error = deserializeJson(responseDoc, jsonPart);

  if (error) {
    Serial.print("❌ Error al analizar JSON: ");
    Serial.println(error.c_str());
    return false;
  } else {
    if (responseDoc.containsKey("error")) {
      String errorMessage = responseDoc["error"]["message"];
      Serial.print("⚠️ La API de Google devolvió un error: ");
      Serial.println(errorMessage);
      return false;
    } else {
      currentLatitude = responseDoc["location"]["lat"];
      currentLongitude = responseDoc["location"]["lng"];
      currentAccuracy = responseDoc["accuracy"];
      geoDataAvailable = true;

      Serial.println();
      Serial.println("📍 ¡Ubicación actualizada!");
      Serial.print("🌎 Latitud:  "); Serial.println(currentLatitude, 6);
      Serial.print("🌍 Longitud: "); Serial.println(currentLongitude, 6);
      Serial.print("🎯 Precisión: "); Serial.print(currentAccuracy); Serial.println(" m");
      Serial.println();
      return true;
    }
  }
  return false;
}

String getRemoteState() {
  if (WiFi.status() != WL_CONNECTED) return "";

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.begin(client, server+"/state");

  int code = http.GET();
  if (code != 200) return "";

  return http.getString();
}

void updateFromServer(bool &estadoRemoto) {
  String json = getRemoteState();
  if (json.length() == 0) return;

  bool open = json.indexOf("\"open\":true") > 0;
  estadoRemoto = open;

  int textStart = json.indexOf("\"text\":\"") + 8;
  int textEnd = json.indexOf("\"", textStart);
  String text = json.substring(textStart, textEnd);

  lcd.clear();
  lcd.print(text);

  gateServo.write(open ? 180 : 0);
  digitalWrite(BUZZER_PIN, open ? HIGH : LOW);
}

void updateRemoteState(bool open, String textMsg) {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.begin(client, String(server) + "/state");
  http.addHeader("Content-Type", "application/json");

  String json = "{";
  json += "\"open\":" + String(open ? "true" : "false") + ",";
  json += "\"text\":\"" + textMsg + "\"";
  json += "}";

  Serial.println("Enviando estado al servidor:");
  Serial.println(json);

  int code = http.POST(json);
  Serial.print("Estado actualizado, HTTP: ");
  Serial.println(code);

  http.end();
}

void sendSupabase(
  float ppm_alcohol, float ppm_H2, float ppm_CO,
  float ppm_propano, float ppm_CH4,
  float lux, float tempBMP, float pres, float alt,
  float hum, float tempDHT, int lluvia, bool abierto
) {

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi desconectado");
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.begin(client, server+"/readings");
  http.addHeader("Content-Type", "application/json");

  String jsonData = "{";
  jsonData += "\"ppm_alcohol\":" + String(safeFloat(ppm_alcohol)) + ",";
  jsonData += "\"ppm_h2\":" + String(safeFloat(ppm_H2)) + ",";
  jsonData += "\"ppm_co\":" + String(safeFloat(ppm_CO)) + ",";
  jsonData += "\"ppm_propano\":" + String(safeFloat(ppm_propano)) + ",";
  jsonData += "\"ppm_ch4\":" + String(safeFloat(ppm_CH4)) + ",";
  jsonData += "\"lux\":" + String(safeFloat(lux)) + ",";
  jsonData += "\"temp_bmp\":" + String(safeFloat(tempBMP)) + ",";
  jsonData += "\"pres\":" + String(safeFloat(pres)) + ",";
  jsonData += "\"alt\":" + String(safeFloat(alt)) + ",";
  jsonData += "\"hum\":" + String(safeFloat(hum)) + ",";
  jsonData += "\"temp_dht\":" + String(safeFloat(tempDHT)) + ",";
  jsonData += "\"lluvia\":" + String(lluvia) + ",";
  jsonData += "\"open\":" + String(abierto);
  jsonData += ",\"latitude\":" + String(geoDataAvailable ? currentLatitude : 0.0, 6);
  jsonData += ",\"longitude\":" + String(geoDataAvailable ? currentLongitude : 0.0, 6);
  jsonData += ",\"accuracy\":" + String(geoDataAvailable ? currentAccuracy : 0.0, 2);
  
  jsonData += "}";

  Serial.println(jsonData);

  int code = http.POST(jsonData);
  Serial.print("Respuesta HTTP: ");
  Serial.println(code);

  http.end();
}

void setup() {
  Serial.begin(115200);

  Wire.begin(4, 5); // SDA=D2, SCL=D1
  dht.begin();
  lightMeter.begin();

  if (!bmp.begin(0x76)) {
    Serial.println("BMP280 no encontrado");
    while (1);
  }

  lcd.init();
  lcd.backlight();

  pinMode(FC37_PIN, INPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  gateServo.attach(SERVO_PIN);
  gateServo.write(0);

  WiFi.begin(ssid, pass);
  Serial.println("\nConectando WiFi...");
  while (WiFi.status() != WL_CONNECTED) {
    delay(200);
    Serial.print(".");
  }
  Serial.println("\nWiFi conectado!");
  Serial.print("Dirección IP: ");
  Serial.println(WiFi.localIP());
  
  // Obtener ubicación inicial
  delay(2000); // Esperar estabilización
  updateGeolocation();
}

void loop() {
  static bool ultimoEstado = false;
  static bool estadoRemoto = false;
  
  bool botonPresionado = (digitalRead(BUTTON_PIN) == LOW);
  
  // Si el botón cambia de estado, actualizar el servidor
  if (botonPresionado != ultimoEstado) {
    ultimoEstado = botonPresionado;
    estadoRemoto = botonPresionado;

    String msg = botonPresionado ? "Puerta abierta" : "Puerta cerrada";
    updateRemoteState(botonPresionado, msg);
    
    // Actualizar servo y buzzer inmediatamente
    gateServo.write(botonPresionado ? 180 : 0);
    digitalWrite(BUZZER_PIN, botonPresionado ? HIGH : LOW);
  } else {
    // Si no hay cambio local, consultar el estado remoto
    updateFromServer(estadoRemoto);
  }

  // ----- MQ-3 -----
  float ppm_alcohol, ppm_H2, ppm_CO, ppm_propano, ppm_CH4;
  readMQ3Gases(ppm_alcohol, ppm_H2, ppm_CO, ppm_propano, ppm_CH4);

  float lux = lightMeter.readLightLevel();
  float tempBMP = bmp.readTemperature();
  float pres = bmp.readPressure() / 100.0;
  float alt  = bmp.readAltitude(1500);
  float hum  = dht.readHumidity();
  float tempDHT = dht.readTemperature();
  int lluvia = digitalRead(FC37_PIN);   // 0 = mojado

  if (millis() - lastLCD >= 2000) {
    lastLCD = millis();
    lcd.clear();

    switch (lcdPage) {
      case 0:
        lcd.print("Alc:");
        lcd.print((int)ppm_alcohol);
        lcd.setCursor(0,1);
        lcd.print("H2:");
        lcd.print((int)ppm_H2);
        break;

      case 1:
        lcd.print("CO:");
        lcd.print((int)ppm_CO);
        lcd.setCursor(0,1);
        lcd.print("Prop:");
        lcd.print((int)ppm_propano);
        break;

      case 2:
        lcd.print("CH4:");
        lcd.print((int)ppm_CH4);
        break;

      case 3:
        lcd.print("Lux:");
        lcd.print((int)lux);
        lcd.setCursor(0,1);
        lcd.print("Rain:");
        lcd.print(lluvia == 0 ? "YES" : "NO");
        break;

      case 4:
        lcd.print("T:");
        lcd.print(tempBMP);
        lcd.print("C  H:");
        lcd.print(hum);
        break;

      case 5:
        lcd.print("Pres:");
        lcd.print(pres);
        lcd.setCursor(0,1);
        lcd.print("Alt:");
        lcd.print(alt);
        break;
      
      case 6:
        if (geoDataAvailable) {
          lcd.print("Lat:");
          lcd.print(currentLatitude, 3);
          lcd.setCursor(0,1);
          lcd.print("Lon:");
          lcd.print(currentLongitude, 3);
        } else {
          lcd.print("Ubicacion:");
          lcd.setCursor(0,1);
          lcd.print("No disponible");
        }
        break;
    }

    lcdPage = (lcdPage + 1) % 7;
  }

  // ---------- TIMER SINCRONIZADO: GEOLOCALIZACIÓN + ENVÍO DATOS ----------
  unsigned long now = millis();
  if (now - lastSend >= interval) {
    lastSend = now;

    // Primero actualizar geolocalización
    Serial.println("=== CICLO DE ACTUALIZACIÓN (cada 3s) ===");
    updateGeolocation();
    
    // Luego enviar todos los datos incluyendo la ubicación actualizada
    sendSupabase(
      ppm_alcohol, ppm_H2, ppm_CO, ppm_propano, ppm_CH4,
      lux, tempBMP, pres, alt,
      hum, tempDHT, lluvia, estadoRemoto
    );
    
    Serial.println("✅ Datos enviados con ubicación!");
    Serial.println("=====================================\n");
  }
}
