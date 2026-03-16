#include <ESP8266WiFi.h>
#include <WiFiUdp.h>

// ====================== CONFIGURAÇÕES ======================
#define DEVICE_NAME "PORTA_A"  // 👉 altere para "PORTA_A", "PORTA_B" ou "PORTEIRO"
#define DEV_PORTA_A "PORTA_A"
#define DEV_PORTA_B "PORTA_B"
#define DEV_PORTEIRO "PORTEIRO"
#define MASTER_DEVICE "PORTEIRO"
const char* ssid = "Evosystems&Wires Visitante";
const char* password = "Wifi2025";

#define PORTA_TIMEOUT 300000
#define UDP_PORT 4210
#define RELAY_TIME 5000  // 5 segundos
#define SENSOR_OPEN LOW
#define MASTER_LEASE_TIME 15000

// ===================== PINOS ================================
#define BTN1_PIN 5    // D1 - facial / botão 1 do porteiro
#define BTN2_PIN 4    // D2 - botão 2 (apenas porteiro)
#define BYPASS_PIN 0  // D3 - interruptor bypass (apenas porteiro)
#define SENSOR_PIN 14 // D5 - sensor da porta
#define RELAY_PIN 12  // D6 - LED ímã
#define PUPE_PIN 13   // D7 - LED puxe/empurre

// ===================== VARIÁVEIS ============================
WiFiUDP udp;
IPAddress localIP;
char knownNames[5][16];
char knownIPs[5][16];
int devicePriority = 1;
int masterPriority = 0;
bool isMaster = false;
char networkMaster[16] = "";
char tokenOwner[16] = "";
bool bypassMode = false;
bool portaAberta = false;
bool relayAtivo = false;
bool masterBusy = false;
bool alertSent = false;
unsigned long lastPing[5];
unsigned long masterBusyTime = 0;
unsigned long relayStart = 0;
unsigned long lastDiscovery = 0;
unsigned long lastPingSent = 0;
unsigned long lastStatusSent = 0;
unsigned long portaAbertaTempo = 0;
unsigned long masterLeaseExpire = 0;
unsigned long lastOpenEvent = 0;
unsigned long sensorWatchdog = 0;
unsigned long sensorDebounce = 0;
unsigned long openToken = 0;
unsigned long currentToken = 0;

// ===================== TOKEN LOCK ==========================
bool tokenActive = false;
char tokenPorta[16] = "";
unsigned long tokenExpire = 0;

bool adquirirToken(const char* porta) {
  if (!tokenActive || millis() > tokenExpire) {
    tokenActive = true;
    strncpy(tokenPorta, porta, sizeof(tokenPorta)-1);
    tokenPorta[sizeof(tokenPorta)-1] = '\0';
    tokenExpire = millis() + 10000;
    return true;
  }
  return strcmp(tokenPorta, porta) == 0;
}

void liberarToken() {
  tokenActive = false;
  tokenPorta[0] = '\0';
}

// Estado das outras portas (recebido via rede)
bool portaAAberta = false;
bool portaBAberta = false;
bool portaALock = false;
bool portaBLock = false;
bool aguardandoAutorizacao = false;
bool novoEstado = false;
unsigned long lastStatusPortaA = 0;
unsigned long lastStatusPortaB = 0;
unsigned long lastMasterSeen = 0;
unsigned long reqTimeout = 0;

// ===================== FUNÇÕES =============================
void sendBroadcast(const char* msg) {
  IPAddress broadcastIP = WiFi.localIP() | ~WiFi.subnetMask();
  udp.beginPacket(broadcastIP, UDP_PORT);
  udp.print(msg);
  udp.endPacket();
}

void sendStatus() {
  char msg[64];
  snprintf(msg, sizeof(msg),
  "STATUS|%s|%s",
  DEVICE_NAME,
  portaAberta ? "OPEN" : "CLOSED");
  sendBroadcast(msg);
  lastStatusSent = millis();
}

