#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>

// ====================== CONFIGURAÇÕES ======================
#define DEVICE_NAME "PORTEIRO"  // 👉 altere para "PORTA_A", "PORTA_B" ou "PORTEIRO"

// ---- Rede gerada pelo PORTEIRO (fixa, nunca muda) ----
#define AP_SSID "PORTEIRO_AP"
#define AP_PASS "porteiro123"

// ---- WiFi local (só PORTEIRO usa) ----
// Salvo em EEPROM via portal web — não precisa mexer aqui
#define EEPROM_SIZE       256
#define EEPROM_SSID_ADDR    0
#define EEPROM_PASS_ADDR   64
#define EEPROM_VALID_ADDR 128
#define EEPROM_VALID_BYTE 0xAA

#define UDP_PORT   4210
#define RELAY_TIME 5000

// ===================== PINOS ================================
#define BTN1_PIN   5   // D1
#define BTN2_PIN   4   // D2
#define BYPASS_PIN 2   // D4 — deve estar HIGH no boot
#define SENSOR_PIN 14  // D5
#define RELAY_PIN  12  // D6
#define PUPE_PIN   13  // D7

// ===================== VARIÁVEIS ============================
WiFiUDP          udp;
ESP8266WebServer server(80);
IPAddress        localIP;

// Dispositivos conhecidos
struct Device {
  String name;
  String ip;
  unsigned long lastSeen;
  bool portaAberta;
};
Device knownDevices[5];

bool          bypassMode      = false;
bool          portaAberta     = false;
unsigned long relayStart      = 0;
bool          relayAtivo      = false;
unsigned long lastDiscovery   = 0;
unsigned long lastPingSent    = 0;
unsigned long lastStatusSent  = 0;

bool          portaAAberta    = false;
bool          portaBAberta    = false;
unsigned long lastStatusPortaA = 0;
unsigned long lastStatusPortaB = 0;

bool          configMode      = false;
bool          isMaster        = false; // true = portal de config sem WiFi salvo

// ===================== EEPROM ===============================
void nvRead(int addr, char* buf, int maxLen) {
  for (int i = 0; i < maxLen - 1; i++) {
    buf[i] = EEPROM.read(addr + i);
    if (buf[i] == '\0') break;
  }
  buf[maxLen - 1] = '\0';
}
void nvWrite(int addr, const char* str, int maxLen) {
  int len = min((int)strlen(str), maxLen - 1);
  for (int i = 0; i < len; i++) EEPROM.write(addr + i, str[i]);
  EEPROM.write(addr + len, '\0');
}
bool nvHasCred()  { return EEPROM.read(EEPROM_VALID_ADDR) == EEPROM_VALID_BYTE; }
void nvSave(const char* s, const char* p) {
  nvWrite(EEPROM_SSID_ADDR, s, 64);
  nvWrite(EEPROM_PASS_ADDR, p, 64);
  EEPROM.write(EEPROM_VALID_ADDR, EEPROM_VALID_BYTE);
  EEPROM.commit();
}
void nvClear() { EEPROM.write(EEPROM_VALID_ADDR, 0); EEPROM.commit(); }

// ===================== UDP ==================================
void sendBroadcast(const String& msg) {
  udp.beginPacket("192.168.4.255", UDP_PORT);
  udp.print(msg);
  udp.endPacket();
  Serial.println("[UDP] " + msg);
}

void sendStatus() {
  String estado = portaAberta ? "OPEN" : "CLOSED";
  sendBroadcast("STATUS|" + String(DEVICE_NAME) + "|" + estado);
}

// ===================== DISPOSITIVOS =========================
void updateDevice(const String& name, const String& ip, bool aberta = false) {
  if (name == DEVICE_NAME) return;
  for (int i = 0; i < 5; i++) {
    if (knownDevices[i].name == name) {
      knownDevices[i].ip       = ip;
      knownDevices[i].lastSeen = millis();
      return;
    }
  }
  for (int i = 0; i < 5; i++) {
    if (knownDevices[i].name == "") {
      knownDevices[i] = { name, ip, millis(), false };
      Serial.println("[DISC] " + name + " -> " + ip);
      break;
    }
  }
  sendBroadcast("CONFIRM|" + String(DEVICE_NAME) + "|" + localIP.toString());
}

