#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <HX711.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ===== Wi-Fi =====
const char* WIFI_SSID     = "Wokwi-GUEST";
const char* WIFI_PASSWORD = "";

// ===== ThingSpeak MQTT =====
const char* MQTT_BROKER   = "mqtt3.thingspeak.com";
const int   MQTT_PORT     = 1883;
const char* CHANNEL_ID    = "3324714";
const char* WRITE_API_KEY = "EUWHXDGHE5QC5YIG";
const char* MQTT_CLIENT   = "MSMWLQkoNyg2NRgHNSwDGyA";
const char* MQTT_USER = "MSMWLQkoNyg2NRgHNSwDGyA";
const char* MQTT_PASS = "ljRbHsXhzGM5XI7JVPJvtV4Q";

// ===== Coordenada fixa (Leopoldina, MG) =====
const float LATITUDE  = -21.5319;
const float LONGITUDE = -42.6453;

// ===== WeatherAPI =====
const char* WEATHER_API_KEY = "114eb94c394f4a808fc135603260204";

// ===== LCD =====
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ===== Ultrassônico (volume) =====
#define TRIG1 13
#define ECHO1 12

// ===== HX711 =====
#define DOUT 18
#define SCK  19
HX711 scale;

// ===== Configurações =====
#define CAPACIDADE_CM        30
#define PESO_MAXIMO          5000.0
#define INTERVALO_SENSOR_MS  300
#define INTERVALO_CLIMA_MS   300000UL  // 5 minutos
#define INTERVALO_MQTT_MS    15000UL   // ThingSpeak exige mínimo 15s entre publicações

// ===== Objetos =====
WiFiClient espClient;
PubSubClient mqtt(espClient);

// ===== Variáveis globais =====
unsigned long ultimaConsultaClima = 0;
unsigned long ultimaPublicacao    = 0;
bool  vaiChover   = false;
float chanceChuva = 0.0;

// ===== Protótipos =====
void conectarWiFi();
void conectarMQTT();
float lerDistancia(int trig, int echo);
void atualizarLCD(float peso, int porcentagem, bool cheia);
void consultarClima();
void publicarThingSpeak(int volume, float peso, bool cheia);

// ========================================================
float lerDistancia(int trig, int echo) {
  digitalWrite(trig, LOW);
  delayMicroseconds(2);
  digitalWrite(trig, HIGH);
  delayMicroseconds(10);
  digitalWrite(trig, LOW);

  long duracao = pulseIn(echo, HIGH, 30000);
  if (duracao == 0) return -1;

  float dist = duracao * 0.034 / 2.0;
  if (dist < 2.0 || dist > 400.0) return -1;

  return dist;
}

// ========================================================
void atualizarLCD(float peso, int porcentagem, bool cheia) {
  lcd.setCursor(0, 0);
  if (cheia) {
    lcd.print("LIXEIRA CHEIA!  ");
  } else {
    lcd.print("Peso:");
    lcd.print((int)peso);
    lcd.print("g        ");
  }

  lcd.setCursor(0, 1);
  if (cheia) {
    lcd.print("Esvazie!        ");
  } else {
    lcd.print("Vol:");
    lcd.print(porcentagem);
    lcd.print(vaiChover ? "% CHUVA!" : "%        ");
  }
}

// ========================================================
void consultarClima() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[CLIMA] Wi-Fi desconectado, pulando consulta.");
    return;
  }

  HTTPClient http;
  String url = "http://api.weatherapi.com/v1/forecast.json?key=";
  url += WEATHER_API_KEY;
  url += "&q=" + String(LATITUDE, 4) + "," + String(LONGITUDE, 4);
  url += "&days=1&lang=pt";

  Serial.println("[CLIMA] Consultando WeatherAPI...");
  http.begin(url);
  int httpCode = http.GET();

  if (httpCode == 200) {
    String payload = http.getString();
    StaticJsonDocument<4096> doc;
    DeserializationError erro = deserializeJson(doc, payload);

    if (!erro) {
      chanceChuva = doc["forecast"]["forecastday"][0]["day"]["daily_chance_of_rain"];
      float mmChuva = doc["forecast"]["forecastday"][0]["day"]["totalprecip_mm"];
      vaiChover = (chanceChuva >= 50.0);

      Serial.printf("[CLIMA] Chance de chuva: %.0f%% | Precipitacao: %.1fmm | %s\n",
        chanceChuva, mmChuva, vaiChover ? "VAI CHOVER!" : "Sem chuva");
    } else {
      Serial.println("[CLIMA] Erro ao parsear JSON!");
    }
  } else {
    Serial.printf("[CLIMA] Erro HTTP: %d\n", httpCode);
  }

  http.end();
}

