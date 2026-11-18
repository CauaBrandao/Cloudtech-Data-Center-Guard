/*
 * ============================================================================
 *  CLOUDTECH SOLUTIONS  |  DATA CENTER GUARD  |  RACK A01 (unidade remota)
 * ============================================================================
 *  Projeto Final - Cloud Computing e Internet das Coisas (Senac - TSI 2026.2)
 *  Equipe: Cauã Brandão, Fernando Siqueira e Igor Aldivan
 *
 *  O que este firmware faz (ESP32 simulado no Wokwi):
 *   1. Lê a temperatura do DHT22 (GPIO 15) LOCALMENTE a cada 1 segundo.
 *   2. Publica 1 medida no feed "temperatura" do Adafruit IO a cada 4 segundos
 *      (60 s / 4 s = 15 pontos/min = 50% da cota gratuita de 30 pontos/min).
 *   3. Escuta o feed "led" (Toggle do Adafruit IO e botão do index.html) sem
 *      delay() bloqueante: mqtt.readSubscription(1000) dentro de um laço for.
 *   4. EDGE COMPUTING: se a temperatura chegar a >= 28 °C, o próprio ESP32
 *      liga o exaustor (LED no GPIO 2) e publica "1" no feed "led".
 *   5. TRAVA TÉRMICA: com a sala >= 28 °C, um pedido de desligar ("0") é
 *      recusado; o LED continua aceso e o ESP32 publica "1" de volta, fazendo
 *      os botões das duas telas "pularem" de volta para LIGADO.
 *   6. RETORNO AUTOMÁTICO: quando a sala esfria (< 28 °C), o ESP32 apaga o LED
 *      e publica "0", devolvendo os botões para DESLIGADO.
 *   7. FILTRO ANTI-ECO: o broker devolve ao ESP32 as mensagens que ele mesmo
 *      publicou no feed "led". A flag ignorarEco descarta esse eco.
 *
 *  Bibliotecas (Library Manager do Wokwi / arquivo libraries.txt):
 *   - Adafruit MQTT Library
 *   - DHT sensor library for ESPx
 *
 *  SEGURANÇA: a chave real do Adafruit IO só deve ser colocada localmente na
 *  hora da apresentação. No Wokwi público e no AVA fica "SUA_CHAVE_AQUI".
 * ============================================================================
 */

#include <WiFi.h>
#include "Adafruit_MQTT.h"
#include "Adafruit_MQTT_Client.h"
#include "DHTesp.h"

// ---------------------------------------------------------------------------
// 1) CONFIGURAÇÕES DE REDE E DA NUVEM
// ---------------------------------------------------------------------------
#define WLAN_SSID       "Wokwi-GUEST"     // Wi-Fi virtual do Wokwi (sem senha)
#define WLAN_PASS       ""
#define WLAN_CANAL      6                 // canal 6 = conexão instantânea no Wokwi

#define AIO_SERVER      "io.adafruit.com"
#define AIO_SERVERPORT  1883              // MQTT sem TLS (o Wokwi-GUEST permite)
#define AIO_USERNAME    "SEU_USUARIO_AQUI"
#define AIO_KEY         "SUA_CHAVE_AQUI"

// ---------------------------------------------------------------------------
// 2) HARDWARE E REGRAS DE NEGÓCIO
// ---------------------------------------------------------------------------
#define PINO_DHT            15     // DHT22 -> SDA no GPIO 15
#define PINO_LED            2      // LED vermelho (exaustor) -> resistor 220 Ω -> GPIO 2
#define LIMITE_TEMP         28.0   // °C: a partir daqui o Edge assume o controle
#define CICLOS_POR_ENVIO    4      // 4 ciclos de 1 s = 1 publicação a cada 4 s
#define JANELA_ECO_MS       5000   // tempo máximo para o eco voltar do broker

// ---------------------------------------------------------------------------
// 3) OBJETOS DE REDE, MQTT E SENSOR
// ---------------------------------------------------------------------------
WiFiClient client;
Adafruit_MQTT_Client mqtt(&client, AIO_SERVER, AIO_SERVERPORT, AIO_USERNAME, AIO_KEY);

// Feed de telemetria (somente publica)
Adafruit_MQTT_Publish temp_feed = Adafruit_MQTT_Publish(&mqtt, AIO_USERNAME "/feeds/temperatura");

// Feed do exaustor: o ESP32 OUVE (cliques humanos) e também PUBLICA
// (para mover os botões sozinho nas duas telas). Por isso, dois objetos.
Adafruit_MQTT_Subscribe led_sub = Adafruit_MQTT_Subscribe(&mqtt, AIO_USERNAME "/feeds/led");
Adafruit_MQTT_Publish   led_pub = Adafruit_MQTT_Publish(&mqtt, AIO_USERNAME "/feeds/led");

DHTesp dht;

