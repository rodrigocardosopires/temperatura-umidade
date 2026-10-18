#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <Preferences.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#warning "secrets.h nao encontrado: usando secrets.example.h (copie e preencha suas credenciais)"
#include "secrets.example.h"
#endif

// Simulação no Wokwi (compilar com -DWOKWI): o simulador só tem DHT22 e a rede aberta "Wokwi-GUEST"
#ifdef WOKWI
#undef WIFI_SSID
#undef WIFI_SENHA
#define WIFI_SSID "Wokwi-GUEST"
#define WIFI_SENHA ""
#endif

// Exibida no Home Assistant (informações do dispositivo)
#define VERSAO_FIRMWARE "1.0.0"

// Leituras e diagnóstico no Monitor Serial (115200). Comente para desativar.
#define DEBUG

// Configurações do Display OLED
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
const uint32_t I2C_CLOCK = 400000UL;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET, I2C_CLOCK, I2C_CLOCK);

// Sensor DHT11 (módulo de 3 pinos: VCC, DATA, GND). O módulo já tem o resistor de pull-up no DATA.
// Não usar GPIO 0, 2, 5, 12 ou 15 (pinos de strapping: o pull-up do módulo pode impedir o boot).
const int PIN_DHT = 14;
#ifdef WOKWI
DHT dht(PIN_DHT, DHT22);
#else
DHT dht(PIN_DHT, DHT11);
#endif

// Ajuste (offset) padrão. Pode ser alterado pelo Home Assistant e fica salvo na flash.
// Temperatura em décimos de °C (15 = +1,5 °C); umidade em pontos percentuais.
const int OFFSET_TEMP_PADRAO_DEC = 0;
const int OFFSET_UMID_PADRAO = 0;

const int OFFSET_TEMP_LIMITE_DEC = 100;  // ±10,0 °C
const int OFFSET_UMID_LIMITE = 20;       // ±20 %

int offsetTempDec = OFFSET_TEMP_PADRAO_DEC;
int offsetUmid = OFFSET_UMID_PADRAO;

Preferences preferencias;

// O DHT11 aceita no máximo 1 leitura/s e a biblioteca devolve o valor em cache se ler antes de 2 s
const unsigned long INTERVALO_LEITURA_MS = 2500;

// Leituras inválidas seguidas até exibir erro (~4 x 2,5s = 10s)
const int MAX_FALHAS_SEGUIDAS = 4;

// Filtro: mediana das últimas leituras (remove leituras espúrias isoladas)
const int TAM_MEDIANA = 3;

float bufferTemp[TAM_MEDIANA];
float bufferUmid[TAM_MEDIANA];
int indiceMediana = 0;
int qtdMediana = 0;
float temperaturaBruta = 0;
float umidadeBruta = 0;
bool leituraIniciada = false;
int falhasSeguidas = 0;

// Estado de exibição (temperaturas em décimos de °C)
int temperaturaDec = 0;
int umidadePct = 0;
int sensacaoDec = 0;
bool sensorComErro = false;
bool precisaRedesenhar = false;

unsigned long ultimaLeitura = 0;

// Home Assistant via MQTT (descoberta automática)
const unsigned long INTERVALO_RECONEXAO_MQTT_MS = 10000;
const unsigned long INTERVALO_MIN_PUBLICACAO_MS = 2000;
const unsigned long INTERVALO_HEARTBEAT_MS = 60000;
const uint32_t TIMEOUT_CONEXAO_MQTT_MS = 1000;  // conexão bloqueia o loop; manter curto

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

char idDispositivo[20];  // ex.: temphumid_a1b2c3
char topicoEstado[48];
char topicoStatus[48];
char topicoCalibracao[48];
char topicoCmdOffsetTemp[64];
char topicoCmdOffsetUmid[64];

bool calibracaoPendente = false;

bool publicacaoPendente = false;
bool mqttConectadoNaTela = false;
bool wifiConectadoNaTela = false;

// Ícones 16x16 da faixa superior (1 bit por pixel, 2 bytes por linha)
const int ICONE_TAM = 16;
const int ICONE_WIFI_X = 0;
const int ICONE_HA_X = 18;
const int TEXTO_TOPO_X = 36;  // início da área de texto, à direita dos ícones