bool deviceKnown(char* dev) {
  if (strcmp(dev, DEVICE_NAME) == 0) return true;

  for (int i = 0; i < 5; i++) {
    if (strcmp(knownNames[i], dev) == 0) return true;
  }
  return false;
}

void checkSplitBrain(int prio, const char* dev) {

  // Se eu sou master e aparece um dispositivo com prioridade maior
  if (isMaster && prio > devicePriority) {

    Serial.println("⚠️ Detectado master com prioridade maior!");
    Serial.println("➡️ Abdicando do papel de master");

    isMaster = false;
    masterPriority = prio;

    strncpy(networkMaster, dev, sizeof(networkMaster)-1);
    networkMaster[sizeof(networkMaster)-1] = '\0';
  }
}

bool podeAbrir(const char* portaAlvo) {
  // Se bypass está ativo, pode abrir sempre
  if (millis() - lastOpenEvent < 2000) {
    Serial.println("⏳ Sistema aguardando estabilização");
    return false;
  }
  if (bypassMode) {
    Serial.println("✅ Bypass ativo, abrindo sem verificar intertravamento");
    return true;
  }

  // Verifica intertravamento: só pode abrir se a OUTRA porta estiver fechada
  if (strcmp(portaAlvo, DEV_PORTA_A) == 0) {
    // Verifica se recebeu status da PORTA_B recentemente
    if (lastStatusPortaB == 0 || millis() - lastStatusPortaB > 10000) {
      Serial.println("⚠️ Sem comunicação com PORTA_B! Bloqueando por segurança.");
      return false;
    }
    
    if (portaBAberta) {
      Serial.println("🚫 PORTA_B está aberta! Não pode abrir PORTA_A.");
      Serial.print("   Estado PORTA_B: ");
      Serial.println(portaBAberta ? "ABERTA" : "FECHADA");
      return false;
    }
    Serial.println("✅ PORTA_B está fechada, pode abrir PORTA_A");
  } 
  else if (strcmp(portaAlvo, DEV_PORTA_B) == 0) {
    // Verifica se recebeu status da PORTA_A recentemente
    if (lastStatusPortaA == 0 || millis() - lastStatusPortaA > 10000) {
      Serial.println("⚠️ Sem comunicação com PORTA_A! Bloqueando por segurança.");
      return false;
    }
    
    if (portaAAberta) {
      Serial.println("🚫 PORTA_A está aberta! Não pode abrir PORTA_B.");
      Serial.print("   Estado PORTA_A: ");
      Serial.println(portaAAberta ? "ABERTA" : "FECHADA");
      return false;
    }
    Serial.println("✅ PORTA_A está fechada, pode abrir PORTA_B");
  }

  if (strcmp(portaAlvo, DEV_PORTA_A) == 0 && portaBLock) {
    Serial.println("🚫 PORTA_B está travando o sistema");
    return false;
  }

  if (strcmp(portaAlvo, DEV_PORTA_B) == 0 && portaALock) {
    Serial.println("🚫 PORTA_A está travando o sistema");
    return false;
  }

  return true;
}

void solicitarAbertura(const char* porta) {
  // Primeiro tenta abrir localmente
  if (strcmp(porta, DEVICE_NAME) == 0 && podeAbrir(porta)) {
    Serial.println("✅ Condições OK, abrindo localmente");
    abrirPorta();
    char ackMsg[64];
    snprintf(ackMsg, sizeof(ackMsg), "ACK_OPEN|%s", DEVICE_NAME);
    sendBroadcast(ackMsg);
    return;
  }
  // Se não puder, tenta pedir ao master
  if (networkMaster[0] != '\0') {
    Serial.println("📨 Pedindo autorização ao master");
    char reqMsg[64];
    openToken = millis();
    snprintf(reqMsg, sizeof(reqMsg), "REQ_OPEN|%s|%s|%lu", porta, DEVICE_NAME, openToken);
    sendBroadcast(reqMsg);
    aguardandoAutorizacao = true;
    reqTimeout = millis();
  } 
  else {
    Serial.println("❌ Nenhum master disponível e não pode abrir");
  }
}

