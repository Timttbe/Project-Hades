#include <ESP8266WiFi.h>
#include <WiFiUdp.h>

// ====================== CONFIGURAÇÕES ======================
#define DEVICE_NAME "PORTA_A"  // 👉 altere para "PORTA_A", "PORTA_B" ou "PORTEIRO"
#define MASTER_DEVICE "PORTEIRO"
const char* ssid = "Evosystems&Wires Visitante";
const char* password = "Wifi2025";

#define PORTA_TIMEOUT 300000
#define UDP_PORT 4210
#define RELAY_TIME 5000  // 5 segundos
#define SENSOR_OPEN LOW
#define MASTER_LEASE_TIME 8000

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
void sendBroadcast(char* msg) {
  IPAddress broadcastIP = WiFi.localIP() | ~WiFi.subnetMask();
  udp.beginPacket(broadcastIP, UDP_PORT);
  udp.print(msg);
  udp.endPacket();
  Serial.println("[UDP]");
  Serial.println(msg);
}

void sendStatus() {
  char msg[64];
  snprintf(msg, sizeof(msg),
  "STATUS|%s|%s",
  DEVICE_NAME,
  portaAberta ? "OPEN" : "CLOSED");
  sendBroadcast(msg);
}

bool deviceKnown(char* dev) {
  if (strcmp(dev, DEVICE_NAME) == 0) return true;

  for (int i = 0; i < 5; i++) {
    if (strcmp(knownNames[i], dev) == 0) return true;
  }
  return false;
}