const uint8_t ICONE_WIFI[] PROGMEM = {
  0x00, 0x00, 0x07, 0xE0, 0x1F, 0xF8, 0x78, 0x1E,
  0xE0, 0x07, 0x87, 0xE1, 0x1F, 0xF8, 0x38, 0x1C,
  0x20, 0x04, 0x03, 0xC0, 0x07, 0xE0, 0x04, 0x20,
  0x00, 0x00, 0x01, 0x80, 0x03, 0xC0, 0x01, 0x80
};

const uint8_t ICONE_HOME_ASSISTANT[] PROGMEM = {
  0x01, 0x80, 0x03, 0xC0, 0x06, 0x60, 0x0C, 0x30,
  0x1B, 0xD8, 0x33, 0xCC, 0x6D, 0xB6, 0xCD, 0xB3,
  0x45, 0xA2, 0x43, 0xC2, 0x41, 0x82, 0x41, 0x82,
  0x41, 0x82, 0x41, 0x82, 0x7F, 0xFE, 0x00, 0x00
};
unsigned long ultimaTentativaMqtt = 0;
unsigned long ultimaPublicacao = 0;

bool offsetsValidos(int tempDec, int umid) {
  return abs(tempDec) <= OFFSET_TEMP_LIMITE_DEC && abs(umid) <= OFFSET_UMID_LIMITE;
}

void carregarCalibracao() {
  preferencias.begin("temphumid", false);
  int tempDec = preferencias.getInt("off_temp", OFFSET_TEMP_PADRAO_DEC);
  int umid = preferencias.getInt("off_umid", OFFSET_UMID_PADRAO);
  if (offsetsValidos(tempDec, umid)) {
    offsetTempDec = tempDec;
    offsetUmid = umid;
  }
}

float calcularMediana(const float* buffer) {
  float ordenado[TAM_MEDIANA];
  for (int i = 0; i < qtdMediana; i++) {
    float valor = buffer[i];
    int j = i - 1;
    while (j >= 0 && ordenado[j] > valor) {
      ordenado[j + 1] = ordenado[j];
      j--;
    }
    ordenado[j + 1] = valor;
  }
  return ordenado[qtdMediana / 2];
}

void atualizarValores() {
  if (!leituraIniciada) return;

  float temperatura = temperaturaBruta + offsetTempDec / 10.0f;
  float umidade = constrain(umidadeBruta + offsetUmid, 0.0f, 100.0f);
  float sensacao = dht.computeHeatIndex(temperatura, umidade, false);

  int novaTemperaturaDec = (int)lroundf(temperatura * 10);
  int novaUmidadePct = (int)lroundf(umidade);
  int novaSensacaoDec = (int)lroundf(sensacao * 10);

  if (novaTemperaturaDec != temperaturaDec || novaUmidadePct != umidadePct || novaSensacaoDec != sensacaoDec) {
    temperaturaDec = novaTemperaturaDec;
    umidadePct = novaUmidadePct;
    sensacaoDec = novaSensacaoDec;
    precisaRedesenhar = true;
    publicacaoPendente = true;
  }
}

