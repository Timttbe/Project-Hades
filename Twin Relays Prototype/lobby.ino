/*
 * ESP-01 SERVIDOR WEB COM ESP-NOW AUTO-DISCOVERY
 * Este ESP cria um ponto de acesso Wi-Fi, encontra automaticamente
 * outros ESPs e se conecta via ESP-NOW sem configuração manual
 * Versão CORRIGIDA - Timer apenas para UI, não controla relay remoto
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <espnow.h>

// Configurações do Access Point
const char* ssid = "ESP-Relay-Control";
const char* password = "12345678";

// Estrutura para descoberta de dispositivos
typedef struct {
  uint8_t msgType;  // 1=discovery, 2=discovery_response, 3=relay_command
  uint8_t deviceType; // 1=server, 2=relay
  char deviceName[32];
  bool relayState;
  int command;
} ESPMessage;

// Lista de dispositivos encontrados
struct Device {
  uint8_t mac[6];
  char name[32];
  uint8_t type;
  bool connected;
  unsigned long lastSeen;
};

Device foundDevices[10];
int deviceCount = 0;
bool discoveryActive = false;
unsigned long lastDiscovery = 0;

ESPMessage outgoingMsg;
ESPMessage incomingMsg;

ESP8266WebServer server(80);

// Status do relay (para controle local)
bool espNowInitialized = false;
bool hasRelayDevice = false;
uint8_t relayMAC[6];

// Controle do timer APENAS PARA INTERFACE (não controla relay remoto)
bool buttonDisabled = false;
unsigned long buttonDisabledStart = 0;
const unsigned long BUTTON_DISABLE_DURATION = 5500; // 5.5 segundos para garantir

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n=== ESP-01 Servidor Web com Auto-Discovery CORRIGIDO ===");
  
  // Configurar como Access Point E Station (necessário para ESP-NOW)
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(ssid, password);
  
  IPAddress IP = WiFi.softAPIP();
  Serial.print("🔶 IP do Access Point: ");
  Serial.println(IP);
  Serial.print("📡 MAC do Servidor: ");
  Serial.println(WiFi.macAddress());
  
  // Inicializar ESP-NOW
  initESPNow();
  
  // Iniciar descoberta automática
  startAutoDiscovery();
  
  // Configurar rotas do servidor web
  server.on("/", handleRoot);
  server.on("/relay/pulse", handleRelayPulse);
  server.on("/status", handleStatus);
  server.on("/scan", handleScan);
  
  server.begin();
  Serial.println("🌐 Servidor web iniciado!");
  Serial.println("   Conecte-se à rede: " + String(ssid));
  Serial.println("   Senha: " + String(password));
  Serial.println("   Acesse: http://" + IP.toString());
  Serial.println("\n🔍 Iniciando busca automática por dispositivos relay...");
}

void loop() {
  server.handleClient();
  
  // Verificar se o botão deve ser reabilitado (APENAS PARA UI)
  if (buttonDisabled && (millis() - buttonDisabledStart >= BUTTON_DISABLE_DURATION)) {
    buttonDisabled = false;
    Serial.println("⏰ Botão reabilitado - relay deve ter desligado automaticamente");
  }
  
  // Auto-discovery periódico
  if (millis() - lastDiscovery > 5000) {  // A cada 5 segundos
    if (!hasRelayDevice) {
      broadcastDiscovery();
    }
    lastDiscovery = millis();
  }
  
  // Limpar dispositivos antigos (não vistos há 30 segundos)
  cleanupDevices();
  
  delay(10);
}

void initESPNow() {
  if (esp_now_init() != 0) {
    Serial.println("❌ Erro ao inicializar ESP-NOW");
    return;
  }
  
  Serial.println("✅ ESP-NOW inicializado com sucesso");
  
  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_register_send_cb(OnDataSent);
  esp_now_register_recv_cb(OnDataRecv);
  
  espNowInitialized = true;
}

void startAutoDiscovery() {
  // Adicionar peer broadcast para descoberta
  uint8_t broadcastMAC[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  esp_now_add_peer(broadcastMAC, ESP_NOW_ROLE_COMBO, 1, NULL, 0);
  discoveryActive = true;
}

void broadcastDiscovery() {
  if (!espNowInitialized) return;
  
  // Preparar mensagem de descoberta
  outgoingMsg.msgType = 1; // discovery
  outgoingMsg.deviceType = 1; // server
  strcpy(outgoingMsg.deviceName, "ESP-Server");
  outgoingMsg.relayState = false;
  outgoingMsg.command = 0;
  
  // Enviar broadcast
  uint8_t broadcastMAC[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  esp_now_send(broadcastMAC, (uint8_t*)&outgoingMsg, sizeof(outgoingMsg));
  
  Serial.println("📡 Broadcast de descoberta enviado...");
}

void OnDataSent(uint8_t *mac_addr, uint8_t sendStatus) {
  if (sendStatus == 0) {
    // Só mostrar sucesso para comandos de relay, não para discovery
    if (outgoingMsg.msgType == 3) {
      Serial.println("✅ Comando enviado com sucesso");
    }
  } else {
    Serial.println("❌ Falha no envio: " + String(sendStatus));
  }
}

void OnDataRecv(uint8_t *mac, uint8_t *incomingData, uint8_t len) {
  memcpy(&incomingMsg, incomingData, sizeof(incomingMsg));
  
  // Processar descoberta response
  if (incomingMsg.msgType == 2 && incomingMsg.deviceType == 2) {
    Serial.println("\n🎯 Dispositivo Relay encontrado!");
    Serial.print("   Nome: ");
    Serial.println(incomingMsg.deviceName);
    Serial.print("   MAC: ");
    printMAC(mac);
    
    // Adicionar como peer se ainda não foi adicionado
    if (!hasRelayDevice) {
      memcpy(relayMAC, mac, 6);
      esp_now_add_peer(mac, ESP_NOW_ROLE_COMBO, 1, NULL, 0);
      hasRelayDevice = true;
      Serial.println("✅ Relay conectado automaticamente!");
      
      // Adicionar à lista de dispositivos
      addDevice(mac, incomingMsg.deviceName, incomingMsg.deviceType);
    }
  }
}

void addDevice(uint8_t* mac, const char* name, uint8_t type) {
  // Verificar se já existe
  for (int i = 0; i < deviceCount; i++) {
    if (memcmp(foundDevices[i].mac, mac, 6) == 0) {
      foundDevices[i].lastSeen = millis();
      foundDevices[i].connected = true;
      return;
    }
  }
  
  // Adicionar novo dispositivo
  if (deviceCount < 10) {
    memcpy(foundDevices[deviceCount].mac, mac, 6);
    strcpy(foundDevices[deviceCount].name, name);
    foundDevices[deviceCount].type = type;
    foundDevices[deviceCount].connected = true;
    foundDevices[deviceCount].lastSeen = millis();
    deviceCount++;
  }
}

void cleanupDevices() {
  for (int i = 0; i < deviceCount; i++) {
    if (millis() - foundDevices[i].lastSeen > 30000) {
      foundDevices[i].connected = false;
    }
  }
}

void sendRelayCommand(bool state, int cmd) {
  if (!espNowInitialized || !hasRelayDevice) {
    Serial.println("❌ Nenhum relay conectado!");
    return;
  }
  
  outgoingMsg.msgType = 3; // relay_command
  outgoingMsg.deviceType = 1; // server
  strcpy(outgoingMsg.deviceName, "ESP-Server");
  outgoingMsg.relayState = state;
  outgoingMsg.command = cmd;
  
  uint8_t result = esp_now_send(relayMAC, (uint8_t*)&outgoingMsg, sizeof(outgoingMsg));
  
  if (result == 0) {
    Serial.println("📡 Comando " + String(state ? "LIGAR" : "DESLIGAR") + " enviado");
  } else {
    Serial.println("❌ Erro ao enviar comando");
  }
}

void printMAC(uint8_t* mac) {
  for (int i = 0; i < 6; i++) {
    Serial.printf("%02X", mac[i]);
    if (i < 5) Serial.print(":");
  }
  Serial.println();
}

// Comando temporizado para o relay (5 segundos)
void handleRelayPulse() {
  if (!hasRelayDevice) {
    server.send(400, "text/plain", "Nenhum relay conectado");
    return;
  }
  
  // Verificar se o botão ainda está desabilitado
  if (buttonDisabled) {
    server.send(400, "text/plain", "Relay ainda em ativação, aguarde...");
    return;
  }
  
  Serial.println("⚡ ACIONAMENTO TEMPORIZADO - O relay controlará seu próprio timer");
  
  // Desabilitar botão APENAS para interface (não controla relay remoto)
  buttonDisabled = true;
  buttonDisabledStart = millis();
  
  // Enviar comando - O RELAY se encarregará do timer
  sendRelayCommand(true, 5);  // comando especial = 5 (indica temporizado)
  
  server.sendHeader("Location", "/");
  server.send(303);
}

// Forçar nova busca (desconectar atual)
void handleScan() {
  Serial.println("🔄 Forçando reconexão...");
  hasRelayDevice = false;
  deviceCount = 0;
  buttonDisabled = false; // Resetar botão também
  Serial.println("   Dispositivo atual desconectado");
  Serial.println("   Iniciando nova busca...");
  server.sendHeader("Location", "/");
  server.send(303);
}

// Status em JSON - Agora apenas informa sobre botão
void handleStatus() {
  String json = "{";
  json += "\"hasRelay\":" + String(hasRelayDevice ? "true" : "false") + ",";
  json += "\"devices\":" + String(deviceCount) + ",";
  json += "\"espnow\":" + String(espNowInitialized ? "true" : "false") + ",";
  json += "\"buttonDisabled\":" + String(buttonDisabled ? "true" : "false");
  
  if (buttonDisabled) {
    unsigned long remaining = BUTTON_DISABLE_DURATION - (millis() - buttonDisabledStart);
    json += ",\"timeRemaining\":" + String(remaining);
  }
  
  json += "}";
  server.send(200, "application/json", json);
}

// Página principal
void handleRoot() {
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<title>Controle ESP-01 Auto-Discovery</title>";
  html += "<style>";
  html += "body{font-family:Arial,sans-serif;max-width:700px;margin:20px auto;padding:20px;background:linear-gradient(135deg,#667eea 0%,#764ba2 100%);color:#333}";
  html += ".container{background:rgba(255,255,255,0.95);padding:30px;border-radius:15px;box-shadow:0 8px 25px rgba(0,0,0,0.15)}";
  html += "h1{color:#333;text-align:center;margin-bottom:10px;font-size:1.8em}";
  html += ".subtitle{text-align:center;color:#666;margin-bottom:30px;font-style:italic}";
  html += ".status{text-align:center;margin:20px 0;padding:15px;border-radius:10px;font-weight:bold;font-size:1.1em}";
  html += ".status.ready{background:linear-gradient(45deg,#28a745,#20c997);color:white;box-shadow:0 4px 15px rgba(40,167,69,0.3)}";
  html += ".status.active{background:linear-gradient(45deg,#ff6b35,#f7931e);color:white;box-shadow:0 4px 15px rgba(255,107,53,0.3)}";
  html += ".status.disconnected{background:linear-gradient(45deg,#ffc107,#fd7e14);color:#333;box-shadow:0 4px 15px rgba(255,193,7,0.3)}";
  html += ".button{display:inline-block;margin:10px;padding:15px 25px;font-size:16px;border:none;border-radius:10px;cursor:pointer;text-decoration:none;text-align:center;font-weight:bold;transition:all 0.3s ease;box-shadow:0 4px 15px rgba(0,0,0,0.2)}";
  html += ".button.pulse{background:linear-gradient(45deg,#ff6b35,#f7931e);color:white}.button.pulse:hover{transform:translateY(-2px);box-shadow:0 6px 20px rgba(255,107,53,0.4)}";
  html += ".button.scan{background:linear-gradient(45deg,#007bff,#6f42c1);color:white}.button.scan:hover{transform:translateY(-2px);box-shadow:0 6px 20px rgba(0,123,255,0.4)}";
  html += ".button:disabled{background:linear-gradient(45deg,#6c757d,#adb5bd)!important;color:#fff!important;cursor:not-allowed!important;transform:none!important;box-shadow:none!important}";
  html += ".button-container{text-align:center;margin:25px 0}";
  html += ".info{background:rgba(0,123,255,0.1);padding:20px;border-radius:10px;margin-top:20px;border-left:4px solid #007bff}";
  html += ".devices{background:rgba(40,167,69,0.1);padding:15px;border-radius:10px;margin-top:15px;border-left:4px solid #28a745}";
  html += ".device-item{background:white;margin:8px 0;padding:10px;border-radius:8px;box-shadow:0 2px 5px rgba(0,0,0,0.1)}";
  html += ".timer-display{background:rgba(255,193,7,0.2);border:2px solid #ffc107;border-radius:10px;padding:15px;margin:15px 0;text-align:center;font-weight:bold;font-size:1.2em;color:#856404}";
  html += "@keyframes pulse{0%{transform:scale(1)}50%{transform:scale(1.05)}100%{transform:scale(1)}}";
  html += ".pulse{animation:pulse 2s infinite}";
  html += "</style>";
  html += "<script>";
  html += "function updateStatus(){fetch('/status').then(r=>r.json()).then(d=>{";
  html += "let statusEl=document.getElementById('status');";
  html += "let pulseBtn=document.getElementById('pulseBtn');";
  html += "let timerEl=document.getElementById('timer');";
  html += "let statusText='';let statusClass='';";
  html += "if(d.hasRelay){";
  html += "if(d.buttonDisabled){statusText='⚡ COMANDO ENVIADO - Aguarde...';statusClass='status active pulse';";
  html += "pulseBtn.disabled=true;pulseBtn.innerHTML='⏳ Aguarde...';}";
  html += "else{statusText='🔗 Relay Conectado - PRONTO';statusClass='status ready';";
  html += "pulseBtn.disabled=false;pulseBtn.innerHTML='▶️ Abrir Portão';}";
  html += "}else{statusText='🔍 Procurando dispositivos...';statusClass='status disconnected pulse';";
  html += "pulseBtn.disabled=true;pulseBtn.innerHTML='🔍 Sem Dispositivo';}";
  html += "statusEl.innerHTML=statusText;statusEl.className=statusClass;";
  html += "if(d.buttonDisabled&&d.timeRemaining){";
  html += "let seconds=Math.ceil(d.timeRemaining/1000);";
  html += "timerEl.style.display='block';";
  html += "timerEl.innerHTML='⏰ Botão liberado em: '+seconds+' segundos';";
  html += "}else{timerEl.style.display='none';}";
  html += "})}";
  html += "setInterval(updateStatus,500);";
  html += "</script>";
  html += "</head><body>";
  html += "<div class='container'>";
  html += "<h1>🚀 ESP-01 Auto-Discovery</h1>";
  html += "<div class='subtitle'>Sistema Inteligente de Controle</div>";
  
  String statusClass = hasRelayDevice ? 
    (buttonDisabled ? "active pulse" : "ready") : 
    "disconnected pulse";
  String statusText = hasRelayDevice ? 
    (buttonDisabled ? "⚡ COMANDO ENVIADO - Aguarde..." : "🔗 Relay Conectado - PRONTO") :
    "🔍 Procurando dispositivos...";
    
  html += "<div id='status' class='status " + statusClass + "'>";
  html += statusText + "</div>";
  
  // Timer display
  html += "<div id='timer' class='timer-display' style='display:none;'></div>";
  
  html += "<div class='button-container'>";
  if (hasRelayDevice) {
    String buttonText = buttonDisabled ? "⏳ Aguarde..." : "▶️ Abrir Portão";
    String buttonState = buttonDisabled ? "disabled" : "";
    html += "<button id='pulseBtn' onclick=\"if(!this.disabled)window.location.href='/relay/pulse'\" class='button pulse' " + buttonState + ">" + buttonText + "</button>";
  } else {
    html += "<button id='pulseBtn' class='button pulse' disabled>🔍 Sem Dispositivo</button>";
  }
  html += "<a href='/scan' class='button scan'>🔄 BUSCAR DISPOSITIVOS</a>";
  html += "</div>";
  
  html += "<div class='info'>";
  html += "<strong>📡 Sistema:</strong> ESP-NOW Auto-Discovery (Timer Corrigido)<br>";
  html += "<strong>🔶 Rede:</strong> " + String(ssid) + "<br>";
  html += "<strong>🌐 IP:</strong> " + WiFi.softAPIP().toString() + "<br>";
  html += "<strong>🔍 Dispositivos:</strong> " + String(deviceCount) + " encontrados<br>";
  html += "<strong>🔧 Status:</strong> Relay controla próprio timer";
  html += "</div>";
  
  if (deviceCount > 0) {
    html += "<div class='devices'>";
    html += "<strong>📋 Dispositivos Conectados:</strong>";
    for (int i = 0; i < deviceCount; i++) {
      if (foundDevices[i].connected) {
        html += "<div class='device-item'>";
        html += "📱 " + String(foundDevices[i].name);
        html += " (" + String(foundDevices[i].type == 2 ? "Relay" : "Servidor") + ")";
        html += "</div>";
      }
    }
    html += "</div>";
  }
  
  html += "</div></body></html>";
  
  server.send(200, "text/html", html);
}