void updateDeviceStatus(const String& name, bool aberta) {
  for (int i = 0; i < 5; i++) {
    if (knownDevices[i].name == name) {
      knownDevices[i].portaAberta = aberta;
      knownDevices[i].lastSeen    = millis();
      return;
    }
  }
}

// ===================== INTERTRAVAMENTO ======================
bool podeAbrir(const String& portaAlvo) {
  if (bypassMode) { Serial.println("Bypass ativo."); return true; }
  if (portaAlvo == "PORTA_A") {
    if (lastStatusPortaB > 0 && millis() - lastStatusPortaB > 10000) { Serial.println("Sem status B."); return false; }
    if (portaBAberta) { Serial.println("B aberta. Bloqueando A."); return false; }
  } else if (portaAlvo == "PORTA_B") {
    if (lastStatusPortaA > 0 && millis() - lastStatusPortaA > 10000) { Serial.println("Sem status A."); return false; }
    if (portaAAberta) { Serial.println("A aberta. Bloqueando B."); return false; }
  }
  return true;
}

void abrirPorta() {
  if (relayAtivo) return;
  if (!podeAbrir(String(DEVICE_NAME))) return;
  Serial.println("Abrindo porta...");
  digitalWrite(RELAY_PIN, LOW);
  digitalWrite(PUPE_PIN,  LOW);
  relayStart  = millis();
  relayAtivo  = true;
  portaAberta = true;
  sendStatus();
}

// ===================== MENSAGENS ============================
void processMessage(const String& msg) {
  if (msg.startsWith("DISCOVERY|") || msg.startsWith("CONFIRM|")) {
    int i1 = msg.indexOf('|') + 1, i2 = msg.indexOf('|', i1);
    updateDevice(msg.substring(i1, i2), msg.substring(i2 + 1));
  }
  else if (msg.startsWith("PING|")) {
    int i1 = msg.indexOf('|') + 1, i2 = msg.indexOf('|', i1);
    updateDevice(msg.substring(i1, i2), msg.substring(i2 + 1));
    sendBroadcast("PONG|" + String(DEVICE_NAME) + "|" + localIP.toString());
  }
  else if (msg.startsWith("PONG|")) {
    int i1 = msg.indexOf('|') + 1, i2 = msg.indexOf('|', i1);
    String dev = msg.substring(i1, i2);
    for (int i = 0; i < 5; i++)
      if (knownDevices[i].name == dev) { knownDevices[i].lastSeen = millis(); break; }
  }
  else if (msg.startsWith("OPEN|")) {
    if (msg.substring(5) == DEVICE_NAME) abrirPorta();
  }
  else if (msg.startsWith("BYPASS|")) {
    bypassMode = msg.endsWith("ON");
    Serial.println(bypassMode ? "Bypass ON." : "Bypass OFF.");
  }
  else if (msg.startsWith("STATUS|")) {
    int i1 = msg.indexOf('|') + 1, i2 = msg.indexOf('|', i1);
    String dev = msg.substring(i1, i2), estado = msg.substring(i2 + 1);
    bool ab = (estado == "OPEN");
    if (dev == "PORTA_A") { portaAAberta = ab; lastStatusPortaA = millis(); }
    if (dev == "PORTA_B") { portaBAberta = ab; lastStatusPortaB = millis(); }
    updateDeviceStatus(dev, ab);
    Serial.println("[STATUS] " + dev + ": " + estado);
  }
}

