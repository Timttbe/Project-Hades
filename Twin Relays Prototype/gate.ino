/*
 * ESP-01 RELAY COM AUTO-DISCOVERY
 * Este ESP responde automaticamente a broadcasts de descoberta
 * e se conecta com o servidor sem configuração manual
 * VERSÃO TOTALMENTE CORRIGIDA - Timer isolado e preciso
 */

#include <ESP8266WiFi.h>
#include <espnow.h>

// Pino do relay (GPIO 0 no ESP-01)
#define RELAY_PIN 0

// LED interno para indicação visual (GPIO 2)
#define LED_PIN 2

// Configurações do timer - Timer mais preciso
#define TIMER_DURATION 5000  // Exatos 5 segundos

// Estrutura das mensagens (deve ser igual ao servidor)
typedef struct {
  uint8_t msgType;  // 1=discovery, 2=discovery_response, 3=relay_command
  uint8_t deviceType; // 1=server, 2=relay
  char deviceName[32];
  bool relayState;
  int command;
} ESPMessage;

ESPMessage incomingMsg;
ESPMessage outgoingMsg;

// Status atual do relay
bool currentRelayState = false;
unsigned long lastCommandTime = 0;
int commandCount = 0;
bool connectedToServer = false;
uint8_t serverMAC[6];
unsigned long lastDiscoveryResponse = 0;

// Variáveis do timer para acionamento temporizado - ISOLADAS
bool timerActive = false;
unsigned long timerStart = 0;
bool forceRelayOff = false;

// Variáveis para LED não interferir no timer
unsigned long lastLedBlink = 0;
bool ledState = HIGH; // LED invertido no ESP-01

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n=== ESP-01 Relay Timer CORRIGIDO ===");
  
  // Configurar pinos
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  
  // Estado inicial: relay desligado
  digitalWrite(RELAY_PIN, LOW);
  digitalWrite(LED_PIN, HIGH); // LED interno invertido (HIGH = apagado)
  
  // Configurar WiFi para ESP-NOW
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  
  Serial.print("📡 MAC Address deste ESP: ");
  Serial.println(WiFi.macAddress());
  Serial.println("🔍 Aguardando descoberta automática...");
  
  // Inicializar ESP-NOW
  initESPNow();
  
  // Piscar LED para indicar inicialização (sem delay no loop)
  for (int i = 0; i < 3; i++) {
    digitalWrite(LED_PIN, LOW);
    delay(200);
    digitalWrite(LED_PIN, HIGH);
    delay(200);
  }
  
  Serial.println("✅ Sistema pronto - aguardando comandos...");
  Serial.println("⏰ Timer configurado para: " + String(TIMER_DURATION) + "ms EXATOS");
}

void loop() {
  // ===== VERIFICAÇÃO DO TIMER - PRIORIDADE ABSOLUTA =====
  if (timerActive) {
    unsigned long elapsed = millis() - timerStart;
    
    // Verificar se expirou COM PRECISÃO
    if (elapsed >= TIMER_DURATION) {
      Serial.println("\n==========================================");
      Serial.println("⏰ TIMER EXPIROU - DESLIGANDO RELAY AGORA");
      Serial.println("   Tempo decorrido: " + String(elapsed) + "ms");
      Serial.println("   Esperado: " + String(TIMER_DURATION) + "ms");
      Serial.println("==========================================");
      
      // FORÇAR desligamento imediato
      timerActive = false;
      currentRelayState = false;
      digitalWrite(RELAY_PIN, LOW);
      
      // LED volta ao estado "conectado" (apagado)
      digitalWrite(LED_PIN, HIGH);
      
      Serial.println("🔴 RELAY DESLIGADO - Timer concluído");
    }
    
    // Status do timer a cada segundo (SEM interferir no timing)
    static unsigned long lastTimerStatus = 0;
    if (millis() - lastTimerStatus >= 1000) {
      unsigned long remaining = TIMER_DURATION - elapsed;
      if (remaining <= TIMER_DURATION) { // Evitar underflow
        Serial.println("⏱️ Timer: " + String(remaining) + "ms restantes");
      }
      lastTimerStatus = millis();
    }
  }
  
  // ===== LED STATUS (sem interferir no timer) =====
  if (millis() - lastLedBlink >= (connectedToServer ? 2000 : 500)) {
    ledState = !ledState;
    if (!timerActive) { // Só controlar LED se timer não estiver ativo
      digitalWrite(LED_PIN, ledState);
    }
    lastLedBlink = millis();
  }
  
  // Status periódico (reduzido para não interferir)
  static unsigned long lastStatus = 0;
  if (millis() - lastStatus >= 10000) { // A cada 10 segundos
    showStatus();
    lastStatus = millis();
  }
  
  // DELAY MÍNIMO para não sobrecarregar CPU
  delay(10);
}