void addDevice(const char* dev, const char* ip) {
  if (strcmp(dev, DEVICE_NAME) == 0)
    return;
  for (int i = 0; i < 5; i++) {
    if (strcmp(knownNames[i], dev) == 0) {
      strncpy(knownIPs[i], ip, 15);
      lastPing[i] = millis();
      return;
    }
  }
  for (int i = 0; i < 5; i++) {
    if (knownNames[i][0] == '\0') {
      strncpy(knownNames[i], dev, 15);
      knownNames[i][15] = '\0';
      strncpy(knownIPs[i], ip, 15);
      knownIPs[i][15] = '\0';
      lastPing[i] = millis();
      Serial.print("[DISCOVERY] ");
      Serial.print(dev);
      Serial.print(" -> ");
      Serial.println(ip);
      break;
    }
  }
}

void abrirPorta() {
  if (relayAtivo) return;

  Serial.println("🚪 Abrindo porta");

  digitalWrite(RELAY_PIN, LOW);
  digitalWrite(PUPE_PIN, LOW);

  relayStart = millis();
  relayAtivo = true;
  lastOpenEvent = millis();
}

void processMessage(char* msg) {
  if (strncmp(msg, "DISCOVERY|", 10) == 0) {
    char buffer[256];
    strncpy(buffer, msg, sizeof(buffer));
    buffer[sizeof(buffer)-1] = '\0';

    char* token = strtok(buffer, "|");
    token = strtok(NULL, "|");
    char* dev = token;
    token = strtok(NULL, "|");
    char* ip = token;
    addDevice(dev, ip);
  }

  else if (strncmp(msg, "CONFIRM|", 8) == 0) {
    char buffer[256];
    strncpy(buffer, msg, sizeof(buffer));
    buffer[sizeof(buffer)-1] = '\0';
    char* token = strtok(buffer, "|");
    token = strtok(NULL, "|");
    char* dev = token;
    token = strtok(NULL, "|");
    char* ip = token;
    addDevice(dev, ip);
  }

  else if (strncmp(msg, "PING|", 5) == 0) {
    char buffer[256];
    strncpy(buffer, msg, sizeof(buffer));
    buffer[sizeof(buffer)-1] = '\0';
    char* token = strtok(buffer, "|");
    token = strtok(NULL, "|");
    char* dev = token;
    token = strtok(NULL, "|");
    char* ip = token;
    addDevice(dev, ip);
    char pongMsg[64];
    snprintf(pongMsg, sizeof(pongMsg), "PONG|%s|%s", DEVICE_NAME, localIP.toString().c_str());
    udp.beginPacket(udp.remoteIP(), UDP_PORT);
    udp.print(pongMsg);
    udp.endPacket();
  }

  else if (strncmp(msg, "HELLO|", 6) == 0) {
    char buffer[256];
    strncpy(buffer, msg, sizeof(buffer));
    buffer[sizeof(buffer)-1] = '\0';
    char* token = strtok(buffer, "|");
    token = strtok(NULL, "|");

    char* dev = token;
    char* p = strtok(NULL, "|");
    if (!p) return;
    int prio = atoi(p);
    char* ip = strtok(NULL, "|");
    char* role = strtok(NULL, "|");
    unsigned long lease = atol(strtok(NULL, "|"));
    checkSplitBrain(prio, dev);

    if (strcmp(dev, networkMaster) == 0) {
      masterLeaseExpire = millis() + lease;
    }
    if (prio > masterPriority) {
      masterPriority = prio;
      strncpy(networkMaster, dev, sizeof(networkMaster)-1);
      networkMaster[sizeof(networkMaster)-1] = '\0';
      isMaster = (strcmp(dev, DEVICE_NAME) == 0);
    }
    else if (prio == masterPriority && strcmp(dev, DEVICE_NAME) != 0) {
      if (strcmp(ip, localIP.toString().c_str()) > 0) { // desempate por IP
        strncpy(networkMaster, dev, sizeof(networkMaster)-1);
        networkMaster[sizeof(networkMaster)-1] = '\0';
        isMaster = false;
      }
    }
    if (strcmp(dev, networkMaster) == 0) {
      lastMasterSeen = millis();
    }
  }

  else if (strncmp(msg, "PONG|", 5) == 0) {
    char buffer[256];
    strncpy(buffer, msg, sizeof(buffer));
    buffer[sizeof(buffer)-1] = '\0';
    char* token = strtok(buffer, "|");
    token = strtok(NULL, "|");
    char* dev = token;
    for (int i = 0; i < 5; i++) {
      if (strcmp(knownNames[i], dev) == 0) {
        lastPing[i] = millis();
      }
    }
  }

  else if (strncmp(msg, "REQ_OPEN|", 9) == 0) {
    if (!isMaster) return;
    char buffer[256];
    strncpy(buffer, msg, sizeof(buffer));
    buffer[sizeof(buffer)-1] = '\0';

    char* token = strtok(buffer, "|");
    token = strtok(NULL, "|");
    char* porta = token;
    token = strtok(NULL, "|");
    char* origem = token;
    token = strtok(NULL, "|");
    unsigned long tokenID = atol(token);
    if (!deviceKnown(origem)) return;
    bool permitido = podeAbrir(porta);
    if (!permitido) {
      char denyMsg[64];
      snprintf(denyMsg,sizeof(denyMsg),"DENY|%s",porta);
      sendBroadcast(denyMsg);
      return;
    }
    Serial.print("📩 Pedido de abertura: ");
    Serial.print(porta);
    Serial.print(" de ");
    Serial.println(origem);
    if (masterBusy) {
        char denyMsg[64];
        snprintf(denyMsg, sizeof(denyMsg), "DENY|%s", porta);
        sendBroadcast(denyMsg);
        return;
    }
    if (permitido && adquirirToken(porta)) {
        masterBusy = true;
        masterBusyTime = millis();
        currentToken = tokenID;
        strncpy(tokenOwner, origem, sizeof(tokenOwner)-1);
        char allowMsg[64];
        snprintf(allowMsg, sizeof(allowMsg), "ALLOW|%s|%lu", porta, currentToken);
        sendBroadcast(allowMsg);
    } 
    else {
        char denyMsg[64];
        snprintf(denyMsg, sizeof(denyMsg), "DENY|%s", porta);
        sendBroadcast(denyMsg);
    }
  }

  else if (strncmp(msg, "ALLOW|", 6) == 0) {
    lastOpenEvent = millis();
    char buffer[128];
    strncpy(buffer,msg,sizeof(buffer));
    buffer[sizeof(buffer)-1]='\0';

    char* token=strtok(buffer,"|");
    token=strtok(NULL,"|");
    char* porta=token;

    token=strtok(NULL,"|");
    unsigned long tokenID=atol(token);
    if (strcmp(porta, DEVICE_NAME) == 0 && tokenID == openToken) {
      Serial.println("✅ Autorização recebida!");
      abrirPorta();
      char ackMsg[64];
      snprintf(ackMsg, sizeof(ackMsg), "ACK_OPEN|%s", DEVICE_NAME);
      sendBroadcast(ackMsg);
      aguardandoAutorizacao = false;
    }
  }

  else if (strncmp(msg, "DENY|", 5) == 0) {
    char* porta = msg + 5;
    if (strcmp(porta, DEVICE_NAME) == 0) {
      Serial.println("❌ Abertura negada!");
      aguardandoAutorizacao = false;
    }
  }

  else if (strncmp(msg, "ACK_OPEN|", 9) == 0) {
    char* porta = msg + 9;
    Serial.println("✔ Porta confirmou abertura: ");
    Serial.println(porta);
    if (isMaster) {
        masterBusy = false;
        liberarToken();
    }
  }

  else if (strncmp(msg, "OPEN|", 5) == 0) {
    char* target = msg + 5;
    if (strcmp(target, DEVICE_NAME) == 0) {
      if (isMaster) {
        abrirPorta();
      }
      else {
        solicitarAbertura(DEVICE_NAME);
      }
    }
  }

  else if (strncmp(msg, "BYPASS|", 7) == 0) {
    if (strstr(msg, "ON")) {
      bypassMode = true;
    } else {
      bypassMode = false;
    }
    Serial.println(bypassMode ? "⚠️ Bypass ativado!" : "🔒 Bypass desativado.");
  }

  else if (strncmp(msg, "STATUS|", 7) == 0) {
    char buffer[256];
    strncpy(buffer, msg, sizeof(buffer));
    buffer[sizeof(buffer)-1] = '\0';

    char* token = strtok(buffer, "|");
    token = strtok(NULL, "|");
    char* dev = token;
    token = strtok(NULL, "|");
    char* estado = token;
    if (!dev || !estado) return;
    
    // Atualiza o estado das outras portas
    if (strcmp(dev, "PORTA_A") == 0) {
      bool estadoAnterior = portaAAberta;
      portaAAberta = (strcmp(estado, "OPEN") == 0);
      portaALock = portaAAberta && strcmp(DEVICE_NAME, "PORTA_A") != 0; // Se a porta A estiver aberta, ela trava o sistema para a porta B
      lastStatusPortaA = millis();
      Serial.print("[STATUS] PORTA_A: ");
      Serial.print(estado);
      if (estadoAnterior != portaAAberta) {
        Serial.println(" (MUDOU!)");
      } else {
        Serial.println();
      }
    }

    else if (strcmp(dev, "PORTA_B") == 0) {
      bool estadoAnterior = portaBAberta;
      portaBAberta = (strcmp(estado, "OPEN") == 0);
      portaBLock = portaBAberta && strcmp(DEVICE_NAME, "PORTA_B") != 0; // Se a porta B estiver aberta, ela trava o sistema para a porta A
      lastStatusPortaB = millis();
      Serial.print("[STATUS] PORTA_B: ");
      Serial.print(estado);
      if (estadoAnterior != portaBAberta) {
        Serial.println(" (MUDOU!)");
      } else {
        Serial.println();
      }
    }
  }

  else if (strncmp(msg, "LOCK|", 5) == 0) {
    char* dev = msg + 5;

    if (strcmp(dev, "PORTA_A") == 0) portaALock = true;
    if (strcmp(dev, "PORTA_B") == 0) portaBLock = true;

    Serial.println("🔒 LOCK recebido de ");
    Serial.println(dev);
  }

  else if (strncmp(msg, "UNLOCK|", 7) == 0) {
    char* dev = msg + 7;

    if (strcmp(dev, "PORTA_A") == 0) portaALock = false;
    if (strcmp(dev, "PORTA_B") == 0) portaBLock = false;

    Serial.println("🔓 UNLOCK recebido de ");
    Serial.println(dev);
  }

  else if (strncmp(msg, "ALERT|", 6) == 0) {
    char buffer[256];
    strncpy(buffer, msg, sizeof(buffer));
    buffer[sizeof(buffer)-1] = '\0';
    char* token = strtok(buffer, "|");
    token = strtok(NULL, "|");
    char* tipo = token;
    token = strtok(NULL, "|");
    char* origem = token;
    token = strtok(NULL, "|");
    unsigned long tokenID = atol(token);
    Serial.print("🚨 ALERTA recebido: ");
    Serial.print(tipo);
    Serial.print(" de ");
    Serial.println(origem);
  }
}