void processarLeitura() {
  float temperatura = dht.readTemperature();
  float umidade = dht.readHumidity();

  if (isnan(temperatura) || isnan(umidade)) {
    if (falhasSeguidas < MAX_FALHAS_SEGUIDAS) falhasSeguidas++;
#ifdef DEBUG
    Serial.println(F("Falha na leitura do DHT11"));
#endif
  } else {
    falhasSeguidas = 0;

    bufferTemp[indiceMediana] = temperatura;
    bufferUmid[indiceMediana] = umidade;
    indiceMediana = (indiceMediana + 1) % TAM_MEDIANA;
    if (qtdMediana < TAM_MEDIANA) qtdMediana++;

    temperaturaBruta = calcularMediana(bufferTemp);
    umidadeBruta = calcularMediana(bufferUmid);
    leituraIniciada = true;

#ifdef DEBUG
    Serial.print(F("bruto: "));
    Serial.print(temperatura, 1);
    Serial.print(F("C "));
    Serial.print(umidade, 0);
    Serial.print(F("%  mediana: "));
    Serial.print(temperaturaBruta, 1);
    Serial.print(F("C "));
    Serial.print(umidadeBruta, 0);
    Serial.println(F("%"));
#endif
  }

  bool erro = falhasSeguidas >= MAX_FALHAS_SEGUIDAS;
  if (erro != sensorComErro) {
    sensorComErro = erro;
    if (erro) {
      // descarta leituras antigas para a mediana recomeçar só com dados após a falha
      qtdMediana = 0;
      indiceMediana = 0;
    }
    precisaRedesenhar = true;
    publicacaoPendente = true;
#ifdef DEBUG
    Serial.println(erro ? F("Sensor sem resposta") : F("Sensor normalizado"));
#endif
  }

  atualizarValores();
}

void aplicarCalibracao(int tempDec, int umid) {
  calibracaoPendente = true;  // republica sempre: em caso de recusa, o HA volta ao valor atual
  if (!offsetsValidos(tempDec, umid)) {
#ifdef DEBUG
    Serial.print(F("Ajuste recusado: temp(dec)="));
    Serial.print(tempDec);
    Serial.print(F(" umid="));
    Serial.println(umid);
#endif
    return;
  }
  if (tempDec == offsetTempDec && umid == offsetUmid) return;

  offsetTempDec = tempDec;
  offsetUmid = umid;
  preferencias.putInt("off_temp", tempDec);
  preferencias.putInt("off_umid", umid);
  atualizarValores();
}

void aoReceberMqtt(char* topico, byte* payload, unsigned int tamanho) {
  char texto[16];
  unsigned int n = min(tamanho, (unsigned int)sizeof(texto) - 1);
  memcpy(texto, payload, n);
  texto[n] = '\0';
  char* fim;
  float valor = strtof(texto, &fim);
  // o limite evita estouro no lroundf antes da validação dos offsets
  if (fim == texto || !isfinite(valor) || fabsf(valor) > 1000) {
    calibracaoPendente = true;  // payload inválido: devolve ao HA o valor atual
    return;
  }

  if (strcmp(topico, topicoCmdOffsetTemp) == 0) {
    aplicarCalibracao((int)lroundf(valor * 10), offsetUmid);
  } else if (strcmp(topico, topicoCmdOffsetUmid) == 0) {
    aplicarCalibracao(offsetTempDec, (int)lroundf(valor));
  }
}

void publicarConfig(const char* componente, const char* chave, const char* topicoDeEstado, const char* campos) {
  char topico[96];
  char payload[640];
  snprintf(topico, sizeof(topico), "homeassistant/%s/%s/%s/config", componente, idDispositivo, chave);
  snprintf(payload, sizeof(payload),
           "{\"uniq_id\":\"%s_%s\",\"stat_t\":\"%s\",\"avty_t\":\"%s\",%s,"
           "\"dev\":{\"ids\":[\"%s\"],\"name\":\"Temperatura e Umidade\",\"mf\":\"DIY\",\"mdl\":\"ESP32 DevKit V1 + DHT11\","
           "\"sw\":\"" VERSAO_FIRMWARE "\"}}",
           idDispositivo, chave, topicoDeEstado, topicoStatus, campos, idDispositivo);
  mqtt.publish(topico, payload, true);
}

void publicarConfigCalibracao(const char* chave, const char* nome, const char* icone, const char* topicoCmd,
                              const char* unidade, int limite, const char* passo) {
  char campos[320];
  snprintf(campos, sizeof(campos),
           "\"name\":\"%s\",\"cmd_t\":\"%s\",\"min\":%d,\"max\":%d,\"step\":%s,\"mode\":\"box\","
           "\"unit_of_meas\":\"%s\",\"ent_cat\":\"config\",\"ic\":\"%s\",\"val_tpl\":\"{{ value_json.%s }}\"",
           nome, topicoCmd, -limite, limite, passo, unidade, icone, chave);
  publicarConfig("number", chave, topicoCalibracao, campos);
}