// ========================================================
void publicarThingSpeak(int volume, float peso, bool cheia) {
  if (!mqtt.connected()) {
    conectarMQTT();
  }

  // Monta o status como número: 0=OK, 1=MEDIO, 2=CHEIA
  int status = cheia ? 2 : (volume >= 60 ? 1 : 0);

  // Monta o payload do ThingSpeak
  // field1=volume, field2=peso, field3=status, field4=chanceChuva, field5=lat, field6=lon
  char payload[200];
  snprintf(payload, sizeof(payload),
    "field1=%d&field2=%.1f&field3=%d&field4=%.0f",
    volume, peso, status, chanceChuva);

  // Tópico de publicação do ThingSpeak
  char topico[64];
  snprintf(topico, sizeof(topico), "channels/%s/publish/%s", CHANNEL_ID, WRITE_API_KEY);
  //snprintf(topico, sizeof(topico), "channels/<CHANNEL_ID>/publish/<WRITE_API_KEY>", CHANNEL_ID, WRITE_API_KEY);

  if (mqtt.publish(topico, payload)) {
    Serial.println("[MQTT] Dados publicados no ThingSpeak!");
    Serial.println(payload);
  } else {
    Serial.println("[MQTT] Falha ao publicar no ThingSpeak.");
  }
}

// ========================================================
void conectarWiFi() {
  Serial.printf("Conectando ao Wi-Fi: %s ", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - inicio < 15000) {
    Serial.print(".");
    delay(500);
  }

  if (WiFi.status() == WL_CONNECTED)
    Serial.printf("\nWi-Fi conectado! IP: %s\n", WiFi.localIP().toString().c_str());
  else
    Serial.println("\n[ERRO] Falha ao conectar no Wi-Fi!");
}

// ========================================================
void conectarMQTT() {
  Serial.print("Conectando ao ThingSpeak MQTT...");
  // No ThingSpeak: username = qualquer string, password = Write API Key
  //if (mqtt.connect(MQTT_CLIENT, "any", WRITE_API_KEY)) {

  if (mqtt.connect(MQTT_CLIENT, MQTT_USER, MQTT_PASS)){ 
    Serial.println(" conectado!");
  } else {
    Serial.printf(" falhou (rc=%d)\n", mqtt.state());
  }
}

// ========================================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println(">>> Lixeira Inteligente iniciando...");

  // LCD
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Iniciando...");

  // Pinos ultrassônico
  pinMode(TRIG1, OUTPUT);
  pinMode(ECHO1, INPUT);

  // HX711
  scale.begin(DOUT, SCK);
  Serial.println("Aguardando HX711...");
  unsigned long t = millis();
  while (!scale.is_ready() && millis() - t < 5000) {
    Serial.print(".");
    delay(300);
  }

  if (scale.is_ready()) {
    scale.set_scale();
    scale.tare();
    Serial.println("\nHX711 OK!");
    lcd.clear();
    lcd.print("Sistema pronto!");
  } else {
    Serial.println("\nERRO: HX711 nao respondeu!");
    lcd.clear();
    lcd.print("Erro balanca!");
  }

  // Wi-Fi e MQTT
  conectarWiFi();
  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  conectarMQTT();

  // Consulta inicial do clima
  consultarClima();
  ultimaConsultaClima = millis();

  delay(1000);
}

// ========================================================
void loop() {
  mqtt.loop();

  // Consulta clima a cada 5 minutos
  if (millis() - ultimaConsultaClima >= INTERVALO_CLIMA_MS) {
    consultarClima();
    ultimaConsultaClima = millis();
  }

  // Volume
  float distancia1 = lerDistancia(TRIG1, ECHO1);
  if (distancia1 < 0) distancia1 = CAPACIDADE_CM;

  int porcentagem = map((int)distancia1, 0, CAPACIDADE_CM, 100, 0);
  porcentagem = constrain(porcentagem, 0, 100);

  // Peso
  float peso = 0;
  if (scale.is_ready()) {
    peso = scale.get_units(10);
    if (peso < 0) peso = 0;
    if (peso > PESO_MAXIMO) peso = PESO_MAXIMO;
  } else {
    Serial.println("[ERRO] HX711 nao pronto!");
  }

  int porcentagemPeso = (int)((peso / PESO_MAXIMO) * 100);
  porcentagemPeso = constrain(porcentagemPeso, 0, 100);

  bool lixeiraCheia = (porcentagem >= 94 || porcentagemPeso >= 100);

  // Serial
  Serial.printf(">>> VOL: %d%% | PESO: %.1fg | %s | Chuva: %.0f%%\n",
    porcentagem, peso, lixeiraCheia ? "CHEIA" : "OK", chanceChuva);

  // LCD
  atualizarLCD(peso, porcentagem, lixeiraCheia);

  // Publica no ThingSpeak a cada 15 segundos (limite mínimo da plataforma)
  if (millis() - ultimaPublicacao >= INTERVALO_MQTT_MS) {
    publicarThingSpeak(porcentagem, peso, lixeiraCheia);
    ultimaPublicacao = millis();
  }

  delay(INTERVALO_SENSOR_MS);
}