bool podeAbrir(char* portaAlvo) {
  // Se bypass está ativo, pode abrir sempre
  if (bypassMode) {
    Serial.println("✅ Bypass ativo, abrindo sem verificar intertravamento");
    return true;
  }

  // Verifica intertravamento: só pode abrir se a OUTRA porta estiver fechada
  if (strcmp(portaAlvo, "PORTA_A") == 0) {
    // Verifica se recebeu status da PORTA_B recentemente
    if (millis() - lastStatusPortaB > 10000 && lastStatusPortaB > 0) {
      Serial.println("⚠️ Sem comunicação com PORTA_B! Bloqueando por segurança.");
      return false;
    }
    
    if (portaBAberta) {
      Serial.println("🚫 PORTA_B está aberta! Não pode abrir PORTA_A.");
      Serial.println("   Estado PORTA_B: " + String(portaBAberta ? "ABERTA" : "FECHADA"));
      return false;
    }
    Serial.println("✅ PORTA_B está fechada, pode abrir PORTA_A");
  } 
  else if (strcmp(portaAlvo, "PORTA_B") == 0) {
    // Verifica se recebeu status da PORTA_A recentemente
    if (millis() - lastStatusPortaA > 10000 && lastStatusPortaA > 0) {
      Serial.println("⚠️ Sem comunicação com PORTA_A! Bloqueando por segurança.");
      return false;
    }
    
    if (portaAAberta) {
      Serial.println("🚫 PORTA_A está aberta! Não pode abrir PORTA_B.");
      Serial.println("   Estado PORTA_A: " + String(portaAAberta ? "ABERTA" : "FECHADA"));
      return false;
    }
    Serial.println("✅ PORTA_A está fechada, pode abrir PORTA_B");
  }

  if (strcmp(portaAlvo, "PORTA_A") == 0 && portaBLock) {
    Serial.println("🚫 PORTA_B está travando o sistema");
    return false;
  }

  if (strcmp(portaAlvo, "PORTA_B") == 0 && portaALock) {
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
    snprintf(reqMsg, sizeof(reqMsg), "REQ_OPEN|%s|%s", porta, DEVICE_NAME);
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
      strncpy(knownIPs[i], ip, 15);
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
}

void processMessage(char* msg) {
  if (strncmp(msg, "DISCOVERY|", 10) == 0) {
    char* token = strtok(msg, "|");
    token = strtok(NULL, "|");
    char* dev = token;
    token = strtok(NULL, "|");
    char* ip = token;
    addDevice(dev, ip);
  }

  else if (strncmp(msg, "CONFIRM|", 8) == 0) {
    char* token = strtok(msg, "|");
    token = strtok(NULL, "|");
    char* dev = token;
    token = strtok(NULL, "|");
    char* ip = token;
    addDevice(dev, ip);
  }

  else if (strncmp(msg, "PING|", 5) == 0) {
    char* token = strtok(msg, "|");
    token = strtok(NULL, "|");
    char* dev = token;
    token = strtok(NULL, "|");
    char* ip = token;
    addDevice(dev, ip);
    char pongMsg[64];
    snprintf(pongMsg, sizeof(pongMsg), "PONG|%s|%s", DEVICE_NAME, localIP.toString().c_str());
    sendBroadcast(pongMsg);
  }

  else if (strncmp(msg, "HELLO|", 6) == 0) {
    char* token = strtok(msg, "|");
    token = strtok(NULL, "|");

    char* dev = token;
    int prio = atoi(strtok(NULL, "|"));
    char* ip = strtok(NULL, "|");
    char* role = strtok(NULL, "|");
    unsigned long lease = atol(strtok(NULL, "|"));
    if (strcmp(dev, networkMaster) == 0) {
      masterLeaseExpire = lease;
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
    char* token = strtok(msg, "|");
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
    char* token = strtok(msg, "|");
    token = strtok(NULL, "|");
    char* porta = token;
    token = strtok(NULL, "|");
    char* origem = token;
    if (!deviceKnown(origem)) return;
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
    if (podeAbrir(porta)) {
        masterBusy = true;
        masterBusyTime = millis();
        char allowMsg[64];
        snprintf(allowMsg, sizeof(allowMsg), "ALLOW|%s", porta);
        sendBroadcast(allowMsg);
    } 
    else {
        char denyMsg[64];
        snprintf(denyMsg, sizeof(denyMsg), "DENY|%s", porta);
        sendBroadcast(denyMsg);
    }
  }

  else if (strncmp(msg, "ALLOW|", 6) == 0) {
    char* porta = msg + 6;
    if (strcmp(porta, DEVICE_NAME) == 0) {
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
    char* token = strtok(msg, "|");
    token = strtok(NULL, "|");
    char* dev = token;
    token = strtok(NULL, "|");
    char* estado = token;
    
    // Atualiza o estado das outras portas
    if (strcmp(dev, "PORTA_A") == 0) {
      bool estadoAnterior = portaAAberta;
      portaAAberta = (strcmp(estado, "OPEN") == 0);
      portaALock = portaAAberta; // Se a porta A estiver aberta, ela trava o sistema para a porta B
      lastStatusPortaA = millis();
      Serial.println(String("[STATUS] PORTA_A: ") + estado +
               (estadoAnterior != portaAAberta ? " (MUDOU!)" : ""));
    } 
    else if (strcmp(dev, "PORTA_B") == 0) {
      bool estadoAnterior = portaBAberta;
      portaBAberta = (strcmp(estado, "OPEN") == 0);
      portaBLock = portaBAberta; // Se a porta B estiver aberta, ela trava o sistema para a porta A
      lastStatusPortaB = millis();
      Serial.println(String("[STATUS] PORTA_B: ") + estado +
               (estadoAnterior != portaBAberta ? " (MUDOU!)" : ""));
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
    char* token = strtok(msg, "|");
    token = strtok(NULL, "|");
    char* tipo = token;
    token = strtok(NULL, "|");
    char* origem = token;
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
}

// ===================== LOOP ================================
void loop() {
  novoEstado = (digitalRead(SENSOR_PIN) ==  SENSOR_OPEN);
  static unsigned long lastReconnect = 0;

    if (WiFi.status() != WL_CONNECTED) {
      if (millis() - lastReconnect > 5000) {
        Serial.println("⚠️ Tentando reconectar WiFi...");
        WiFi.disconnect();
        WiFi.begin(ssid, password);
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
    }
  
  // Receber pacotes UDP
  int packetSize = udp.parsePacket();
  if (packetSize) {
    char buffer[256];
    int len = udp.read(buffer, 255);
    if (len > 0) buffer[len] = '\0';
    processMessage(buffer);
  }

  // SEMPRE envia broadcast de descoberta a cada 5s (não para nunca!)
  if (millis() - lastDiscovery > 15000) {
    char msg[64];
    snprintf(msg, sizeof(msg), "DISCOVERY|%s|%s", DEVICE_NAME, localIP.toString().c_str());
    sendBroadcast(msg);
    String role = (isMaster) ? "MASTER" : "NODE";
    char helloMsg[128];
    snprintf(helloMsg, sizeof(helloMsg), "HELLO|%s|%d|%s|%s|%lu",
             DEVICE_NAME, devicePriority,
             localIP.toString().c_str(),
             role.c_str(),
             millis() + MASTER_LEASE_TIME);
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
    

    Serial.println("Minha porta: " + String(portaAberta ? "ABERTA" : "FECHADA"));
    Serial.println("PORTA_A: " + String(portaAAberta ? "ABERTA" : "FECHADA") + 
                   " (última atualização: " + String(millis() - lastStatusPortaA) + "ms)");
    Serial.println("PORTA_B: " + String(portaBAberta ? "ABERTA" : "FECHADA") + 
                   " (última atualização: " + String(millis() - lastStatusPortaB) + "ms)");
    Serial.println("Bypass: " + String(bypassMode ? "ON" : "OFF"));
    Serial.println("Relay ativo: " + String(relayAtivo ? "SIM" : "NÃO"));
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
    // Botão 1 - Abre PORTA_A
    static bool lastBtn1 = HIGH;
    bool btn1State = digitalRead(BTN1_PIN);
    if (btn1State == LOW && lastBtn1 == HIGH) {
      Serial.println("🔘 Botão 1 pressionado - tentando abrir PORTA_A");
      if (podeAbrir("PORTA_A")) {
        solicitarAbertura("PORTA_A");
        Serial.println("✅ Comando enviado para PORTA_A");
      } else {
        Serial.println("❌ Bloqueado pelo intertravamento");
      }
      delay(300);
    }
    lastBtn1 = btn1State;

    // Botão 2 - Abre PORTA_B
    static bool lastBtn2 = HIGH;
    bool btn2State = digitalRead(BTN2_PIN);
    if (btn2State == LOW && lastBtn2 == HIGH) {
      Serial.println("🔘 Botão 2 pressionado - tentando abrir PORTA_B");
      if (podeAbrir("PORTA_B")) {
        solicitarAbertura("PORTA_B");
        Serial.println("✅ Comando enviado para PORTA_B");
      } else {
        Serial.println("❌ Bloqueado pelo intertravamento");
      }
      delay(300);
    }
    lastBtn2 = btn2State;

    // Interruptor Bypass
    bool bypassState = (digitalRead(BYPASS_PIN) == LOW);
    static bool lastBypass = !bypassState;
    if (bypassState != lastBypass) {
      char bypassMsg[64];
      snprintf(bypassMsg, sizeof(bypassMsg), "BYPASS|%s", bypassState ? "ON" : "OFF");
      sendBroadcast(bypassMsg);
      lastBypass = bypassState;
      Serial.println("🔀 Bypass alterado: " + String(bypassState ? "ON" : "OFF"));
      delay(300);
    }
  } else {
    // Portas A e B - botão local
    static bool lastBtn1 = HIGH;
    bool btn1State = digitalRead(BTN1_PIN);
    if (btn1State == LOW && lastBtn1 == HIGH) {
      Serial.println("🔘 Botão local pressionado");
      solicitarAbertura(DEVICE_NAME);
      delay(300);
    }
    lastBtn1 = btn1State;
  }

  // Atualiza estado do sensor e envia status se mudar
  novoEstado = (digitalRead(SENSOR_PIN) == SENSOR_OPEN);
  if (novoEstado != portaAberta) {
    portaAberta = novoEstado;
    if (portaAberta) {
      portaAbertaTempo = millis();
      alertSent = false;
      char lockMsg[64];
      snprintf(lockMsg, sizeof(lockMsg), "LOCK|%s", DEVICE_NAME);
      sendBroadcast(lockMsg);
      if (strcmp(DEVICE_NAME, "PORTA_A") == 0) {
        portaALock = true; // Se minha porta abrir, eu travo o sistema para a outra porta
      }
      else if (strcmp(DEVICE_NAME, "PORTA_B") == 0) {
        portaBLock = true; // Se minha porta abrir, eu travo o sistema para a outra porta
      }
    }
    else {
      char unlockMsg[64];
      snprintf(unlockMsg, sizeof(unlockMsg), "UNLOCK|%s", DEVICE_NAME);
      sendBroadcast(unlockMsg);
      if (strcmp(DEVICE_NAME, "PORTA_A") == 0) {
        portaALock = false; // Se minha porta fechar, eu destravo o sistema para a outra porta
      }
      else if (strcmp(DEVICE_NAME, "PORTA_B") == 0) {
        portaBLock = false; // Se minha porta fechar, eu destravo o sistema para a outra porta
      }
    }
    sendStatus();
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

  if (masterBusy && millis() - masterBusyTime > 30000) {
    Serial.println("⚠️ Master ocupado por muito tempo, resetando estado!");
    masterBusy = false;
    masterBusyTime = 0;
  }
}