// ===================== INTERFACE WEB (só PORTEIRO) ==========
void setupWebServer() {

  server.on("/", HTTP_GET, []() {
    unsigned long now = millis();

    // --- Build device rows ---
    String deviceRows = "";
    for (int i = 0; i < 5; i++) {
      if (knownDevices[i].name == "") continue;
      bool online = (now - knownDevices[i].lastSeen < 20000);
      bool ab     = knownDevices[i].portaAberta;
      deviceRows += "<div class='dev-card" + String(online ? "" : " offline") + "'>";
      deviceRows += "<div class='dev-header'>";
      deviceRows += "<span class='dev-name'>" + knownDevices[i].name + "</span>";
      deviceRows += "<span class='status-dot " + String(online ? "dot-online" : "dot-offline") + "'></span>";
      deviceRows += "</div>";
      deviceRows += "<div class='dev-body'>";
      deviceRows += "<div class='info-row'><span class='label'>PORTA</span>";
      deviceRows += "<span class='value " + String(ab ? "val-open" : "val-closed") + "'>";
      deviceRows += String(ab ? "&#9650; ABERTA" : "&#9660; FECHADA") + "</span></div>";
      deviceRows += "<div class='info-row'><span class='label'>REDE</span>";
      deviceRows += "<span class='value'>" + knownDevices[i].ip + "</span></div>";
      if (online) {
        deviceRows += "<form action='/open' method='POST'>"
                      "<input type='hidden' name='porta' value='" + knownDevices[i].name + "'>"
                      "<button class='btn-open' type='submit'>ABRIR</button></form>";
      }
      deviceRows += "</div></div>";
    }

    // --- WiFi section ---
    String wifiSection = "";
    if (WiFi.status() == WL_CONNECTED) {
      wifiSection += "<div class='info-row'><span class='label'>REDE</span><span class='value'>" + WiFi.SSID() + "</span></div>";
      wifiSection += "<div class='info-row'><span class='label'>IP LOCAL</span><span class='value'>" + WiFi.localIP().toString() + "</span></div>";
      wifiSection += "<div class='info-row'><span class='label'>SINAL</span><span class='value'>" + String(WiFi.RSSI()) + " dBm</span></div>";
      wifiSection += "<form action='/wificlear' method='POST'><button class='btn-danger' type='submit'>TROCAR REDE</button></form>";
    } else {
      wifiSection += "<div class='info-row'><span class='label'>STATUS</span><span class='value val-open'>SEM CONEXAO</span></div>";
      wifiSection += "<a href='/wificonfig'><button class='btn-open' style='margin-top:12px'>CONFIGURAR WIFI</button></a>";
    }

    String bypassLabel = bypassMode ? "DESATIVAR BYPASS" : "ATIVAR BYPASS";
    String bypassClass = bypassMode ? "btn-bypass-on" : "btn-bypass";

    String html = "<!DOCTYPE html><html lang='pt-BR'><head>"
      "<meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<meta http-equiv='refresh' content='4'>"
      "<title>Wires | Porteiro</title>"
      "<link href='https://fonts.googleapis.com/css2?family=Bebas+Neue&family=DM+Mono:wght@400;500&display=swap' rel='stylesheet'>"
      "<style>"
      ":root{"
        "--gold:#F5A623;"
        "--gold-dim:#c4851a;"
        "--bg:#0a0a0a;"
        "--surface:#111111;"
        "--surface2:#1a1a1a;"
        "--border:#2a2a2a;"
        "--text:#e8e8e8;"
        "--muted:#666;"
        "--online:#4CAF50;"
        "--offline:#555;"
        "--open:#F5A623;"
        "--closed:#4a90d9;"
      "}"
      "*{margin:0;padding:0;box-sizing:border-box;}"
      "body{background:var(--bg);color:var(--text);font-family:'DM Mono',monospace;"
        "min-height:100vh;}"
      /* Top bar */
      ".topbar{background:var(--surface);border-bottom:1px solid var(--border);"
        "padding:0 24px;height:56px;display:flex;align-items:center;"
        "justify-content:space-between;position:sticky;top:0;z-index:10;}"
      ".logo{display:flex;align-items:center;gap:12px;}"
      ".logo-mark{font-family:'Bebas Neue',sans-serif;font-size:1.6em;color:var(--gold);"
        "letter-spacing:3px;}"
      ".logo-sub{font-size:.65em;color:var(--muted);letter-spacing:2px;text-transform:uppercase;"
        "margin-top:2px;}"
      ".topbar-right{font-size:.72em;color:var(--muted);text-align:right;line-height:1.6;}"
      ".topbar-right b{color:var(--gold);}"
      /* Layout */
      ".main{padding:24px;max-width:960px;margin:0 auto;}"
      ".section-label{font-family:'Bebas Neue',sans-serif;font-size:1em;letter-spacing:4px;"
        "color:var(--gold);border-bottom:1px solid var(--border);padding-bottom:8px;"
        "margin-bottom:16px;margin-top:28px;}"
      /* Device grid */
      ".dev-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(240px,1fr));gap:14px;}"
      ".dev-card{background:var(--surface);border:1px solid var(--border);"
        "border-radius:4px;overflow:hidden;transition:border-color .2s;}"
      ".dev-card:hover{border-color:var(--gold-dim);}"
      ".dev-card.offline{opacity:.5;}"
      ".dev-header{background:var(--surface2);padding:12px 16px;"
        "display:flex;justify-content:space-between;align-items:center;"
        "border-bottom:1px solid var(--border);}"
      ".dev-name{font-family:'Bebas Neue',sans-serif;letter-spacing:2px;font-size:1.05em;"
        "color:var(--text);}"
      ".status-dot{width:8px;height:8px;border-radius:50%;}"
      ".dot-online{background:var(--online);box-shadow:0 0 6px var(--online);}"
      ".dot-offline{background:var(--offline);}"
      ".dev-body{padding:14px 16px;}"
      ".info-row{display:flex;justify-content:space-between;align-items:center;"
        "padding:5px 0;border-bottom:1px solid #1e1e1e;font-size:.78em;}"
      ".info-row:last-of-type{border-bottom:none;}"
      ".label{color:var(--muted);letter-spacing:1px;text-transform:uppercase;font-size:.85em;}"
      ".value{color:var(--text);font-weight:500;}"
      ".val-open{color:var(--open);}"
      ".val-closed{color:var(--closed);}"
      /* Porteiro card */
      ".porteiro-card{background:var(--surface);border:1px solid var(--border);"
        "border-radius:4px;padding:16px;display:flex;flex-wrap:wrap;gap:16px;"
        "align-items:center;justify-content:space-between;}"
      ".porteiro-info{display:flex;flex-direction:column;gap:8px;flex:1;min-width:180px;}"
      ".porteiro-actions{display:flex;gap:10px;flex-wrap:wrap;}"
      /* Buttons */
      ".btn-open,.btn-bypass,.btn-bypass-on,.btn-danger{"
        "padding:9px 18px;border:none;border-radius:3px;cursor:pointer;"
        "font-family:'DM Mono',monospace;font-size:.78em;letter-spacing:1px;"
        "text-transform:uppercase;font-weight:500;transition:all .15s;}"
      ".btn-open{background:var(--gold);color:#000;margin-top:12px;width:100%;}"
      ".btn-open:hover{background:var(--gold-dim);}"
      ".btn-bypass{background:transparent;border:1px solid var(--gold);color:var(--gold);}"
      ".btn-bypass:hover{background:rgba(245,166,35,.1);}"
      ".btn-bypass-on{background:var(--gold);color:#000;}"
      ".btn-bypass-on:hover{background:var(--gold-dim);}"
      ".btn-danger{background:transparent;border:1px solid #c62828;color:#ef5350;margin-top:10px;width:100%;}"
      ".btn-danger:hover{background:rgba(198,40,40,.15);}"
      /* WiFi card */
      ".wifi-card{background:var(--surface);border:1px solid var(--border);"
        "border-radius:4px;padding:16px;max-width:360px;}"
      /* Ticker */
      ".ticker{font-size:.68em;color:var(--muted);margin-top:20px;text-align:center;"
        "letter-spacing:1px;}"
      /* Bypass badge */
      ".bypass-badge{display:inline-block;background:rgba(245,166,35,.15);"
        "border:1px solid var(--gold);color:var(--gold);padding:2px 10px;"
        "border-radius:2px;font-size:.72em;letter-spacing:2px;margin-left:8px;}"
      "</style></head><body>"
      "<div class='topbar'>"
        "<div class='logo'>"
          "<div>"
            "<div class='logo-mark'>WIRES</div>"
            "<div class='logo-sub'>Access Control</div>"
          "</div>"
        "</div>"
        "<div class='topbar-right'>"
          "<div>AP &nbsp;<b>192.168.4.1</b></div>";

    if (WiFi.status() == WL_CONNECTED)
      html += "<div>LAN &nbsp;<b>" + WiFi.localIP().toString() + "</b></div>";

    html += "</div></div>"
      "<div class='main'>"
      "<div class='section-label'>PORTEIRO</div>"
      "<div class='porteiro-card'>"
        "<div class='porteiro-info'>"
          "<div class='info-row'><span class='label'>STATUS</span>"
            "<span class='value' style='display:flex;align-items:center;gap:8px;'>"
              "<span class='status-dot dot-online'></span> ONLINE";

    if (bypassMode) html += "<span class='bypass-badge'>BYPASS</span>";

    html += "</span></div>"
          "<div class='info-row'><span class='label'>AP</span>"
            "<span class='value'>" + String(AP_SSID) + "</span></div>";

    if (WiFi.status() == WL_CONNECTED)
      html += "<div class='info-row'><span class='label'>REDE LOCAL</span>"
              "<span class='value'>" + WiFi.SSID() + "</span></div>";

    html += "</div>"
        "<div class='porteiro-actions'>"
          "<form action='/bypass' method='POST'>"
            "<button class='" + bypassClass + "' type='submit'>" + bypassLabel + "</button>"
          "</form>"
        "</div>"
      "</div>"
      "<div class='section-label'>PORTAS</div>"
      "<div class='dev-grid'>" + deviceRows + "</div>"
      "<div class='section-label'>WIFI LOCAL</div>"
      "<div class='wifi-card'>" + wifiSection + "</div>"
      "<div class='ticker'>&#x25B6; WIRES ACCESS CONTROL &nbsp;&nbsp; ATUALIZA A CADA 4S &nbsp;&nbsp; &#x25B6;</div>"
      "</div></body></html>";

    server.send(200, "text/html", html);
  });

  server.on("/bypass", HTTP_POST, []() {
    bypassMode = !bypassMode;
    sendBroadcast("BYPASS|" + String(bypassMode ? "ON" : "OFF"));
    server.sendHeader("Location", "/"); server.send(303);
  });

  server.on("/open", HTTP_POST, []() {
    String porta = server.arg("porta");
    if (podeAbrir(porta)) sendBroadcast("OPEN|" + porta);
    server.sendHeader("Location", "/"); server.send(303);
  });

  server.on("/wificonfig", HTTP_GET, []() {
    server.send(200, "text/html", F(
      "<!DOCTYPE html><html><head><meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<link href='https://fonts.googleapis.com/css2?family=Bebas+Neue&family=DM+Mono:wght@400;500&display=swap' rel='stylesheet'>"
      "<title>Wires | Config WiFi</title>"
      "<style>"
      "*{margin:0;padding:0;box-sizing:border-box;}"
      "body{background:#0a0a0a;color:#e8e8e8;font-family:'DM Mono',monospace;"
        "display:flex;align-items:center;justify-content:center;min-height:100vh;padding:20px;}"
      ".card{background:#111;border:1px solid #2a2a2a;border-radius:4px;padding:32px;width:100%;max-width:360px;}"
      "h2{font-family:'Bebas Neue',sans-serif;color:#F5A623;letter-spacing:4px;font-size:1.6em;margin-bottom:4px;}"
      "p{color:#666;font-size:.78em;margin-bottom:24px;line-height:1.6;}"
      "label{display:block;font-size:.72em;letter-spacing:2px;color:#666;text-transform:uppercase;margin-bottom:6px;margin-top:16px;}"
      "input{width:100%;background:#1a1a1a;border:1px solid #2a2a2a;color:#e8e8e8;"
        "padding:10px 12px;border-radius:3px;font-family:'DM Mono',monospace;font-size:.9em;outline:none;}"
      "input:focus{border-color:#F5A623;}"
      "button{margin-top:24px;width:100%;padding:12px;background:#F5A623;color:#000;"
        "border:none;border-radius:3px;font-family:'DM Mono',monospace;font-size:.85em;"
        "letter-spacing:2px;text-transform:uppercase;font-weight:500;cursor:pointer;}"
      ".back{display:block;text-align:center;margin-top:14px;color:#555;font-size:.75em;text-decoration:none;}"
      ".back:hover{color:#F5A623;}"
      "</style></head><body>"
      "<div class='card'>"
        "<h2>WIRES</h2>"
        "<p>Configure a rede local. O AP PORTEIRO_AP continua ativo para as portas.</p>"
        "<form method='POST' action='/wifisave'>"
          "<label>SSID</label><input name='ssid' type='text' autocomplete='off' required>"
          "<label>SENHA</label><input name='pass' type='password'>"
          "<button type='submit'>SALVAR E CONECTAR</button>"
        "</form>"
        "<a class='back' href='/'>&#8592; Voltar</a>"
      "</div></body></html>"
    ));
  });

  server.on("/wifisave", HTTP_POST, []() {
    String ss = server.arg("ssid"), ps = server.arg("pass");
    if (!ss.length()) { server.send(400, "text/plain", "SSID vazio."); return; }
    server.send(200, "text/html", F(
      "<!DOCTYPE html><html><head><meta charset='utf-8'>"
      "<link href='https://fonts.googleapis.com/css2?family=Bebas+Neue&family=DM+Mono:wght@400&display=swap' rel='stylesheet'>"
      "<style>body{background:#0a0a0a;color:#e8e8e8;font-family:'DM Mono',monospace;"
        "display:flex;align-items:center;justify-content:center;min-height:100vh;}"
      ".card{text-align:center;padding:40px;}"
      "h2{font-family:'Bebas Neue',sans-serif;color:#F5A623;letter-spacing:4px;font-size:2em;}"
      "p{color:#666;font-size:.82em;margin-top:12px;line-height:1.8;}"
      "</style></head><body>"
      "<div class='card'><h2>CONECTANDO</h2>"
        "<p>Credenciais salvas.<br>Aguarde e acesse pelo IP do roteador.</p>"
      "</div></body></html>"
    ));
    delay(800);
    nvSave(ss.c_str(), ps.c_str());
    WiFi.begin(ss.c_str(), ps.c_str());
  });

  server.on("/wificlear", HTTP_POST, []() {
    server.send(200, "text/html", F(
      "<!DOCTYPE html><html><head><meta charset='utf-8'>"
      "<link href='https://fonts.googleapis.com/css2?family=Bebas+Neue&display=swap' rel='stylesheet'>"
      "<style>body{background:#0a0a0a;color:#e8e8e8;font-family:monospace;"
        "display:flex;align-items:center;justify-content:center;min-height:100vh;}"
      "h2{font-family:'Bebas Neue',sans-serif;color:#F5A623;letter-spacing:4px;font-size:2em;text-align:center;}"
      "</style></head><body><h2>REINICIANDO...</h2></body></html>"
    ));
    delay(600); nvClear(); ESP.restart();
  });

  server.begin();
  Serial.println("Web server ativo.");
}