void publicarCalibracao() {
  char payload[64];
  snprintf(payload, sizeof(payload), "{\"offset_temp\":%.1f,\"offset_umid\":%d}",
           offsetTempDec / 10.0f, offsetUmid);
  if (mqtt.publish(topicoCalibracao, payload, true)) {
    calibracaoPendente = false;
  }
}

void publicarDescoberta() {
  publicarConfigCalibracao("offset_temp", "Ajuste de temperatura", "mdi:thermometer-plus", topicoCmdOffsetTemp,
                           "°C", OFFSET_TEMP_LIMITE_DEC / 10, "0.1");
  publicarConfigCalibracao("offset_umid", "Ajuste de umidade", "mdi:water-plus", topicoCmdOffsetUmid,
                           "%", OFFSET_UMID_LIMITE, "1");
  publicarConfig("sensor", "temperatura", topicoEstado,
                 "\"name\":\"Temperatura\",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\",\"stat_cla\":\"measurement\","
                 "\"sug_dsp_prc\":1,\"val_tpl\":\"{{ value_json.temperatura }}\"");
  publicarConfig("sensor", "umidade", topicoEstado,
                 "\"name\":\"Umidade\",\"unit_of_meas\":\"%\",\"dev_cla\":\"humidity\",\"stat_cla\":\"measurement\","
                 "\"val_tpl\":\"{{ value_json.umidade }}\"");
  publicarConfig("sensor", "sensacao", topicoEstado,
                 "\"name\":\"Sensação térmica\",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\",\"stat_cla\":\"measurement\","
                 "\"sug_dsp_prc\":1,\"ic\":\"mdi:sun-thermometer\",\"val_tpl\":\"{{ value_json.sensacao }}\"");
  publicarConfig("binary_sensor", "erro", topicoEstado,
                 "\"name\":\"Erro do sensor\",\"dev_cla\":\"problem\",\"ent_cat\":\"diagnostic\","
                 "\"val_tpl\":\"{{ 'ON' if value_json.erro else 'OFF' }}\"");
}

void publicarEstado() {
  char payload[112];
  if (sensorComErro) {
    // null deixa as entidades como "desconhecido" no HA em vez de manter o último valor
    strcpy(payload, "{\"temperatura\":null,\"umidade\":null,\"sensacao\":null,\"erro\":true}");
  } else {
    snprintf(payload, sizeof(payload), "{\"temperatura\":%.1f,\"umidade\":%d,\"sensacao\":%.1f,\"erro\":false}",
             temperaturaDec / 10.0f, umidadePct, sensacaoDec / 10.0f);
  }
  // Atualiza o horário mesmo em falha, para não republicar a cada volta do loop
  ultimaPublicacao = millis();
  if (mqtt.publish(topicoEstado, payload, true)) {
    publicacaoPendente = false;
  }
}

void iniciarRede() {
  uint64_t mac = ESP.getEfuseMac();
  snprintf(idDispositivo, sizeof(idDispositivo), "temphumid_%02x%02x%02x",
           (uint8_t)(mac >> 24), (uint8_t)(mac >> 32), (uint8_t)(mac >> 40));
  snprintf(topicoEstado, sizeof(topicoEstado), "%s/estado", idDispositivo);
  snprintf(topicoStatus, sizeof(topicoStatus), "%s/status", idDispositivo);
  snprintf(topicoCalibracao, sizeof(topicoCalibracao), "%s/calibracao", idDispositivo);
  snprintf(topicoCmdOffsetTemp, sizeof(topicoCmdOffsetTemp), "%s/calibracao/offset_temp/set", idDispositivo);
  snprintf(topicoCmdOffsetUmid, sizeof(topicoCmdOffsetUmid), "%s/calibracao/offset_umid/set", idDispositivo);

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(idDispositivo);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_SENHA);

  wifiClient.setConnectionTimeout(TIMEOUT_CONEXAO_MQTT_MS);
  mqtt.setServer(MQTT_HOST, MQTT_PORTA);
  mqtt.setBufferSize(768);
  mqtt.setSocketTimeout(2);
  mqtt.setCallback(aoReceberMqtt);
}