// ===================== SETUP ===============================
void setup() {
  Serial.begin(115200);
  pinMode(BTN1_PIN, INPUT_PULLUP);
  pinMode(BTN2_PIN, INPUT_PULLUP);
  pinMode(BYPASS_PIN, INPUT_PULLUP);
  pinMode(SENSOR_PIN, INPUT_PULLUP);
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(PUPE_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);
  digitalWrite(PUPE_PIN, HIGH);

  memset(knownNames, 0, sizeof(knownNames));
  memset(knownIPs, 0, sizeof(knownIPs));
  memset(lastPing, 0, sizeof(lastPing));

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Conectando ao WiFi");

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\n✅ WiFi conectado!");
  localIP = WiFi.localIP();
  Serial.println("IP: " + localIP.toString());
  Serial.println("Device: " + DEVICE_NAME);

  udp.begin(UDP_PORT);
  char msg[40];
  snprintf(msg, sizeof(msg), "DISCOVERY|%s|%s", DEVICE_NAME, localIP.toString().c_str());
  sendBroadcast(msg);

  if (strcmp(DEVICE_NAME, "PORTEIRO") == 0) devicePriority = 3;
  else if (strcmp(DEVICE_NAME, "PORTA_A") == 0) devicePriority = 2;
  else if (strcmp(DEVICE_NAME, "PORTA_B") == 0) devicePriority = 1;

  portaAberta = (digitalRead(SENSOR_PIN) == SENSOR_OPEN);
  sendStatus();

  masterPriority = devicePriority; // Assume que é master até provar o contrário
  strcpy(networkMaster, DEVICE_NAME);
  isMaster = true;
}