// ===================== SETUP ================================
void setup() {
  Serial.begin(115200);
  EEPROM.begin(EEPROM_SIZE);

  pinMode(BTN1_PIN,   INPUT_PULLUP);
  pinMode(BTN2_PIN,   INPUT_PULLUP);
  pinMode(BYPASS_PIN, INPUT_PULLUP);
  pinMode(SENSOR_PIN, INPUT_PULLUP);
  pinMode(RELAY_PIN,  OUTPUT);
  pinMode(PUPE_PIN,   OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);
  digitalWrite(PUPE_PIN,  HIGH);

  bool isPorteiro = (String(DEVICE_NAME) == "PORTEIRO");
  isMaster = isPorteiro;

  if (isPorteiro) {
    // Sempre sobe o AP com canal fixo 1
    WiFi.mode(WIFI_AP_STA);
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
    WiFi.softAPConfig(
      IPAddress(192,168,4,1),
      IPAddress(192,168,4,1),
      IPAddress(255,255,255,0)
    );
    WiFi.softAP(AP_SSID, AP_PASS, 1); // canal 1 fixo
    Serial.println("AP: " + String(AP_SSID) + " | IP: 192.168.4.1");

    localIP = WiFi.softAPIP();

    // Inicia web server ANTES de tentar roteador — sempre disponível em 192.168.4.1
    setupWebServer();

    // Se tem WiFi local salvo, conecta em background
    if (nvHasCred()) {
      char ss[64], ps[64];
      nvRead(EEPROM_SSID_ADDR, ss, 64);
      nvRead(EEPROM_PASS_ADDR, ps, 64);
      Serial.println("Conectando ao roteador: " + String(ss));
      WiFi.setAutoReconnect(true);
      WiFi.begin(ss, ps);
      unsigned long t = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - t < 12000) {
        delay(200); Serial.print(".");
      }
      Serial.println();
      if (WiFi.status() == WL_CONNECTED)
        Serial.println("Roteador OK. IP: " + WiFi.localIP().toString());
      else
        Serial.println("Roteador indisponivel. Seguindo so com AP.");
    }

  } else {
    // PORTA_A / PORTA_B: conecta no AP do PORTEIRO
    WiFi.mode(WIFI_STA);
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
    WiFi.begin(AP_SSID, AP_PASS);
    Serial.print("Conectando ao PORTEIRO_AP");
    while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
    Serial.println("\nConectado! IP: " + WiFi.localIP().toString());
    localIP = WiFi.localIP();
  }

  Serial.println("Device: " + String(DEVICE_NAME));
  udp.begin(UDP_PORT);
  sendBroadcast("DISCOVERY|" + String(DEVICE_NAME) + "|" + localIP.toString());
}