void manterMqtt(unsigned long agora) {
  if (WiFi.status() != WL_CONNECTED) return;

  if (!mqtt.connected()) {
    if (ultimaTentativaMqtt != 0 && agora - ultimaTentativaMqtt < INTERVALO_RECONEXAO_MQTT_MS) return;
    ultimaTentativaMqtt = agora;

    if (!mqtt.connect(idDispositivo, MQTT_USUARIO, MQTT_SENHA, topicoStatus, 0, true, "offline")) {
#ifdef DEBUG
      Serial.print(F("MQTT falhou, rc="));
      Serial.println(mqtt.state());
#endif
      return;
    }
    mqtt.publish(topicoStatus, "online", true);
    publicarDescoberta();
    mqtt.subscribe(topicoCmdOffsetTemp);
    mqtt.subscribe(topicoCmdOffsetUmid);
    calibracaoPendente = true;
    publicacaoPendente = true;
    ultimaPublicacao = agora - INTERVALO_MIN_PUBLICACAO_MS;
  }

  mqtt.loop();

  if (calibracaoPendente) publicarCalibracao();

  if (!leituraIniciada && !sensorComErro) return;
  bool podePublicar = agora - ultimaPublicacao >= INTERVALO_MIN_PUBLICACAO_MS;
  if ((publicacaoPendente && podePublicar) || agora - ultimaPublicacao >= INTERVALO_HEARTBEAT_MS) {
    publicarEstado();
  }
}

void desenharErro() {
  display.setTextSize(2);
  display.setCursor(40, 22);
  display.print(F("ERRO"));
  display.setCursor(28, 44);
  display.print(F("SENSOR"));
}

void desenharGota(int x, int y) {
  display.fillTriangle(x + 5, y, x, y + 9, x + 10, y + 9, SSD1306_WHITE);
  display.fillCircle(x + 5, y + 10, 5, SSD1306_WHITE);
}

void desenharTermometro(int temperaturaDec) {
  const int tuboX = 10, tuboY = 18, tuboLargura = 11, tuboAltura = 36, tuboRaio = 5;
  const int bulboX = tuboX + tuboLargura / 2, bulboY = 56, bulboRaio = 7;
  display.drawRoundRect(tuboX, tuboY, tuboLargura, tuboAltura, tuboRaio, SSD1306_WHITE);
  display.fillCircle(bulboX, bulboY, bulboRaio, SSD1306_WHITE);

  // Coluna centralizada no tubo, nascendo dentro do bulbo; o topo para antes da curva do tubo
  const int colunaLargura = 5;
  const int colunaX = bulboX - colunaLargura / 2;
  const int colunaBase = bulboY - bulboRaio + 2;
  const int colunaTopo = tuboY + tuboRaio - 1;
  const int alturaMax = colunaBase - colunaTopo;

  // Coluna proporcional à faixa do DHT11 (0 a 50 °C)
  int altura = constrain(temperaturaDec * alturaMax / 500, 0, alturaMax);
  display.fillRect(colunaX, colunaBase - altura, colunaLargura, altura + 1, SSD1306_WHITE);
}