// ===================== LOOP ================================
void loop() {
  bool leitura = (digitalRead(SENSOR_PIN) == SENSOR_OPEN);
  if (leitura != novoEstado && millis() - sensorDebounce > 100) {
    sensorDebounce = millis();
    novoEstado = leitura;
  }
  static unsigned long lastReconnect = 0;

    if (WiFi.status() != WL_CONNECTED) {
      if (millis() - lastReconnect > 5000) {
        Serial.println("⚠️ Tentando reconectar WiFi...");
        WiFi.disconnect();
        WiFi.begin(ssid, password);
        if (WiFi.status() == WL_CONNECTED) {
          udp.stop();
          udp.begin(UDP_PORT);
        }
        lastReconnect = millis();
      }
    }
    static bool wifiWasDown = false;
    if (WiFi.status() != WL_CONNECTED) {
        wifiWasDown = true;
    }
    else if (wifiWasDown) {
        Serial.println("✅ Conexão restabelecida!");
        wifiWasDown = false;
        sendStatus();
    }
  
  // Receber pacotes UDP
  int packetSize = udp.parsePacket();
  if (packetSize) {
    char buffer[256];
    int len = udp.read(buffer, 255);
    if (len > 0) buffer[len] = '\0';
    processMessage(buffer);
  }

  // SEMPRE envia broadcast de descoberta a cada 15s (não para nunca!)
  if (millis() - lastDiscovery > 15000) {
    char msg[64];
    snprintf(msg, sizeof(msg), "DISCOVERY|%s|%s", DEVICE_NAME, localIP.toString().c_str());
    sendBroadcast(msg);
    const char* role = (isMaster) ? "MASTER" : "NODE";
    char helloMsg[128];
    snprintf(helloMsg, sizeof(helloMsg), "HELLO|%s|%d|%s|%s|%lu",
             DEVICE_NAME, devicePriority,
             localIP.toString().c_str(),
             role,
             MASTER_LEASE_TIME);
    sendBroadcast(helloMsg);
    lastDiscovery = millis();
  }

  // Envia PING a cada 10s
  if (millis() - lastPingSent > 15000) {
    char pingMsg[128];
    snprintf(pingMsg, sizeof(pingMsg), "PING|%s|%s", DEVICE_NAME, localIP.toString().c_str());
    sendBroadcast(pingMsg);
    lastPingSent = millis();
  }

  // Envia STATUS a cada 15s (para manter todos sincronizados)
  if (millis() - lastStatusSent > 15000) {
    sendStatus();
    lastStatusSent = millis();
    if (millis() - lastStatusPortaA > 20000) {
      portaAAberta = false;
      portaALock = false;
    }
    if (millis() - lastStatusPortaB > 20000) {
      portaBAberta = false;
      portaBLock = false;
    }
    

    Serial.print("Minha porta: ");
    Serial.println(portaAberta ? "ABERTA" : "FECHADA");
    Serial.print("PORTA_A: ");
    Serial.print(portaAAberta ? "ABERTA" : "FECHADA");
    Serial.print(" (última atualização: ");
    Serial.print(millis() - lastStatusPortaA);
    Serial.println(" ms)");
    Serial.print("PORTA_B: ");
    Serial.print(portaBAberta ? "ABERTA" : "FECHADA");
    Serial.print(" (última atualização: ");
    Serial.print(millis() - lastStatusPortaB);
    Serial.println(" ms)");
    Serial.print("Bypass: ");
    Serial.println(bypassMode ? "ON" : "OFF");
    Serial.print("Relay ativo: ");
    Serial.println(relayAtivo ? "SIM" : "NÃO");
    Serial.println("-------------------------");
  }

  // Remove dispositivos inativos (sem PONG há 30s)
  for (int i = 0; i < 5; i++) {
    if (knownNames[i][0] != '\0' && millis() - lastPing[i] > 30000) {
      Serial.println("⚠️ Dispositivo inativo removido: " + knownNames[i]);
      if (strcmp(knownNames[i], networkMaster) == 0) {
        Serial.println("⚠️ Master perdido por inatividade!");
        networkMaster[0] = '\0';
        masterPriority = 0;
        isMaster = false;
      }
      knownNames[i][0] = '\0';
      knownIPs[i][0] = '\0';
    }
  }

 // Lógica de botões
if (strcmp(DEVICE_NAME, "PORTEIRO") == 0) {
  // BOTÃO 1 -> PORTA_A
  static bool lastBtn1 = HIGH;
  static unsigned long lastBtn1Press = 0;
  bool btn1State = digitalRead(BTN1_PIN);
  if (btn1State == LOW && lastBtn1 == HIGH && millis() - lastBtn1Press > 300) {
    lastBtn1Press = millis();
    Serial.println("🔘 Botão 1 pressionado - PORTA_A");
    if (podeAbrir("PORTA_A")) {
      solicitarAbertura("PORTA_A");
    } else {
      Serial.println("❌ Bloqueado pelo intertravamento");
    }
  }
  lastBtn1 = btn1State;
  // BOTÃO 2 -> PORTA_B
  static bool lastBtn2 = HIGH;
  static unsigned long lastBtn2Press = 0;
  bool btn2State = digitalRead(BTN2_PIN);
  if (btn2State == LOW && lastBtn2 == HIGH && millis() - lastBtn2Press > 300) {
    lastBtn2Press = millis();
    Serial.println("🔘 Botão 2 pressionado - PORTA_B");
    if (podeAbrir("PORTA_B")) {
      solicitarAbertura("PORTA_B");
    } else {
      Serial.println("❌ Bloqueado pelo intertravamento");
    }
  }
  lastBtn2 = btn2State;
  // BYPASS
  bool bypassState = (digitalRead(BYPASS_PIN) == LOW);
  static bool lastBypass = !bypassState;
  if (bypassState != lastBypass) {
    char bypassMsg[64];
    snprintf(bypassMsg, sizeof(bypassMsg), "BYPASS|%s", bypassState ? "ON" : "OFF");
    sendBroadcast(bypassMsg);
    lastBypass = bypassState;
    Serial.println("🔀 Bypass alterado: ");
    Serial.println(bypassState ? "ON" : "OFF");
  }
}
  else {
    // BOTÃO LOCAL NAS PORTAS
    static bool lastBtn = HIGH;
    static unsigned long lastPress = 0;
    bool btnState = digitalRead(BTN1_PIN);
    if (btnState == LOW && lastBtn == HIGH && millis() - lastPress > 300) {
      lastPress = millis();
      Serial.println("🔘 Botão local pressionado");
      solicitarAbertura(DEVICE_NAME);
    }
    lastBtn = btnState;
  }

  if (novoEstado != portaAberta) {
    portaAberta = novoEstado;
    if (portaAberta) {
      portaAbertaTempo = millis();
      alertSent = false;
      char lockMsg[64];
      snprintf(lockMsg, sizeof(lockMsg), "LOCK|%s", DEVICE_NAME);
      sendBroadcast(lockMsg);
      // 🔽 IMPLEMENTAÇÃO (10 linhas)
      if (strcmp(DEVICE_NAME, "PORTA_A") == 0) {
        portaAAberta = true;
        portaALock = true;
        lastStatusPortaA = millis();
      }
      else if (strcmp(DEVICE_NAME, "PORTA_B") == 0) {
        portaBAberta = true;
        portaBLock = true;
        lastStatusPortaB = millis();
      }
    }
    else {
      portaAbertaTempo = 0;

      char unlockMsg[64];
      snprintf(unlockMsg, sizeof(unlockMsg), "UNLOCK|%s", DEVICE_NAME);
      sendBroadcast(unlockMsg);
      // 🔽 IMPLEMENTAÇÃO (10 linhas)
      if (strcmp(DEVICE_NAME, "PORTA_A") == 0) {
        portaAAberta = false;
        portaALock = false;
        lastStatusPortaA = millis();
      }
      else if (strcmp(DEVICE_NAME, "PORTA_B") == 0) {
        portaBAberta = false;
        portaBLock = false;
        lastStatusPortaB = millis();
      }
    }
    sendStatus();
    lastStatusSent = millis();
    Serial.println(portaAberta ? "🚪 Sensor: Porta ABERTA" : "🚪 Sensor: Porta FECHADA");
  }

  // Desliga o relé após 5s
  if (relayAtivo && millis() - relayStart >= RELAY_TIME) {
    digitalWrite(RELAY_PIN, HIGH);
    digitalWrite(PUPE_PIN, HIGH);
    relayAtivo = false;
    Serial.println("🔒 Relé desligado.");
    
    // Atualiza status baseado no sensor
    bool sensorAberto = (digitalRead(SENSOR_PIN) == SENSOR_OPEN);
    if (portaAberta != sensorAberto) {
      portaAberta = sensorAberto;
      sendStatus();
    }
  }

  if (portaAberta && !alertSent && millis() - portaAbertaTempo > PORTA_TIMEOUT) {
    Serial.println("⚠️ Porta aberta por muito tempo! Enviando alerta...");
    char alertMsg[64];
    snprintf(alertMsg, sizeof(alertMsg), "ALERT|PORTA_ABERTA|%s", DEVICE_NAME);
    sendBroadcast(alertMsg);
    alertSent = true;
  }

  if (aguardandoAutorizacao && millis() - reqTimeout > 3000) {
    Serial.println("⚠️ Timeout aguardando autorização do master!");
    aguardandoAutorizacao = false;
  }
  
  if (networkMaster[0] != '\0' && millis() > masterLeaseExpire) {
    Serial.println("⚠️ Master perdido!");
    masterPriority = devicePriority;
    strcpy(networkMaster, DEVICE_NAME);
    isMaster = true;
    Serial.println("👑 Assumindo papel de master!");
  }

  if (masterBusy && millis() - masterBusyTime > 10000) {
    Serial.println("⚠️ Master ocupado por muito tempo, resetando estado!");
    masterBusy = false;
    masterBusyTime = 0;
    liberarToken();
    Serial.println("🔄 Liberando sistema por timeout");
    portaALock = false;
    portaBLock = false;
  }
  if (portaAberta) {
    if (sensorWatchdog == 0)
      sensorWatchdog = millis();
  } else {
    sensorWatchdog = 0;
  }
  if (!isMaster && networkMaster[0] != '\0') {
    if (millis() - lastMasterSeen > 20000) {
      Serial.println("⚠️ Master desapareceu!");
      masterPriority = devicePriority;
      strcpy(networkMaster, DEVICE_NAME);
      isMaster = true;
    }
  }
}