// ===================== LOOP =================================
void loop() {
  // Web server (só PORTEIRO)
  if (isMaster) server.handleClient();

  // Reconexão das portas ao PORTEIRO_AP se cair
  if (!isMaster && WiFi.status() != WL_CONNECTED) {
    static unsigned long lastRC = 0;
    if (millis() - lastRC > 10000) {
      lastRC = millis();
      Serial.println("Reconectando ao PORTEIRO_AP...");
      WiFi.begin(AP_SSID, AP_PASS);
    }
  }

  // Recebe UDP
  int pkt = udp.parsePacket();
  if (pkt) {
    char buf[255]; int len = udp.read(buf, 254);
    if (len > 0) { buf[len] = '\0'; processMessage(String(buf)); }
  }

  // Discovery a cada 5s
  if (millis() - lastDiscovery > 5000) {
    sendBroadcast("DISCOVERY|" + String(DEVICE_NAME) + "|" + localIP.toString());
    lastDiscovery = millis();
  }

  // Ping a cada 10s
  if (millis() - lastPingSent > 10000) {
    sendBroadcast("PING|" + String(DEVICE_NAME) + "|" + localIP.toString());
    lastPingSent = millis();
  }

  // Status a cada 3s
  if (millis() - lastStatusSent > 3000) {
    sendStatus();
    lastStatusSent = millis();
  }

  // Remove inativos
  for (int i = 0; i < 5; i++) {
    if (knownDevices[i].name != "" && millis() - knownDevices[i].lastSeen > 30000) {
      Serial.println("Inativo: " + knownDevices[i].name);
      knownDevices[i] = { "", "", 0, false };
    }
  }

  // ---- Botões ----
  if (isMaster) {
    static bool lb1 = HIGH; static unsigned long lp1 = 0;
    bool b1 = digitalRead(BTN1_PIN);
    if (b1 == LOW && lb1 == HIGH && millis() - lp1 > 300) {
      lp1 = millis(); Serial.println("BTN1-A");
      if (podeAbrir("PORTA_A")) sendBroadcast("OPEN|PORTA_A");
      else Serial.println("Bloqueado.");
    }
    lb1 = b1;

    static bool lb2 = HIGH; static unsigned long lp2 = 0;
    bool b2 = digitalRead(BTN2_PIN);
    if (b2 == LOW && lb2 == HIGH && millis() - lp2 > 300) {
      lp2 = millis(); Serial.println("BTN2-B");
      if (podeAbrir("PORTA_B")) sendBroadcast("OPEN|PORTA_B");
      else Serial.println("Bloqueado.");
    }
    lb2 = b2;

    bool bp = (digitalRead(BYPASS_PIN) == LOW);
    static bool lbp = false;
    if (bp != lbp) {
      bypassMode = bp;
      sendBroadcast("BYPASS|" + String(bp ? "ON" : "OFF"));
      lbp = bp;
      Serial.println("Bypass: " + String(bp ? "ON" : "OFF"));
    }
  } else {
    static bool lb = HIGH; static unsigned long lp = 0;
    bool b = digitalRead(BTN1_PIN);
    if (b == LOW && lb == HIGH && millis() - lp > 300) {
      lp = millis(); abrirPorta();
    }
    lb = b;
  }

  // Sensor (HIGH = aberta)
  bool novoEstado = (digitalRead(SENSOR_PIN) == HIGH);
  if (novoEstado != portaAberta && !relayAtivo) {
    portaAberta = novoEstado;
    sendStatus();
    Serial.println(portaAberta ? "Sensor: ABERTA" : "Sensor: FECHADA");
  }

  // Relé timeout
  if (relayAtivo && millis() - relayStart >= RELAY_TIME) {
    digitalWrite(RELAY_PIN, HIGH); digitalWrite(PUPE_PIN, HIGH);
    relayAtivo = false; Serial.println("Rele OFF.");
    bool sa = (digitalRead(SENSOR_PIN) == HIGH);
    if (portaAberta != sa) { portaAberta = sa; sendStatus(); }
  }
}