void desenharLeituras() {
  // ZONA AMARELA (Y: 0 a 15): gota + umidade, centralizados à direita dos ícones
  char texto[12];
  const int larguraGota = 11;
  const int espacoGota = 4;
  snprintf(texto, sizeof(texto), "%d%%", umidadePct);
  int larguraTexto = strlen(texto) * 12;  // fonte padrão 6px x tamanho 2
  int larguraTotal = larguraGota + espacoGota + larguraTexto;
  int x = TEXTO_TOPO_X + (SCREEN_WIDTH - TEXTO_TOPO_X - larguraTotal) / 2;
  desenharGota(x, 0);
  display.setTextSize(2);
  display.setCursor(x + larguraGota + espacoGota, 1);
  display.print(texto);

  // ZONA AZUL (Y: 16 a 63): termômetro + temperatura grande + sensação térmica
  desenharTermometro(temperaturaDec);

  snprintf(texto, sizeof(texto), "%.1f", temperaturaDec / 10.0f);
  // Com 5 caracteres (ex.: "-10.0") o valor invadiria o termômetro: mostra sem a casa decimal
  if (strlen(texto) > 4) snprintf(texto, sizeof(texto), "%.0f", temperaturaDec / 10.0f);
  larguraTexto = strlen(texto) * 18;     // tamanho 3
  larguraTotal = larguraTexto + 9 + 11;  // símbolo de grau + "C" em tamanho 2
  const int areaX = 26;
  x = areaX + (SCREEN_WIDTH - areaX - larguraTotal) / 2;
  const int y = 18;
  display.setTextSize(3);
  display.setCursor(x, y);
  display.print(texto);
  display.drawCircle(x + larguraTexto + 4, y + 3, 3, SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(x + larguraTexto + 9, y);
  display.print(F("C"));

  // Sensação térmica em tamanho 2: "ST" + valor + grau + "C"; o rótulo some se o valor tiver 5 caracteres
  snprintf(texto, sizeof(texto), "%.1f", sensacaoDec / 10.0f);
  const int larguraRotulo = 2 * 12 + 6;
  larguraTexto = strlen(texto) * 12;
  larguraTotal = larguraTexto + 7 + 12;
  bool cabeRotulo = larguraRotulo + larguraTotal <= SCREEN_WIDTH - areaX;
  if (cabeRotulo) larguraTotal += larguraRotulo;
  x = areaX + (SCREEN_WIDTH - areaX - larguraTotal) / 2;
  const int ySensacao = 47;
  display.setTextSize(2);
  if (cabeRotulo) {
    display.setCursor(x, ySensacao);
    display.print(F("ST"));
    x += larguraRotulo;
  }
  display.setCursor(x, ySensacao);
  display.print(texto);
  display.drawCircle(x + larguraTexto + 3, ySensacao + 2, 2, SSD1306_WHITE);
  display.setCursor(x + larguraTexto + 7, ySensacao);
  display.print(F("C"));
}

void desenharTela() {
  display.clearDisplay();
  if (sensorComErro) {
    desenharErro();
  } else {
    desenharLeituras();
  }
  if (wifiConectadoNaTela) {
    display.drawBitmap(ICONE_WIFI_X, 0, ICONE_WIFI, ICONE_TAM, ICONE_TAM, SSD1306_WHITE);
  }
  if (mqttConectadoNaTela) {
    display.drawBitmap(ICONE_HA_X, 0, ICONE_HOME_ASSISTANT, ICONE_TAM, ICONE_TAM, SSD1306_WHITE);
  }
  display.display();
}

void setup() {
  Serial.begin(115200);

  dht.begin();

  Wire.begin(21, 22);
  Wire.setClock(I2C_CLOCK);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C) && !display.begin(SSD1306_SWITCHCAPVCC, 0x3D)) {
    Serial.println(F("Display SSD1306 nao encontrado (0x3C/0x3D)"));
  }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(10, 0);
  display.println(F("TempHumid"));
  display.setCursor(10, 32);
  display.println(F("INICIANDO"));
  display.display();

  carregarCalibracao();
  iniciarRede();
  delay(1500);  // o DHT11 precisa de ~1 s após energizar antes da primeira leitura
}

void loop() {
  unsigned long agora = millis();

  if (ultimaLeitura == 0 || agora - ultimaLeitura >= INTERVALO_LEITURA_MS) {
    ultimaLeitura = agora;
    processarLeitura();
  }

  manterMqtt(agora);
  bool wifiConectado = WiFi.status() == WL_CONNECTED;
  if (wifiConectado != wifiConectadoNaTela) {
    wifiConectadoNaTela = wifiConectado;
    precisaRedesenhar = true;
  }
  if (mqtt.connected() != mqttConectadoNaTela) {
    mqttConectadoNaTela = mqtt.connected();
    precisaRedesenhar = true;
  }

  if (precisaRedesenhar && (leituraIniciada || sensorComErro)) {
    precisaRedesenhar = false;
    desenharTela();
  }
}