// ---------------------------------------------------------------------------
// 4) VARIÁVEIS DE ESTADO
// ---------------------------------------------------------------------------
float temperaturaAtual = NAN;   // última leitura válida do DHT22
float umidadeAtual     = NAN;
bool  ledLigado        = false; // estado físico do exaustor (LED)
bool  emEmergencia     = false; // true enquanto a sala estiver >= 28 °C

// Filtro anti-eco: guarda o que o ESP32 acabou de publicar no feed "led"
bool          ignorarEco    = false;
char          valorEco      = '0';  // '1' ou '0'
unsigned long instanteEco   = 0;

// Sincronização: quando conecta (ou reconecta) o ESP32 publica o estado real
// do LED para que as duas telas nunca mostrem algo diferente do hardware.
bool precisaSincronizar = true;

unsigned long totalPublicacoes = 0;

// ---------------------------------------------------------------------------
// 5) FUNÇÕES AUXILIARES
// ---------------------------------------------------------------------------

// Liga/desliga o exaustor fisicamente (só escreve no pino se mudou)
void acionarExaustor(bool ligar, const char *motivo) {
  if (ligar == ledLigado) return;
  ledLigado = ligar;
  digitalWrite(PINO_LED, ligar ? HIGH : LOW);
  Serial.printf("[EXAUSTOR] %s  (%s)\n", ligar ? "LIGADO" : "DESLIGADO", motivo);
}

// Publica "1" ou "0" no feed led e arma o filtro anti-eco
void publicarLed(bool ligado) {
  const char *valor = ligado ? "1" : "0";
  if (!mqtt.connected()) {          // sem nuvem: sincroniza quando voltar
    precisaSincronizar = true;
    return;
  }
  if (led_pub.publish(valor)) {
    ignorarEco  = true;              // o broker vai devolver esta mensagem
    valorEco    = valor[0];
    instanteEco = millis();
    precisaSincronizar = false;
    Serial.printf("[MQTT] feed led <- \"%s\" (sincronizando as duas telas)\n", valor);
  } else {
    precisaSincronizar = true;
    Serial.println("[MQTT] falha ao publicar no feed led (tentará de novo)");
  }
}

// Lê o DHT22. resetTimer() força uma leitura nova a cada 1 s no simulador
// (a biblioteca, por padrão, só relê o DHT22 a cada 2 s).
void lerSensor() {
  dht.resetTimer();
  TempAndHumidity leitura = dht.getTempAndHumidity();
  if (dht.getStatus() != DHTesp::ERROR_NONE || isnan(leitura.temperature)) {
    Serial.printf("[DHT22] leitura inválida (%s) - mantendo a última válida\n",
                  dht.getStatusString());
    return;
  }
  temperaturaAtual = leitura.temperature;
  umidadeAtual     = leitura.humidity;
}

// DESAFIO 2 - EDGE COMPUTING: decisão tomada no próprio ESP32, sem nuvem
void verificarRegraEdge() {
  if (isnan(temperaturaAtual)) return;

  if (temperaturaAtual >= LIMITE_TEMP) {
    if (!emEmergencia) {
      // Acabou de entrar em emergência: liga e avisa as duas telas
      emEmergencia = true;
      Serial.printf("\n[EDGE] >>> ALERTA TÉRMICO: %.1f °C >= %.1f °C <<<\n",
                    temperaturaAtual, LIMITE_TEMP);
      acionarExaustor(true, "disparo automático Edge");
      publicarLed(true);
    } else {
      acionarExaustor(true, "sala continua quente");  // garante LED aceso
    }
  } else if (emEmergencia) {
    // Esfriou: sai da emergência, apaga sozinho e avisa as duas telas
    emEmergencia = false;
    Serial.printf("\n[EDGE] Temperatura normalizada: %.1f °C < %.1f °C\n",
                  temperaturaAtual, LIMITE_TEMP);
    acionarExaustor(false, "retorno automático ao esfriar");
    publicarLed(false);
  }
}

// DESAFIO 3 - Trata um comando que chegou pelo feed "led"
void tratarComando(const char *mensagem) {
  // Aceita "1"/"0" e também "ON"/"OFF" (qualquer combinação de maiúsculas)
  bool pedidoLigar;
  if (strcmp(mensagem, "1") == 0 || strcasecmp(mensagem, "ON") == 0) {
    pedidoLigar = true;
  } else if (strcmp(mensagem, "0") == 0 || strcasecmp(mensagem, "OFF") == 0) {
    pedidoLigar = false;
  } else {
    Serial.printf("[MQTT] comando desconhecido ignorado: \"%s\"\n", mensagem);
    return;
  }

  // FILTRO ANTI-ECO: é a própria mensagem que o ESP32 acabou de publicar?
  if (ignorarEco && (millis() - instanteEco) < JANELA_ECO_MS &&
      (pedidoLigar ? '1' : '0') == valorEco) {
    ignorarEco = false;
    Serial.printf("[ANTI-ECO] \"%s\" é eco da nossa publicação - ignorado\n", mensagem);
    return;
  }
  ignorarEco = false;  // janela expirou ou chegou outro valor: desarma o filtro

  Serial.printf("[COMANDO] Pedido recebido: %s (sala a %.1f °C)\n",
                pedidoLigar ? "LIGAR" : "DESLIGAR", temperaturaAtual);

  if (pedidoLigar) {
    acionarExaustor(true, "comando manual");
  } else if (!isnan(temperaturaAtual) && temperaturaAtual >= LIMITE_TEMP) {
    // TRAVA DE SEGURANÇA TÉRMICA: recusa o desligamento com a sala quente
    Serial.println("[TRAVA] Desligamento RECUSADO: sala >= 28 °C. "
                   "Exaustor continua LIGADO e o botão volta para 1.");
    acionarExaustor(true, "trava térmica");
    publicarLed(true);   // faz o botão "pular de volta" nas duas telas
  } else {
    acionarExaustor(false, "comando manual");
  }
}