void initESPNow() {
  if (esp_now_init() != 0) {
    Serial.println("❌ Erro ao inicializar ESP-NOW");
    delay(1000);
    ESP.restart();
    return;
  }
  
  Serial.println("✅ ESP-NOW inicializado com sucesso");
  
  // Configurar como COMBO para poder enviar e receber
  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_register_send_cb(OnDataSent);
  esp_now_register_recv_cb(OnDataRecv);
  
  // Adicionar broadcast para receber descobertas
  uint8_t broadcastMAC[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  esp_now_add_peer(broadcastMAC, ESP_NOW_ROLE_COMBO, 1, NULL, 0);
  
  Serial.println("🎧 Escutando broadcasts de descoberta...");
}

void OnDataSent(uint8_t *mac_addr, uint8_t sendStatus) {
  if (sendStatus == 0) {
    Serial.println("✅ Resposta de descoberta enviada");
  } else {
    Serial.println("❌ Falha ao enviar resposta: " + String(sendStatus));
  }
}

void OnDataRecv(uint8_t *mac, uint8_t *incomingData, uint8_t len) {
  memcpy(&incomingMsg, incomingData, sizeof(incomingMsg));
  
  // Processar descoberta do servidor
  if (incomingMsg.msgType == 1 && incomingMsg.deviceType == 1) {
    Serial.println("\n🎯 Descoberta recebida do servidor!");
    Serial.print("   Nome: ");
    Serial.println(incomingMsg.deviceName);
    Serial.print("   MAC: ");
    printMAC(mac);
    
    // Responder apenas uma vez a cada 2 segundos para evitar spam
    if (millis() - lastDiscoveryResponse > 2000) {
      respondToDiscovery(mac);
      lastDiscoveryResponse = millis();
    }
    return;
  }
  
  // Processar comando de relay
  if (incomingMsg.msgType == 3) {
    commandCount++;
    lastCommandTime = millis();
    
    // Marcar como conectado se não estava
    if (!connectedToServer) {
      memcpy(serverMAC, mac, 6);
      connectedToServer = true;
      Serial.println("🔗 Conectado ao servidor!");
    }
    
    Serial.println("\n📡 Comando recebido:");
    Serial.print("   MAC Origem: ");
    printMAC(mac);
    Serial.println("   Estado: " + String(incomingMsg.relayState ? "LIGAR" : "DESLIGAR"));
    Serial.println("   Comando: " + String(incomingMsg.command));
    Serial.println("   Total comandos: " + String(commandCount));
    
    // ===== COMANDO TEMPORIZADO ESPECIAL =====
    if (incomingMsg.command == 5 && incomingMsg.relayState) {
      Serial.println("\n🚀 COMANDO TEMPORIZADO DETECTADO!");
      Serial.println("==========================================");
      Serial.println("⚡ INICIANDO TIMER DE " + String(TIMER_DURATION) + "ms");
      Serial.println("==========================================");
      
      // RESETAR qualquer timer anterior
      timerActive = false;
      
      // LIGAR RELAY IMEDIATAMENTE
      currentRelayState = true;
      digitalWrite(RELAY_PIN, HIGH);
      digitalWrite(LED_PIN, LOW); // LED ligado (invertido)
      
      // INICIAR TIMER COM TIMESTAMP PRECISO
      timerStart = millis();
      timerActive = true;
      
      Serial.println("✅ RELAY LIGADO - Timer iniciado às " + String(timerStart) + "ms");
      Serial.println("🎯 Desligamento programado para: " + String(timerStart + TIMER_DURATION) + "ms");
      
    } else if (incomingMsg.command != 5) {
      // Comando normal (não temporizado)
      Serial.println("📝 Comando normal - controlando relay diretamente");
      controlRelayNormal(incomingMsg.relayState);
    }
  }
}

void respondToDiscovery(uint8_t* serverMac) {
  // Adicionar servidor como peer se não foi adicionado
  if (!connectedToServer) {
    esp_now_add_peer(serverMac, ESP_NOW_ROLE_COMBO, 1, NULL, 0);
    Serial.println("📄 Servidor adicionado como peer");
  }
  
  // Preparar resposta
  outgoingMsg.msgType = 2; // discovery_response
  outgoingMsg.deviceType = 2; // relay
  strcpy(outgoingMsg.deviceName, "ESP-Relay-01");
  outgoingMsg.relayState = currentRelayState;
  outgoingMsg.command = 0;
  
  // Enviar resposta
  uint8_t result = esp_now_send(serverMac, (uint8_t*)&outgoingMsg, sizeof(outgoingMsg));
  
  if (result == 0) {
    Serial.println("📤 Resposta de descoberta enviada");
  } else {
    Serial.println("❌ Erro ao enviar resposta: " + String(result));
  }
}

// Função para comando normal (não temporizado)
void controlRelayNormal(bool state) {
  // NUNCA interferir se timer estiver ativo
  if (timerActive) {
    Serial.println("⚠️ TIMER ATIVO - Comando normal IGNORADO");
    return;
  }
  
  currentRelayState = state;
  digitalWrite(RELAY_PIN, state ? HIGH : LOW);
  
  if (state) {
    digitalWrite(LED_PIN, LOW); // LED ligado (invertido)
  } else {
    digitalWrite(LED_PIN, HIGH); // LED desligado
  }
  
  Serial.println("==========================================");
  Serial.println(state ? "⚡ RELAY LIGADO (normal)" : "🔴 RELAY DESLIGADO (normal)");
  Serial.println("   GPIO 0: " + String(state ? "HIGH" : "LOW"));
  Serial.println("   Timer ativo: NÃO");
  Serial.println("==========================================");
}

void printMAC(uint8_t* mac) {
  for (int i = 0; i < 6; i++) {
    Serial.printf("%02X", mac[i]);
    if (i < 5) Serial.print(":");
  }
  Serial.println();
}

void showStatus() {
  Serial.println("\n📊 STATUS DO SISTEMA:");
  Serial.println("   🔗 Conectado: " + String(connectedToServer ? "SIM" : "NÃO"));
  Serial.println("   ⚡ Relay: " + String(currentRelayState ? "LIGADO" : "DESLIGADO"));
  Serial.println("   🔍 GPIO 0: " + String(digitalRead(RELAY_PIN) ? "HIGH" : "LOW"));
  
  if (timerActive) {
    unsigned long elapsed = millis() - timerStart;
    unsigned long remaining = TIMER_DURATION - elapsed;
    Serial.println("   ⏰ Timer: ATIVO");
    Serial.println("   ⏱️ Decorrido: " + String(elapsed) + "ms");
    Serial.println("   ⏳ Restante: " + String(remaining) + "ms");
    Serial.println("   🎯 Iniciado em: " + String(timerStart) + "ms");
  } else {
    Serial.println("   ⏰ Timer: INATIVO");
  }
  
  Serial.println("   📡 Comandos: " + String(commandCount));
  
  if (connectedToServer) {
    Serial.println("   🕐 Último comando: " + String((millis() - lastCommandTime) / 1000) + "s atrás");
  }
  
  Serial.println("   📶 MAC: " + WiFi.macAddress());
  Serial.println("   ⏱️ Uptime: " + String(millis() / 1000) + "s");
  Serial.println();
}