// DESAFIO 4 - Publica 1 medida de temperatura (chamado a cada 4 s)
void publicarTemperatura() {
  if (isnan(temperaturaAtual)) return;
  if (temp_feed.publish(temperaturaAtual, 1)) {   // 1 casa decimal
    totalPublicacoes++;
    Serial.printf("[TELEMETRIA #%lu] temperatura <- %.1f °C | umidade %.0f%% | "
                  "exaustor %s | modo %s\n",
                  totalPublicacoes, temperaturaAtual, umidadeAtual,
                  ledLigado ? "LIGADO" : "DESLIGADO",
                  emEmergencia ? "EDGE/TRAVA" : "MANUAL");
  } else {
    Serial.println("[TELEMETRIA] falha ao publicar temperatura");
  }
}

// Garante o Wi-Fi conectado (canal 6 = conexão rápida no Wokwi)
void conectarWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.print("[WiFi] Conectando a " WLAN_SSID);
  WiFi.begin(WLAN_SSID, WLAN_PASS, WLAN_CANAL);
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print(".");
  }
  Serial.print(" OK! IP: ");
  Serial.println(WiFi.localIP());
}

// Conecta ao broker MQTT do Adafruit IO. Enquanto a nuvem estiver fora,
// o ESP32 CONTINUA protegendo o rack (Edge funciona mesmo offline).
void MQTT_connect() {
  if (mqtt.connected()) return;
  conectarWiFi();

  Serial.print("[MQTT] Conectando ao Adafruit IO... ");
  int8_t ret;
  while ((ret = mqtt.connect()) != 0) {
    Serial.printf("erro: %s. Nova tentativa em 3 s (Edge segue ativo)\n",
                  mqtt.connectErrorString(ret));
    mqtt.disconnect();
    for (int i = 0; i < 3; i++) {   // 3 s de espera sem abandonar o rack
      lerSensor();
      verificarRegraEdge();
      delay(1000);
    }
    conectarWiFi();
  }
  Serial.println("conectado!");
  precisaSincronizar = true;          // republica o estado real do LED
}

// ---------------------------------------------------------------------------
// 6) SETUP
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n==============================================");
  Serial.println("  CloudTech Solutions - Data Center Guard A01");
  Serial.println("  ESP32 + DHT22 + Exaustor | Adafruit IO MQTT");
  Serial.println("==============================================");

  pinMode(PINO_LED, OUTPUT);
  digitalWrite(PINO_LED, LOW);

  dht.setup(PINO_DHT, DHTesp::DHT22);

  mqtt.subscribe(&led_sub);           // a inscrição precisa vir antes do connect

  conectarWiFi();
  lerSensor();                        // primeira leitura antes de ir para a nuvem
  verificarRegraEdge();
}

// ---------------------------------------------------------------------------
// 7) LOOP PRINCIPAL - ciclo padrão de 4 segundos
// ---------------------------------------------------------------------------
void loop() {
  MQTT_connect();

  // Ao (re)conectar, publica o estado real do LED para alinhar as duas telas
  if (precisaSincronizar) {
    publicarLed(ledLigado);
  }

  // 4 ciclos de 1 s = 4 s entre cada envio de temperatura (15 pontos/min)
  for (int i = 0; i < CICLOS_POR_ENVIO; i++) {

    // DESAFIO 1: leitura local do DHT22 a cada 1 s
    lerSensor();

    // DESAFIO 2: regra Edge (liga sozinho em até 1 s ao passar de 28 °C)
    verificarRegraEdge();

    // DESAFIO 3: escuta cliques por até 1000 ms (substitui o delay(1000))
    unsigned long inicioEscuta = millis();
    Adafruit_MQTT_Subscribe *subscription;
    while ((subscription = mqtt.readSubscription(1000))) {
      if (subscription == &led_sub) {
        tratarComando((char *)led_sub.lastread);
      }
    }
    // Se a conexão caiu, readSubscription volta na hora: mantém o ritmo de 1 s
    unsigned long decorrido = millis() - inicioEscuta;
    if (decorrido < 1000) delay(1000 - decorrido);
  }

  // DESAFIO 4: passados os 4 segundos, publica 1 medida de temperatura
  publicarTemperatura();
}
