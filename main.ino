/*
 * main.ino  —  v4.0
 * Sistema de controle de portas com intertravamento via UDP/WiFi
 * Dispositivos: PORTA_A | PORTA_B | PORTEIRO
 *
 * Modos de WiFi:
 *   NORMAL   — conecta ao roteador com credenciais salvas na EEPROM
 *   CONFIG   — sem credenciais: sobe AP "<DEVICE>_CONFIG" com portal web
 *   FALLBACK — roteador caiu: PORTEIRO vira AP; PORTA_A/B reconectam nele
 */

#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>

// ====================== IDENTIFICAÇÃO =======================
#define DEVICE_NAME  "PORTA_A"   // 👉 altere para "PORTA_A", "PORTA_B" ou "PORTEIRO"
#define DEV_PORTA_A  "PORTA_A"
#define DEV_PORTA_B  "PORTA_B"
#define DEV_PORTEIRO "PORTEIRO"

// ====================== EEPROM ==============================
// Layout: [0..63] ssid | [64..127] password | [128] flag válido (0xAA)
#define EEPROM_SIZE        256
#define EEPROM_SSID_ADDR     0
#define EEPROM_PASS_ADDR    64
#define EEPROM_VALID_ADDR  128
#define EEPROM_VALID_MAGIC 0xAA

// ====================== AP FALLBACK =========================
// 👉 Altere FALLBACK_AP_PASS antes de gravar em produção.
// PORTA_A e PORTA_B usam estas mesmas constantes para conectar no PORTEIRO.
#define FALLBACK_AP_SSID "PORTEIRO_AP"
#define FALLBACK_AP_PASS "porteiro123"
#define FALLBACK_AP_IP   "192.168.4.1"

#define WIFI_CONNECT_TIMEOUT   15000UL   // espera máxima no boot
#define WIFI_RECONNECT_TIMEOUT 20000UL   // espera antes de ativar fallback no loop

// ====================== SEGURANÇA UDP =======================
// Chave compartilhada: todos os ESPs do sistema devem ter a mesma.
// Adicionada automaticamente em cada mensagem enviada e verificada ao receber.
// Elimina comandos de qualquer dispositivo fora do sistema (IP spoofing, etc).
// 👉 Altere antes de gravar em produção.
#define SHARED_KEY "WIRES2025"
#define PORTA_TIMEOUT     300000UL
#define UDP_PORT          4210
#define RELAY_TIME        5000UL
#define MASTER_BUSY_TIMEOUT (RELAY_TIME + 1000UL) // segurança: 1s além do relé
#define SENSOR_OPEN       HIGH  // INPUT_PULLUP: porta aberta = contato aberto = HIGH
#define MASTER_LEASE_TIME 15000UL
#define MASTER_TIMEOUT    20000UL
#define MAX_DEVICES       10

// ====================== PINOS ================================
#define BTN1_PIN   5
#define BTN2_PIN   4
#define BYPASS_PIN 0
#define SENSOR_PIN 14
#define RELAY_PIN  12
#define PUPE_PIN   13

// ====================== STRUCT DEVICE =======================
// Agrupa nome, IP (string) e timestamp do último ping em um único registro.
// Substitui os três arrays separados (knownNames / knownIPs / lastPing).
struct Device
{
  char          name[16];
  char          ip[16];
  unsigned long lastPing;
};

Device devices[MAX_DEVICES];   // tabela de dispositivos conhecidos

// ── helpers inline para a tabela ────────────────────────────
inline void deviceClear(int i)
{
  devices[i].name[0] = '\0';
  devices[i].ip[0]   = '\0';
  devices[i].lastPing = 0;
}

inline bool deviceEmpty(int i)     { return devices[i].name[0] == '\0'; }
inline bool deviceMatch(int i, const char *n) { return strcmp(devices[i].name, n) == 0; }

// ====================== ESTADO WIFI =========================
enum WifiMode { WIFI_MODE_NORMAL, WIFI_MODE_CONFIG, WIFI_MODE_FALLBACK };
WifiMode wifiMode      = WIFI_MODE_NORMAL;
bool     fallbackAtivo = false;
unsigned long wifiDownSince = 0;

ESP8266WebServer webServer(80);

// ====================== VARIÁVEIS ============================
WiFiUDP   udp;
IPAddress localIP;

int  devicePriority = 1;
int  masterPriority = 0;
bool isMaster       = false;
char networkMaster[16] = "";
char tokenOwner[16]    = "";

bool bypassMode    = false;
bool bypassBootOk  = false;        // true após 2s do boot — ignora GPIO0 antes disso
unsigned long bypassBootTime = 0;  // setado no setup()
bool portaAberta   = false;
bool sensorLeitura = false;
bool relayAtivo    = false;
bool masterBusy    = false;
bool alertSent     = false;

unsigned long masterBusyTime    = 0;
unsigned long relayStart        = 0;
unsigned long lastDiscovery     = 0;
unsigned long lastPingSent      = 0;
unsigned long lastStatusSent    = 0;
unsigned long portaAbertaTempo  = 0;
unsigned long masterLeaseStart  = 0;   // momento do último HELLO recebido do master
unsigned long lastOpenEvent     = 0;
unsigned long sensorDebounce    = 0;
unsigned long openToken         = 0;
unsigned long currentToken      = 0;

// ===================== TOKEN LOCK ===========================
bool          tokenActive = false;
char          tokenPorta[16] = "";
unsigned long tokenStart  = 0;       // momento em que o token foi adquirido
#define TOKEN_TIMEOUT 10000UL

// Token expira apenas quando: tempo esgotado E nenhum relé ativo.
// Isso evita liberar o sistema enquanto uma porta ainda está fisicamente abrindo.
static bool tokenExpirou()
{
  if (!tokenActive) return false;
  bool relayOcupado = relayAtivo || portaARelayAtivo || portaBRelayAtivo;
  return !relayOcupado && (millis() - tokenStart > TOKEN_TIMEOUT);
}

bool adquirirToken(const char *porta)
{
  if (!tokenActive || tokenExpirou())
  {
    tokenActive = true;
    tokenStart  = millis();
    strncpy(tokenPorta, porta, sizeof(tokenPorta) - 1);
    tokenPorta[sizeof(tokenPorta) - 1] = '\0';
    return true;
  }
  return strcmp(tokenPorta, porta) == 0;
}

void liberarToken()
{
  tokenActive   = false;
  tokenPorta[0] = '\0';
}

// ===================== ESTADO DAS OUTRAS PORTAS =============
bool portaAAberta = false;
bool portaBAberta = false;
bool portaALock   = false;
bool portaBLock   = false;

bool          aguardandoAutorizacao = false;
unsigned long lastStatusPortaA     = 0;
unsigned long lastStatusPortaB     = 0;
unsigned long lastMasterSeen       = 0;

#define REQ_RETRY_INTERVAL 1000UL
#define REQ_MAX_RETRIES    3
char          reqPorta[16] = "";
int           reqRetryCount = 0;
unsigned long lastReqSent   = 0;

// Watchdog lógico: detecta se o loop travou silenciosamente
// (diferente do watchdog de hardware — este monitora a lógica da aplicação)
unsigned long lastLoopWatchdog = 0;
#define APP_WATCHDOG_TIMEOUT 8000UL  // reinicia se o loop não rodar por 8s
// Envia WIFICFG|ssid|pass a cada 30s enquanto detectar portas no AP (IP 192.168.4.x)
#define WIFICFG_INTERVAL  30000UL
unsigned long lastWificfgSent = 0;

// ===================== EEPROM ================================

void eepromReadStr(int addr, char *buf, int maxLen)
{
  for (int i = 0; i < maxLen - 1; i++)
  {
    buf[i] = EEPROM.read(addr + i);
    if (buf[i] == '\0') break;
  }
  buf[maxLen - 1] = '\0';
}

void eepromWriteStr(int addr, const char *str, int maxLen)
{
  int len = (int)strlen(str);
  int end = (len < maxLen - 1) ? len : maxLen - 1;
  for (int i = 0; i < end; i++)
    EEPROM.write(addr + i, str[i]);
  EEPROM.write(addr + end, '\0');
}

bool eepromHasCredentials()
{
  return EEPROM.read(EEPROM_VALID_ADDR) == EEPROM_VALID_MAGIC;
}

void eepromSaveCredentials(const char *ssid, const char *pass)
{
  eepromWriteStr(EEPROM_SSID_ADDR, ssid, 64);
  eepromWriteStr(EEPROM_PASS_ADDR, pass, 64);
  EEPROM.write(EEPROM_VALID_ADDR, EEPROM_VALID_MAGIC);
  EEPROM.commit();
  Serial.println(F("💾 Credenciais salvas na EEPROM."));
}

// ===================== PORTAL DE CONFIGURAÇÃO ===============

void startConfigPortal()
{
  char apName[32];
  snprintf(apName, sizeof(apName), "%s_CONFIG", DEVICE_NAME);

  Serial.print(F("📡 Sem credenciais. AP de config: "));
  Serial.println(apName);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(apName);
  Serial.print(F("IP do portal: "));
  Serial.println(WiFi.softAPIP());

  webServer.on("/", HTTP_GET, []() {
    static const char PAGE[] PROGMEM =
      "<!DOCTYPE html><html><head>"
      "<meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>Configurar WiFi</title>"
      "<style>"
      "body{font-family:sans-serif;max-width:360px;margin:40px auto;padding:0 16px;background:#f9f9f9;}"
      "h2{color:#1565C0;}p{color:#555;font-size:.9em;}"
      "label{display:block;margin-top:14px;font-size:.85em;color:#777;font-weight:bold;}"
      "input{width:100%;padding:9px;box-sizing:border-box;border:1px solid #ccc;"
      "border-radius:6px;font-size:1em;margin-top:4px;}"
      "button{margin-top:18px;width:100%;padding:11px;background:#1976D2;color:#fff;"
      "border:none;border-radius:6px;font-size:1em;cursor:pointer;font-weight:bold;}"
      "button:hover{background:#1565C0;}"
      ".footer{font-size:.75em;color:#aaa;text-align:center;margin-top:24px;}"
      "</style></head><body>"
      "<h2>Configurar WiFi</h2>"
      "<p>Dispositivo: <strong>" DEVICE_NAME "</strong></p>"
      "<p>Preencha as credenciais da rede WiFi que este dispositivo deve usar.</p>"
      "<form method='POST' action='/save'>"
      "<label>Nome da rede (SSID)</label>"
      "<input name='ssid' type='text' placeholder='Nome do WiFi' required>"
      "<label>Senha</label>"
      "<input name='pass' type='password' placeholder='Senha (deixe vazio se aberta)'>"
      "<button type='submit'>Salvar e conectar</button>"
      "</form>"
      "<div class='footer'>Wires</div>"
      "</body></html>";
    webServer.send_P(200, "text/html", PAGE);
  });

  webServer.on("/save", HTTP_POST, []() {
    String newSsid = webServer.arg("ssid");
    String newPass = webServer.arg("pass");
    if (newSsid.length() == 0)
    {
      webServer.send(400, "text/plain", "SSID não pode ser vazio.");
      return;
    }
    webServer.send(200, "text/html",
      "<!DOCTYPE html><html><head><meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<style>body{font-family:sans-serif;max-width:360px;margin:40px auto;padding:0 16px;text-align:center;}</style>"
      "</head><body>"
      "<h2>✅ Configuração salva!</h2>"
      "<p>Conectando em <strong>" + newSsid + "</strong>...</p>"
      "<p>Reconecte ao seu WiFi normal.</p>"
      "</body></html>");
    delay(600);
    eepromSaveCredentials(newSsid.c_str(), newPass.c_str());
    delay(400);
    ESP.restart();
  });

  // Botão manual: envia WIFICFG para as portas imediatamente
  webServer.on("/sendwifi", HTTP_GET, []() {
    sendWificfg();
    webServer.send(200, "text/html",
      "<!DOCTYPE html><html><head><meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<style>body{font-family:sans-serif;max-width:360px;margin:40px auto;padding:0 16px;text-align:center;}</style>"
      "</head><body>"
      "<h2>📤 Credenciais enviadas!</h2>"
      "<p>As portas PORTA_A e PORTA_B receberão o WiFi e reiniciarão automaticamente.</p>"
      "<p><a href='/'>← Voltar</a></p>"
      "</body></html>");
  });

  webServer.begin();
  wifiMode = WIFI_MODE_CONFIG;
}

// ===================== AP FALLBACK ==========================

void startFallbackAP()
{
  Serial.println(F("🆘 Roteador indisponível! Subindo AP de fallback..."));
  WiFi.mode(WIFI_AP);
  WiFi.softAP(FALLBACK_AP_SSID, FALLBACK_AP_PASS);
  Serial.print(F("AP ativo: ")); Serial.print(FALLBACK_AP_SSID);
  Serial.print(F("  IP: ")); Serial.println(WiFi.softAPIP());

  webServer.on("/", HTTP_GET, []() {
    // FIX heap fragmentation: envia em chunks via WiFiClient em vez de String dinâmica.
    // String += em 24/7 fragmenta o heap do ESP8266 e causa crashes depois de dias.
    unsigned long now    = millis();
    bool aOnline = lastStatusPortaA && (now - lastStatusPortaA < 30000UL);
    bool bOnline = lastStatusPortaB && (now - lastStatusPortaB < 30000UL);

    WiFiClient client = webServer.client();
    webServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
    webServer.send(200, F("text/html"), "");

    client.print(F("<!DOCTYPE html><html><head>"
      "<meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<meta http-equiv='refresh' content='5'>"
      "<title>Status de Emergencia</title>"
      "<style>"
      "body{font-family:sans-serif;max-width:400px;margin:40px auto;padding:0 16px;background:#fff3e0;}"
      "h2{color:#e65100;}"
      ".badge{display:inline-block;padding:3px 10px;border-radius:12px;font-size:.85em;font-weight:bold;}"
      ".open{background:#ffcdd2;color:#c62828;}.closed{background:#c8e6c9;color:#2e7d32;}"
      ".online{background:#c8e6c9;color:#2e7d32;}.offline{background:#ffcdd2;color:#c62828;}"
      "table{width:100%;border-collapse:collapse;margin-top:16px;}"
      "td,th{padding:10px;border:1px solid #ffe0b2;font-size:.9em;}"
      "th{background:#fff8e1;}"
      ".note{font-size:.75em;color:#999;margin-top:16px;text-align:center;}"
      "</style></head><body>"
      "<h2>Modo Emergencia</h2>"
      "<p>Roteador offline. Sistema operando na rede local do PORTEIRO.</p>"
      "<table>"
      "<tr><th>Dispositivo</th><th>Porta</th><th>Rede</th></tr>"
      "<tr><td>PORTEIRO</td><td>-</td>"
      "<td><span class='badge online'>AP Ativo</span></td></tr>"));

    // PORTA_A — dados dinâmicos em prints separados (sem String)
    client.print(F("<tr><td>PORTA_A</td><td><span class='badge "));
    client.print(portaAAberta ? F("open'>Aberta") : F("closed'>Fechada"));
    client.print(F("</span></td><td><span class='badge "));
    client.print(aOnline     ? F("online'>Online") : F("offline'>Sem sinal"));
    client.print(F("</span></td></tr>"));

    // PORTA_B
    client.print(F("<tr><td>PORTA_B</td><td><span class='badge "));
    client.print(portaBAberta ? F("open'>Aberta") : F("closed'>Fechada"));
    client.print(F("</span></td><td><span class='badge "));
    client.print(bOnline      ? F("online'>Online") : F("offline'>Sem sinal"));
    client.print(F("</span></td></tr>"));

    client.print(F("</table><p class='note'>Atualiza a cada 5s</p></body></html>"));
  });

  webServer.begin();
  fallbackAtivo = true;
  wifiMode      = WIFI_MODE_FALLBACK;
  localIP       = WiFi.softAPIP();
  udp.stop();
  udp.begin(UDP_PORT);
}

void connectFallbackAsClient()
{
  Serial.println(F("📡 Conectando no AP do PORTEIRO..."));
  WiFi.mode(WIFI_STA);
  WiFi.begin(FALLBACK_AP_SSID, FALLBACK_AP_PASS);

  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 10000UL)
  {
    ESP.wdtFeed();   // ← watchdog: evita reset durante este loop bloqueante
    delay(300);
    Serial.print(F("."));
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.println(F("✅ Conectado no AP do PORTEIRO!"));
    localIP = WiFi.localIP();
    Serial.print(F("IP: ")); Serial.println(localIP);
    udp.stop();
    udp.begin(UDP_PORT);
    fallbackAtivo = true;
    wifiMode      = WIFI_MODE_FALLBACK;
  }
  else
  {
    Serial.println(F("❌ Não conseguiu conectar no AP do PORTEIRO."));
  }
}

// ===================== FUNÇÕES AUXILIARES ===================

// Converte localIP para string sem alocar String no heap.
// Uso: char ip[16]; localIPStr(ip);
static void localIPStr(char *buf)
{
  snprintf(buf, 16, "%u.%u.%u.%u",
           localIP[0], localIP[1], localIP[2], localIP[3]);
}

// Converte o IP remoto do último pacote UDP para string sem alocar no heap
static void remoteIPStr(char *buf)
{
  IPAddress rip = udp.remoteIP();
  snprintf(buf, 16, "%u.%u.%u.%u", rip[0], rip[1], rip[2], rip[3]);
}

void sendBroadcast(const char *msg)
{
  // Prefixa a chave compartilhada: "KEY|mensagem"
  // Receptores rejeitam qualquer pacote sem a chave correta.
  char frame[280];
  snprintf(frame, sizeof(frame), "%s|%s", SHARED_KEY, msg);
  uint32_t localIPint = (uint32_t)WiFi.localIP();
  uint32_t subnetInt  = (uint32_t)WiFi.subnetMask();
  IPAddress broadcastIP(localIPint | ~subnetInt);
  udp.beginPacket(broadcastIP, UDP_PORT);
  udp.print(frame);
  udp.endPacket();
}

void sendWificfg()
{
  if (strcmp(DEVICE_NAME, DEV_PORTEIRO) != 0) return;
  if (!eepromHasCredentials()) return;
  char ssid[64], pass[64];
  eepromReadStr(EEPROM_SSID_ADDR, ssid, sizeof(ssid));
  eepromReadStr(EEPROM_PASS_ADDR, pass, sizeof(pass));
  char cfgMsg[160];
  snprintf(cfgMsg, sizeof(cfgMsg), "WIFICFG|%s|%s", ssid, pass);
  // Envia diretamente no AP (192.168.4.255) onde as portas sem config estão
  udp.beginPacket(IPAddress(192, 168, 4, 255), UDP_PORT);
  udp.print(cfgMsg);
  udp.endPacket();
  // E também via broadcast normal (caso alguma porta já esteja na rede do roteador)
  sendBroadcast(cfgMsg);
  lastWificfgSent = millis();
  Serial.println(F("📤 WIFICFG enviado para as portas."));
}

void sendStatus()
{
  char msg[64];
  snprintf(msg, sizeof(msg), "STATUS|%s|%s",
           DEVICE_NAME, portaAberta ? "OPEN" : "CLOSED");
  sendBroadcast(msg);
  lastStatusSent = millis();
}

bool deviceKnown(const char *dev)
{
  if (strcmp(dev, DEVICE_NAME) == 0) return true;
  for (int i = 0; i < MAX_DEVICES; i++)
    if (!deviceEmpty(i) && deviceMatch(i, dev)) return true;
  return false;
}

// Valida origem por IP (usado nas verificações de segurança UDP)
bool deviceKnownByIP(const char *ip)
{
  char myIP[16]; localIPStr(myIP);
  if (strcmp(ip, myIP) == 0) return true;  // mensagem de si mesmo
  for (int i = 0; i < MAX_DEVICES; i++)
    if (!deviceEmpty(i) && strcmp(devices[i].ip, ip) == 0) return true;
  return false;
}

void addDevice(const char *dev, const char *ip)
{
  if (!dev || !ip || strcmp(dev, DEVICE_NAME) == 0) return;

  // Atualiza existente
  for (int i = 0; i < MAX_DEVICES; i++)
  {
    if (!deviceEmpty(i) && deviceMatch(i, dev))
    {
      strncpy(devices[i].ip, ip, sizeof(devices[i].ip) - 1);
      devices[i].ip[sizeof(devices[i].ip) - 1] = '\0';
      devices[i].lastPing = millis();
      return;
    }
  }

  // Slot vazio disponível
  for (int i = 0; i < MAX_DEVICES; i++)
  {
    if (deviceEmpty(i))
    {
      strncpy(devices[i].name, dev, sizeof(devices[i].name) - 1);
      devices[i].name[sizeof(devices[i].name) - 1] = '\0';
      strncpy(devices[i].ip,   ip,  sizeof(devices[i].ip)   - 1);
      devices[i].ip[sizeof(devices[i].ip) - 1] = '\0';
      devices[i].lastPing = millis();
      Serial.print(F("[DISCOVERY] ")); Serial.print(dev);
      Serial.print(F(" -> ")); Serial.println(ip);
      return;
    }
  }

  // Tabela cheia: substitui o dispositivo com o lastPing mais antigo
  // Evita que broadcast excessivo na rede bloqueie a entrada de dispositivos legítimos
  int oldest = 0;
  for (int i = 1; i < MAX_DEVICES; i++)
    if (devices[i].lastPing < devices[oldest].lastPing) oldest = i;

  Serial.print(F("⚠️ Tabela cheia: substituindo dispositivo mais antigo: "));
  Serial.println(devices[oldest].name);
  strncpy(devices[oldest].name, dev, sizeof(devices[oldest].name) - 1);
  devices[oldest].name[sizeof(devices[oldest].name) - 1] = '\0';
  strncpy(devices[oldest].ip,   ip,  sizeof(devices[oldest].ip)   - 1);
  devices[oldest].ip[sizeof(devices[oldest].ip) - 1] = '\0';
  devices[oldest].lastPing = millis();
  Serial.print(F("[DISCOVERY] ")); Serial.print(dev);
  Serial.print(F(" -> ")); Serial.println(ip);
}

// ===================== LÓGICA DE MASTER =====================

void assumirMaster()
{
  Serial.println(F("👑 Assumindo papel de master!"));
  masterPriority = devicePriority;
  strncpy(networkMaster, DEVICE_NAME, sizeof(networkMaster) - 1);
  networkMaster[sizeof(networkMaster) - 1] = '\0';
  isMaster         = true;
  masterLeaseStart = millis();  // renova o lease no momento de assumir
  lastMasterSeen   = millis();
}

void abdicarMaster(int novaPrio, const char *novoDev)
{
  Serial.println(F("⚠️ Master com prioridade maior detectado! Abdicando."));
  isMaster       = false;
  masterPriority = novaPrio;
  strncpy(networkMaster, novoDev, sizeof(networkMaster) - 1);
  networkMaster[sizeof(networkMaster) - 1] = '\0';
}

void checkMasterHealth()
{
  if (isMaster) return;
  bool masterConhecido  = (networkMaster[0] != '\0');
  // FIX overflow: millis() - X > intervalo é seguro; millis() > X quebra em ~49 dias
  bool leaseExpirou     = masterConhecido && (millis() - masterLeaseStart > MASTER_LEASE_TIME * 2);
  bool masterSilencioso = masterConhecido && (millis() - lastMasterSeen   > MASTER_TIMEOUT);
  if (leaseExpirou || masterSilencioso)
  {
    Serial.print(F("⚠️ Master perdido ("));
    Serial.print(leaseExpirou ? F("lease expirado") : F("timeout"));
    Serial.println(F(")!"));
    assumirMaster();
  }
}

void processHello(const char *dev, int prio, const char *ip, unsigned long lease)
{
  if (isMaster && prio > devicePriority) abdicarMaster(prio, dev);

  if (strcmp(dev, networkMaster) == 0)
  {
    masterLeaseStart = millis();
    lastMasterSeen   = millis();
  }
  if (prio > masterPriority)
  {
    masterPriority = prio;
    strncpy(networkMaster, dev, sizeof(networkMaster) - 1);
    networkMaster[sizeof(networkMaster) - 1] = '\0';
    isMaster       = (strcmp(dev, DEVICE_NAME) == 0);
    lastMasterSeen = millis();
  }
  else if (prio == masterPriority && strcmp(dev, DEVICE_NAME) != 0)
  {
    char localIPBuf[16]; localIPStr(localIPBuf);
    if (strcmp(ip, localIPBuf) > 0)
    {
      strncpy(networkMaster, dev, sizeof(networkMaster) - 1);
      networkMaster[sizeof(networkMaster) - 1] = '\0';
      isMaster       = false;
      lastMasterSeen = millis();
    }
  }
}

// ===================== INTERTRAVAMENTO ======================

// portaARelayAtivo / portaBRelayAtivo: verdadeiro enquanto o relé da outra porta
// está ativo — mesmo que o sensor ainda não tenha detectado abertura.
// Atualizado via mensagem LOCK (enviada assim que abrirPorta() é chamado).
// Isso fecha a janela de race condition entre relé ativo e sensor ainda LOW.
bool portaARelayAtivo = false;
bool portaBRelayAtivo = false;

// podeAbrir: única fonte de verdade de intertravamento.
// masterOverride = true → porteiro ignorou intertravamento (bypass total do master).
bool podeAbrir(const char *portaAlvo, bool masterOverride = false)
{
  // Cooldown mínimo entre aberturas consecutivas
  if (millis() - lastOpenEvent < 2000UL)
  {
    Serial.println(F("⏳ Aguardando estabilização"));
    return false;
  }

  // Bypass ativo: libera intertravamento de sensor/lock,
  // MAS ainda bloqueia se o relé da outra porta estiver girando
  // (evitar dano mecânico a portas que estejam fisicamente se movendo).
  // masterOverride ignora até isso — decisão total do porteiro.
  if (bypassMode && !masterOverride)
  {
    // Verifica apenas relé ativo (proteção mecânica mínima)
    if (strcmp(portaAlvo, DEV_PORTA_A) == 0 && portaBRelayAtivo)
    {
      Serial.println(F("⚠️ Bypass: PORTA_B com relé ativo, aguarde."));
      return false;
    }
    if (strcmp(portaAlvo, DEV_PORTA_B) == 0 && portaARelayAtivo)
    {
      Serial.println(F("⚠️ Bypass: PORTA_A com relé ativo, aguarde."));
      return false;
    }
    Serial.println(F("✅ Bypass ativo"));
    return true;
  }

  if (masterOverride)
  {
    Serial.println(F("👑 Master override: abrindo sem intertravamento"));
    return true;
  }

  // Intertravamento normal
  if (strcmp(portaAlvo, DEV_PORTA_A) == 0)
  {
    if (lastStatusPortaB == 0 || millis() - lastStatusPortaB > 10000UL)
    {
      // Log com tempo desde último status — essencial para diagnóstico em campo
      if (lastStatusPortaB == 0)
        Serial.println(F("⚠️ PORTA_B: nunca recebeu status! Bloqueando."));
      else
      {
        Serial.print(F("⚠️ PORTA_B: sem comunicação há "));
        Serial.print((millis() - lastStatusPortaB) / 1000UL);
        Serial.println(F("s! Verifique o nó ou o WiFi. Bloqueando."));
      }
      return false;
    }
    if (portaBAberta || portaBLock || portaBRelayAtivo)
    {
      Serial.println(F("🚫 PORTA_B aberta/travada/abrindo!"));
      return false;
    }
    Serial.println(F("✅ PORTA_B livre, pode abrir PORTA_A"));
  }
  else if (strcmp(portaAlvo, DEV_PORTA_B) == 0)
  {
    if (lastStatusPortaA == 0 || millis() - lastStatusPortaA > 10000UL)
    {
      if (lastStatusPortaA == 0)
        Serial.println(F("⚠️ PORTA_A: nunca recebeu status! Bloqueando."));
      else
      {
        Serial.print(F("⚠️ PORTA_A: sem comunicação há "));
        Serial.print((millis() - lastStatusPortaA) / 1000UL);
        Serial.println(F("s! Verifique o nó ou o WiFi. Bloqueando."));
      }
      return false;
    }
    if (portaAAberta || portaALock || portaARelayAtivo)
    {
      Serial.println(F("🚫 PORTA_A aberta/travada/abrindo!"));
      return false;
    }
    Serial.println(F("✅ PORTA_A livre, pode abrir PORTA_B"));
  }
  return true;
}

// ===================== ABERTURA =============================

void abrirPorta()
{
  if (relayAtivo) return;
  Serial.println(F("🚪 Abrindo porta"));
  digitalWrite(RELAY_PIN, LOW);
  digitalWrite(PUPE_PIN,  LOW);
  relayStart    = millis();
  relayAtivo    = true;
  lastOpenEvent = millis();
}

void solicitarAbertura(const char *porta, bool masterOverride = false)
{
  // Caminho rápido: sou master → sem roundtrip de rede
  if (isMaster)
  {
    // FIX race condition: trava imediatamente antes de qualquer validação
    if (masterBusy)
    {
      Serial.println(F("⛔ Master ocupado, aguarde."));
      return;
    }
    masterBusy     = true;
    masterBusyTime = millis();

    if (!podeAbrir(porta, masterOverride))
    {
      masterBusy = false;
      Serial.println(F("❌ Master: abertura bloqueada"));
      return;
    }

    if (strcmp(porta, DEVICE_NAME) == 0)
    {
      Serial.println(F("⚡ Master local: abrindo sem roundtrip"));
      abrirPorta();
      char ackMsg[64];
      snprintf(ackMsg, sizeof(ackMsg), "ACK_OPEN|%s", DEVICE_NAME);
      sendBroadcast(ackMsg);
    }
    else
    {
      Serial.print(F("⚡ Master remoto: OPEN direto para ")); Serial.println(porta);
      if (adquirirToken(porta))
      {
        currentToken = millis();
        strncpy(tokenOwner, DEVICE_NAME, sizeof(tokenOwner) - 1);
        tokenOwner[sizeof(tokenOwner) - 1] = '\0';
        // Flag "M" indica masterOverride: porta destino pula podeAbrir()
        char openMsg[48];
        snprintf(openMsg, sizeof(openMsg), masterOverride ? "OPEN|%s|M" : "OPEN|%s", porta);
        sendBroadcast(openMsg);
      }
      else
      {
        masterBusy = false;
      }
    }
    return;
  }

  // Caminho normal: não sou master
  if (strcmp(porta, DEVICE_NAME) == 0 && podeAbrir(porta))
  {
    Serial.println(F("✅ Abrindo localmente"));
    abrirPorta();
    char ackMsg[64];
    snprintf(ackMsg, sizeof(ackMsg), "ACK_OPEN|%s", DEVICE_NAME);
    sendBroadcast(ackMsg);
    return;
  }

  if (networkMaster[0] != '\0')
  {
    Serial.println(F("📨 Pedindo autorização ao master"));
    openToken = millis();
    char reqMsg[64];
    snprintf(reqMsg, sizeof(reqMsg), "REQ_OPEN|%s|%s|%lu", porta, DEVICE_NAME, openToken);
    sendBroadcast(reqMsg);
    aguardandoAutorizacao = true;
    lastReqSent           = millis();
    reqRetryCount         = 0;
    strncpy(reqPorta, porta, sizeof(reqPorta) - 1);
    reqPorta[sizeof(reqPorta) - 1] = '\0';
  }
  else
    Serial.println(F("❌ Nenhum master disponível"));
}

// ===================== PARSER ================================

static int splitMsg(char *buf, size_t bufSize, const char *src, char **fields, int maxFields)
{
  strncpy(buf, src, bufSize - 1);
  buf[bufSize - 1] = '\0';
  int count = 0;
  char *tok = strtok(buf, "|");
  while (tok && count < maxFields)
  {
    fields[count++] = tok;
    tok = strtok(NULL, "|");
  }
  return count;
}

void processMessage(char *msg)
{
  // Verifica chave compartilhada: rejeita mensagens sem o prefixo correto.
  // Formato esperado: "SHARED_KEY|CMD|..."  ex: "WIRES2025|STATUS|PORTA_A|OPEN"
  // WIFICFG é exceção: enviado antes do dispositivo conhecer a chave.
  const size_t keyLen = strlen(SHARED_KEY);
  if (strncmp(msg, "WIFICFG|", 8) != 0)  // WIFICFG não leva chave (bootstrap)
  {
    if (strncmp(msg, SHARED_KEY, keyLen) != 0 || msg[keyLen] != '|')
    {
      // Silencioso — evita poluir o log com broadcasts legítimos de outros equipamentos
      return;
    }
    msg += keyLen + 1;  // avança ponteiro para depois da chave
  }

  char  buf[256];
  char *f[8];
  int   n;

  if (strncmp(msg, "DISCOVERY|", 10) == 0 || strncmp(msg, "CONFIRM|", 8) == 0)
  {
    n = splitMsg(buf, sizeof(buf), msg, f, 3);
    if (n >= 3)
    {
      addDevice(f[1], f[2]);
      // PORTEIRO detecta porta sem config: IP na faixa do AP (192.168.4.x)
      // Agenda envio imediato de WIFICFG
      if (strcmp(DEVICE_NAME, DEV_PORTEIRO) == 0 && strncmp(f[2], "192.168.4.", 10) == 0)
      {
        Serial.print(F("📋 Porta sem config detectada: ")); Serial.println(f[1]);
        lastWificfgSent = 0; // força envio imediato no próximo ciclo do loop
      }
    }
  }
  // WIFICFG|ssid|pass — recebido pelas portas sem config enquanto no PORTEIRO_AP.
  // Restrições de segurança:
  //   1. Só aceito se o dispositivo ainda não tem credenciais salvas
  //   2. Só aceito se vier do IP 192.168.4.x (AP do PORTEIRO) ou se estivermos em modo fallback
  // Isso evita que alguém na rede do roteador injete credenciais maliciosas.
  else if (strncmp(msg, "WIFICFG|", 8) == 0)
  {
    if (!eepromHasCredentials())
    {
      char remoteIP[16]; remoteIPStr(remoteIP);
      bool origemValida = (strncmp(remoteIP, "192.168.4.", 10) == 0) ||
                          (wifiMode == WIFI_MODE_FALLBACK);
      if (!origemValida)
      {
        Serial.print(F("🚫 WIFICFG rejeitado: origem suspeita ")); Serial.println(remoteIP);
        return;
      }
      n = splitMsg(buf, sizeof(buf), msg, f, 3);
      if (n >= 2 && strlen(f[1]) > 0)
      {
        Serial.print(F("📥 WIFICFG recebido de ")); Serial.print(remoteIP);
        Serial.print(F(" — SSID: ")); Serial.println(f[1]);
        eepromSaveCredentials(f[1], n >= 3 ? f[2] : "");
        delay(300);
        ESP.restart();
      }
    }
  }
  else if (strncmp(msg, "PING|", 5) == 0)
  {
    n = splitMsg(buf, sizeof(buf), msg, f, 3);
    if (n >= 3)
    {
      addDevice(f[1], f[2]);
      char pongMsg[80];
      char localIPBuf[16]; localIPStr(localIPBuf);
      snprintf(pongMsg, sizeof(pongMsg), "%s|PONG|%s|%s", SHARED_KEY, DEVICE_NAME, localIPBuf);
      udp.beginPacket(udp.remoteIP(), UDP_PORT);
      udp.print(pongMsg);
      udp.endPacket();
    }
  }
  else if (strncmp(msg, "HELLO|", 6) == 0)
  {
    n = splitMsg(buf, sizeof(buf), msg, f, 6);
    if (n < 6) return;
    addDevice(f[1], f[3]);
    processHello(f[1], atoi(f[2]), f[3], (unsigned long)atol(f[5]));
  }
  else if (strncmp(msg, "PONG|", 5) == 0)
  {
    n = splitMsg(buf, sizeof(buf), msg, f, 3);
    if (n < 2) return;
    for (int i = 0; i < MAX_DEVICES; i++)
      if (!deviceEmpty(i) && deviceMatch(i, f[1])) { devices[i].lastPing = millis(); break; }
  }
  else if (strncmp(msg, "REQ_OPEN|", 9) == 0)
  {
    if (!isMaster) return;
    n = splitMsg(buf, sizeof(buf), msg, f, 4);
    if (n < 4) return;
    if (!deviceKnown(f[2])) return;
    Serial.print(F("📩 REQ_OPEN: ")); Serial.print(f[1]); Serial.print(F(" de ")); Serial.println(f[2]);

    // FIX race condition: trava masterBusy IMEDIATAMENTE antes de qualquer validação.
    // Sem isso, dois REQ_OPEN chegando em sequência rápida passam ambos pelo podeAbrir()
    // antes de qualquer um setar masterBusy, abrindo as duas portas ao mesmo tempo.
    if (masterBusy)
    {
      Serial.println(F("⛔ DENY — master ocupado (aguarde relé terminar)"));
      char denyMsg[64]; snprintf(denyMsg, sizeof(denyMsg), "DENY|%s", f[1]); sendBroadcast(denyMsg); return;
    }
    masterBusy     = true;   // trava imediata — libera em abrirPorta() ou timeout
    masterBusyTime = millis();

    if (!podeAbrir(f[1]))
    {
      Serial.println(F("🚫 DENY — intertravamento ativo (outra porta aberta/travada/abrindo)"));
      masterBusy = false;    // libera se não vai abrir
      char denyMsg[64]; snprintf(denyMsg, sizeof(denyMsg), "DENY|%s", f[1]); sendBroadcast(denyMsg); return;
    }
    if (adquirirToken(f[1]))
    {
      currentToken   = (unsigned long)atol(f[3]);
      strncpy(tokenOwner, f[2], sizeof(tokenOwner) - 1); tokenOwner[sizeof(tokenOwner)-1] = '\0';
      char allowMsg[64]; snprintf(allowMsg, sizeof(allowMsg), "ALLOW|%s|%lu", f[1], currentToken); sendBroadcast(allowMsg);
    }
    else
    {
      masterBusy = false;    // token não adquirido, libera
      char denyMsg[64]; snprintf(denyMsg, sizeof(denyMsg), "DENY|%s", f[1]); sendBroadcast(denyMsg);
    }
  }
  else if (strncmp(msg, "ALLOW|", 6) == 0)
  {
    n = splitMsg(buf, sizeof(buf), msg, f, 3);
    if (n < 3) return;
    if (strcmp(f[1], DEVICE_NAME) == 0 && (unsigned long)atol(f[2]) == openToken)
    {
      Serial.println(F("✅ Autorização recebida!"));
      lastOpenEvent = millis(); abrirPorta();
      char ackMsg[64]; snprintf(ackMsg, sizeof(ackMsg), "ACK_OPEN|%s", DEVICE_NAME); sendBroadcast(ackMsg);
      aguardandoAutorizacao = false; reqRetryCount = 0; reqPorta[0] = '\0';
    }
  }
  else if (strncmp(msg, "DENY|", 5) == 0)
  {
    if (strcmp(msg + 5, DEVICE_NAME) == 0)
    {
      Serial.println(F("❌ Abertura negada!"));
      aguardandoAutorizacao = false; reqRetryCount = 0; reqPorta[0] = '\0';
    }
  }
  else if (strncmp(msg, "ACK_OPEN|", 9) == 0)
  {
    Serial.print(F("✔ ACK_OPEN de ")); Serial.println(msg + 9);
    if (isMaster) { masterBusy = false; liberarToken(); }
  }
  else if (strncmp(msg, "OPEN|", 5) == 0)
  {
    char remoteIP[16]; remoteIPStr(remoteIP);
    if (!deviceKnownByIP(remoteIP))
    {
      Serial.print(F("🚫 OPEN rejeitado: IP desconhecido ")); Serial.println(remoteIP);
      return;
    }
    char buf2[32];
    strncpy(buf2, msg + 5, sizeof(buf2) - 1);
    buf2[sizeof(buf2) - 1] = '\0';
    char *target   = strtok(buf2, "|");
    char *flagStr  = strtok(NULL, "|");
    bool override  = (flagStr && strcmp(flagStr, "M") == 0);
    if (target && strcmp(target, DEVICE_NAME) == 0)
    {
      if (override || podeAbrir(DEVICE_NAME))
      {
        Serial.println(override ? F("👑 OPEN com master override") : F("✅ OPEN autorizado"));
        abrirPorta();
        char ackMsg[64];
        snprintf(ackMsg, sizeof(ackMsg), "ACK_OPEN|%s", DEVICE_NAME);
        sendBroadcast(ackMsg);
      }
      else
      {
        Serial.println(F("🚫 OPEN bloqueado pelo intertravamento local"));
      }
    }
  }
  else if (strncmp(msg, "LOCK|", 5) == 0)
  {
    char remoteIP[16]; remoteIPStr(remoteIP);
    if (!deviceKnownByIP(remoteIP)) return;
    const char *dev = msg + 5;
    if (strcmp(dev, DEV_PORTA_A) == 0) { portaALock = true;  portaARelayAtivo = true; }
    if (strcmp(dev, DEV_PORTA_B) == 0) { portaBLock = true;  portaBRelayAtivo = true; }
    Serial.print(F("🔒 LOCK de ")); Serial.println(dev);
  }
  else if (strncmp(msg, "UNLOCK|", 7) == 0)
  {
    char remoteIP[16]; remoteIPStr(remoteIP);
    if (!deviceKnownByIP(remoteIP)) return;
    const char *dev = msg + 7;
    if (strcmp(dev, DEV_PORTA_A) == 0) { portaALock = false; portaARelayAtivo = false; }
    if (strcmp(dev, DEV_PORTA_B) == 0) { portaBLock = false; portaBRelayAtivo = false; }
    Serial.print(F("🔓 UNLOCK de ")); Serial.println(dev);
  }
  else if (strncmp(msg, "BYPASS|", 7) == 0)
  {
    char remoteIP[16]; remoteIPStr(remoteIP);
    if (!deviceKnownByIP(remoteIP))
    {
      Serial.print(F("🚫 BYPASS rejeitado: IP desconhecido ")); Serial.println(remoteIP);
      return;
    }
    bypassMode = (strcmp(msg + 7, "ON") == 0);
    Serial.println(bypassMode ? F("⚠️ Bypass ativado!") : F("🔒 Bypass desativado."));
  }
  else if (strncmp(msg, "STATUS|", 7) == 0)
  {
    char remoteIP[16]; remoteIPStr(remoteIP);
    if (!deviceKnownByIP(remoteIP)) return;
    n = splitMsg(buf, sizeof(buf), msg, f, 3);
    if (n < 3) return;
    if (strcmp(f[1], DEV_PORTA_A) == 0)
    {
      bool ant = portaAAberta;
      portaAAberta = (strcmp(f[2], "OPEN") == 0);
      portaALock   = portaAAberta && strcmp(DEVICE_NAME, DEV_PORTA_A) != 0;
      lastStatusPortaA = millis();
      // STATUS conta como sinal de vida — evita falso "Inativo" quando PONG se perde
      for (int i = 0; i < MAX_DEVICES; i++)
        if (!deviceEmpty(i) && deviceMatch(i, f[1])) { devices[i].lastPing = millis(); break; }
      Serial.print(F("[STATUS] PORTA_A: ")); Serial.print(f[2]);
      Serial.println(ant != portaAAberta ? F(" (MUDOU!)") : F(""));
    }
    else if (strcmp(f[1], DEV_PORTA_B) == 0)
    {
      bool ant = portaBAberta;
      portaBAberta = (strcmp(f[2], "OPEN") == 0);
      portaBLock   = portaBAberta && strcmp(DEVICE_NAME, DEV_PORTA_B) != 0;
      lastStatusPortaB = millis();
      // STATUS conta como sinal de vida — evita falso "Inativo" quando PONG se perde
      for (int i = 0; i < MAX_DEVICES; i++)
        if (!deviceEmpty(i) && deviceMatch(i, f[1])) { devices[i].lastPing = millis(); break; }
      Serial.print(F("[STATUS] PORTA_B: ")); Serial.print(f[2]);
      Serial.println(ant != portaBAberta ? F(" (MUDOU!)") : F(""));
    }
  }
  else if (strncmp(msg, "ALERT|", 6) == 0)
  {
    char remoteIP[16]; remoteIPStr(remoteIP);
    if (!deviceKnownByIP(remoteIP)) return;
    n = splitMsg(buf, sizeof(buf), msg, f, 3);
    if (n >= 3)
    {
      Serial.print(F("🚨 ALERTA: ")); Serial.print(f[1]);
      Serial.print(F(" de ")); Serial.println(f[2]);
    }
  }
}

// ===================== SETUP ================================

void setup()
{
  Serial.begin(115200);
  EEPROM.begin(EEPROM_SIZE);

  // Watchdog de software: permite até 8s sem feed antes de resetar
  ESP.wdtEnable(8000);

  pinMode(BTN1_PIN,   INPUT_PULLUP);
  pinMode(BTN2_PIN,   INPUT_PULLUP);
  pinMode(BYPASS_PIN, INPUT_PULLUP);  // ⚠️ GPIO0 — sensível ao boot (ver nota abaixo)
  pinMode(SENSOR_PIN, INPUT_PULLUP);
  pinMode(RELAY_PIN,  OUTPUT);
  pinMode(PUPE_PIN,   OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);
  digitalWrite(PUPE_PIN,  HIGH);

  // ── Guarda do GPIO0 (BYPASS_PIN) ─────────────────────────────────────
  // GPIO0 determina o modo de boot do ESP8266:
  //   LOW  no boot → modo gravação de firmware (não executa o código!)
  //   HIGH no boot → modo normal
  //
  // Se o interruptor de bypass estiver FECHADO quando o ESP ligar,
  // o dispositivo entra em modo de gravação e trava silenciosamente.
  //
  // SOLUÇÃO DE HARDWARE recomendada: coloque um resistor de 10kΩ entre
  // BYPASS_PIN e 3.3V (pull-up externo) para garantir nível HIGH no boot,
  // independente da posição do interruptor.
  //
  // DETECÇÃO em software: lemos o pino 200ms após o boot. Se ainda estiver
  // LOW, o boot já ocorreu normalmente (o ESP não travou), mas avisamos
  // no Serial para facilitar diagnóstico em campo.
  delay(200);  // aguarda estabilização dos pinos após energização
  if (strcmp(DEVICE_NAME, DEV_PORTEIRO) == 0 && digitalRead(BYPASS_PIN) == LOW)
  {
    Serial.println(F("⚠️ AVISO: BYPASS_PIN (GPIO0) está LOW no boot!"));
    Serial.println(F("   Se o sistema não inicializar corretamente, verifique"));
    Serial.println(F("   o interruptor de bypass e adicione pull-up externo 10k."));
    // Não bloqueamos — se chegamos aqui, o boot já foi normal.
    // O bypass será ignorado durante os primeiros 2s (bypassBootGuard).
  }
  // Registra o momento do boot para a guarda do GPIO0.
  // O bypass só será lido no loop() após 2s (bypassBootTime + 2000).
  bypassBootTime = millis();

  memset(devices, 0, sizeof(devices));

  if      (strcmp(DEVICE_NAME, DEV_PORTEIRO) == 0) devicePriority = 3;
  else if (strcmp(DEVICE_NAME, DEV_PORTA_A)  == 0) devicePriority = 2;
  else                                               devicePriority = 1;

  // ── Fase 1: sem credenciais ───────────────────────────────────────
  if (!eepromHasCredentials())
  {
    if (strcmp(DEVICE_NAME, DEV_PORTEIRO) == 0)
    {
      // PORTEIRO: sobe portal web para o usuário configurar o WiFi
      startConfigPortal();
      return;
    }
    else
    {
      // PORTA_A / PORTA_B: conecta no AP do PORTEIRO e aguarda WIFICFG via UDP.
      // Não precisa de portal próprio — o PORTEIRO distribui as credenciais.
      Serial.println(F("📡 Sem credenciais. Conectando no PORTEIRO_AP para receber config..."));
      connectFallbackAsClient();
      if (!fallbackAtivo)
      {
        // PORTEIRO ainda não está disponível — aguarda em loop até ele aparecer
        Serial.println(F("⏳ Aguardando PORTEIRO_AP ficar disponível..."));
        unsigned long tEspera = millis();
        while (!fallbackAtivo)
        {
          ESP.wdtFeed();
          delay(3000);
          connectFallbackAsClient();
          if (millis() - tEspera > 120000UL)
          {
            Serial.println(F("⚠️ PORTEIRO não encontrado em 2min. Reiniciando..."));
            ESP.restart();
          }
        }
      }
      // Conectado no PORTEIRO_AP — inicia UDP e aguarda WIFICFG
      udp.begin(UDP_PORT);
      Serial.println(F("✅ Aguardando credenciais do PORTEIRO via UDP (WIFICFG)..."));
      // Fica em loop leve até receber WIFICFG ou timeout de 5min
      unsigned long tCfg = millis();
      while (millis() - tCfg < 300000UL)
      {
        ESP.wdtFeed();
        int pkt = udp.parsePacket();
        if (pkt)
        {
          char rxBuf[128];
          int len = udp.read(rxBuf, 127);
          if (len > 0)
          {
            rxBuf[len] = '\0';
            // Formato: WIFICFG|ssid|pass
            if (strncmp(rxBuf, "WIFICFG|", 8) == 0)
            {
              char tmp[128];
              strncpy(tmp, rxBuf, sizeof(tmp) - 1);
              tmp[sizeof(tmp) - 1] = '\0';
              char *f0   = strtok(tmp, "|");
              char *ssid = strtok(NULL, "|");
              char *pass = strtok(NULL, "|");
              if (ssid)
              {
                Serial.print(F("📥 WIFICFG recebido! SSID: ")); Serial.println(ssid);
                eepromSaveCredentials(ssid, pass ? pass : "");
                delay(300);
                ESP.restart();
              }
            }
          }
        }
        delay(50);
      }
      Serial.println(F("⚠️ Timeout aguardando WIFICFG. Reiniciando..."));
      ESP.restart();
      return;
    }
  }

  // ── Fase 2: tenta conectar ao roteador ────────────────────────────
  char savedSsid[64], savedPass[64];
  eepromReadStr(EEPROM_SSID_ADDR, savedSsid, sizeof(savedSsid));
  eepromReadStr(EEPROM_PASS_ADDR, savedPass, sizeof(savedPass));

  Serial.print(F("Conectando ao WiFi: ")); Serial.println(savedSsid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(savedSsid, savedPass);

  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < WIFI_CONNECT_TIMEOUT)
  {
    ESP.wdtFeed();   // ← watchdog: loop pode durar até 15s
    delay(400);
    Serial.print(F("."));
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED)
  {
    localIP  = WiFi.localIP();
    wifiMode = WIFI_MODE_NORMAL;
    Serial.println(F("✅ WiFi conectado!"));
    Serial.print(F("IP: ")); Serial.println(localIP);
  }
  else
  {
    Serial.println(F("❌ Roteador indisponível no boot. Ativando fallback."));
    if (strcmp(DEVICE_NAME, DEV_PORTEIRO) == 0) startFallbackAP();
    else                                          connectFallbackAsClient();
  }

  // ── Fase 3: inicializar e anunciar ────────────────────────────────
  udp.begin(UDP_PORT);
  portaAberta   = (digitalRead(SENSOR_PIN) == SENSOR_OPEN);
  sensorLeitura = portaAberta;
  assumirMaster();

  char msg[64];
  char localIPBuf[16]; localIPStr(localIPBuf);
  snprintf(msg, sizeof(msg), "DISCOVERY|%s|%s", DEVICE_NAME, localIPBuf);
  sendBroadcast(msg);
  sendStatus();

  Serial.print(F("Device: ")); Serial.println(DEVICE_NAME);
  Serial.print(F("Modo WiFi: "));
  Serial.println(wifiMode == WIFI_MODE_NORMAL ? F("NORMAL") : F("FALLBACK"));
}

// ===================== LOOP =================================

void loop()
{
  ESP.wdtFeed();   // watchdog de hardware — evita reset por loop lento

  // Watchdog lógico: se este ponto não for atingido por APP_WATCHDOG_TIMEOUT ms,
  // algo travou na lógica (ex: lock infinito, loop bloqueante não previsto).
  // Diferente do wdtFeed() que só monitora o hardware.
  if (lastLoopWatchdog != 0 && millis() - lastLoopWatchdog > APP_WATCHDOG_TIMEOUT)
  {
    Serial.println(F("🔴 Watchdog lógico: loop travado! Reiniciando..."));
    delay(100);
    ESP.restart();
  }
  lastLoopWatchdog = millis();

  // Modo CONFIG: só serve o portal até reiniciar
  if (wifiMode == WIFI_MODE_CONFIG)
  {
    webServer.handleClient();
    return;
  }

  unsigned long now = millis();

  // --- Debounce do sensor ---
  bool leitura = (digitalRead(SENSOR_PIN) == SENSOR_OPEN);
  if (leitura != sensorLeitura && now - sensorDebounce > 100UL)
  {
    sensorDebounce = now;
    sensorLeitura  = leitura;
  }

  // --- Página de status (fallback) ---
  if (fallbackAtivo) webServer.handleClient();

  // ── Gerenciamento de WiFi (modo NORMAL) ───────────────────────────
  if (wifiMode == WIFI_MODE_NORMAL)
  {
    static unsigned long lastReconnect = 0;

    if (WiFi.status() != WL_CONNECTED)
    {
      if (wifiDownSince == 0) wifiDownSince = now;

      if (now - lastReconnect > 5000UL)
      {
        char savedSsid[64], savedPass[64];
        eepromReadStr(EEPROM_SSID_ADDR, savedSsid, sizeof(savedSsid));
        eepromReadStr(EEPROM_PASS_ADDR, savedPass, sizeof(savedPass));
        Serial.println(F("⚠️ Tentando reconectar WiFi..."));
        WiFi.disconnect();
        WiFi.begin(savedSsid, savedPass);
        lastReconnect = now;
      }

      if (now - wifiDownSince > WIFI_RECONNECT_TIMEOUT)
      {
        Serial.println(F("⏱️ Roteador ausente. Ativando fallback."));
        udp.stop();
        if (strcmp(DEVICE_NAME, DEV_PORTEIRO) == 0) startFallbackAP();
        else                                          connectFallbackAsClient();
        wifiDownSince = 0;
      }
    }
    else
    {
      if (wifiDownSince != 0)
      {
        Serial.println(F("✅ Conexão restabelecida!"));
        localIP       = WiFi.localIP();
        wifiDownSince = 0;
        udp.stop();
        udp.begin(UDP_PORT);
        sendStatus();
      }
    }
  }

  if (WiFi.status() != WL_CONNECTED && !fallbackAtivo) return;

  // --- Receber UDP ---
  int packetSize = udp.parsePacket();
  if (packetSize)
  {
    char buffer[256];
    int len = udp.read(buffer, sizeof(buffer) - 1);
    if (len > 0) { buffer[len] = '\0'; processMessage(buffer); }
  }

  // --- Discovery + HELLO a cada 15s ---
  if (now - lastDiscovery > 15000UL)
  {
    char localIPBuf[16]; localIPStr(localIPBuf);
    char msg[64];
    snprintf(msg, sizeof(msg), "DISCOVERY|%s|%s", DEVICE_NAME, localIPBuf);
    sendBroadcast(msg);
    char helloMsg[128];
    snprintf(helloMsg, sizeof(helloMsg), "HELLO|%s|%d|%s|%s|%lu",
             DEVICE_NAME, devicePriority, localIPBuf,
             isMaster ? "MASTER" : "NODE", MASTER_LEASE_TIME);
    sendBroadcast(helloMsg);
    lastDiscovery = now;
  }

  // --- WIFICFG a cada 30s (apenas PORTEIRO, enquanto detectar porta sem config) ---
  if (strcmp(DEVICE_NAME, DEV_PORTEIRO) == 0 && eepromHasCredentials())
  {
    if (lastWificfgSent == 0 || now - lastWificfgSent > WIFICFG_INTERVAL)
    {
      bool temSemConfig = false;
      for (int i = 0; i < MAX_DEVICES; i++)
      {
        if (!deviceEmpty(i) && strncmp(devices[i].ip, "192.168.4.", 10) == 0)
        {
          temSemConfig = true;
          break;
        }
      }
      if (temSemConfig || lastWificfgSent == 0)
        sendWificfg();
      else
        lastWificfgSent = now;
    }
  }

  // --- PING a cada 15s ---
  if (now - lastPingSent > 15000UL)
  {
    char localIPBuf[16]; localIPStr(localIPBuf);
    char pingMsg[64];
    snprintf(pingMsg, sizeof(pingMsg), "PING|%s|%s", DEVICE_NAME, localIPBuf);
    sendBroadcast(pingMsg);
    lastPingSent = now;
  }

  // --- STATUS a cada 15s ---
  if (now - lastStatusSent > 15000UL)
  {
    sendStatus();
    if (lastStatusPortaA != 0 && now - lastStatusPortaA > 20000UL) { portaAAberta = false; portaALock = false; }
    if (lastStatusPortaB != 0 && now - lastStatusPortaB > 20000UL) { portaBAberta = false; portaBLock = false; }
    Serial.print(F("Minha porta: "));  Serial.println(portaAberta   ? F("ABERTA") : F("FECHADA"));
    Serial.print(F("PORTA_A: "));      Serial.print(portaAAberta    ? F("ABERTA") : F("FECHADA"));
    Serial.print(F(" (")); Serial.print(lastStatusPortaA ? now - lastStatusPortaA : 0UL); Serial.println(F("ms)"));
    Serial.print(F("PORTA_B: "));      Serial.print(portaBAberta    ? F("ABERTA") : F("FECHADA"));
    Serial.print(F(" (")); Serial.print(lastStatusPortaB ? now - lastStatusPortaB : 0UL); Serial.println(F("ms)"));
    Serial.print(F("Master: "));       Serial.print(networkMaster); Serial.println(isMaster ? F(" (EU)") : F(""));
    Serial.print(F("WiFi: "));         Serial.println(wifiMode == WIFI_MODE_NORMAL ? F("NORMAL") : F("FALLBACK"));
    Serial.println(F("-------------------------"));
  }

  // --- Remove dispositivos inativos (sem PONG ou STATUS há 45s = 3× intervalo de 15s) ---
  for (int i = 0; i < MAX_DEVICES; i++)
  {
    if (!deviceEmpty(i) && now - devices[i].lastPing > 45000UL)
    {
      Serial.print(F("⚠️ Inativo: ")); Serial.println(devices[i].name);
      if (strcmp(devices[i].name, networkMaster) == 0)
      {
        Serial.println(F("⚠️ Master removido por inatividade!"));
        networkMaster[0] = '\0'; masterPriority = 0; isMaster = false;
      }
      deviceClear(i);
    }
  }

  // --- Botões ---
  if (strcmp(DEVICE_NAME, DEV_PORTEIRO) == 0)
  {
    static bool lastBtn1 = HIGH; static unsigned long lastBtn1Press = 0;
    bool btn1 = digitalRead(BTN1_PIN);
    if (btn1 == LOW && lastBtn1 == HIGH && now - lastBtn1Press > 300UL)
    {
      lastBtn1Press = now;
      Serial.println(F("🔘 Botão 1 - PORTA_A"));
      if (podeAbrir(DEV_PORTA_A, true)) solicitarAbertura(DEV_PORTA_A, true);
      else Serial.println(F("❌ Bloqueado"));
    }
    lastBtn1 = btn1;

    static bool lastBtn2 = HIGH; static unsigned long lastBtn2Press = 0;
    bool btn2 = digitalRead(BTN2_PIN);
    if (btn2 == LOW && lastBtn2 == HIGH && now - lastBtn2Press > 300UL)
    {
      lastBtn2Press = now;
      Serial.println(F("🔘 Botão 2 - PORTA_B"));
      if (podeAbrir(DEV_PORTA_B, true)) solicitarAbertura(DEV_PORTA_B, true);
      else Serial.println(F("❌ Bloqueado"));
    }
    lastBtn2 = btn2;

    // Guarda GPIO0: ignora leitura do bypass nos primeiros 2s do boot.
    // Sem isso, se o interruptor estiver fechado no boot, bypassMode ficaria
    // true permanentemente (o esp não teria travado, mas a lógica estaria errada).
    if (!bypassBootOk && millis() - bypassBootTime > 2000UL)
      bypassBootOk = true;

    if (bypassBootOk)
    {
      bool bypassState = (digitalRead(BYPASS_PIN) == LOW);
      static bool lastBypass = false;
      if (bypassState != lastBypass)
      {
        char bMsg[16]; snprintf(bMsg, sizeof(bMsg), "BYPASS|%s", bypassState ? "ON" : "OFF");
        sendBroadcast(bMsg); lastBypass = bypassState;
        Serial.print(F("🔀 Bypass: ")); Serial.println(bypassState ? F("ON") : F("OFF"));
      }
    }
  }
  else
  {
    static bool lastBtn = HIGH; static unsigned long lastPress = 0;
    bool btnState = digitalRead(BTN1_PIN);
    if (btnState == LOW && lastBtn == HIGH && now - lastPress > 300UL)
    {
      lastPress = now;
      Serial.println(F("🔘 Botão local"));
      solicitarAbertura(DEVICE_NAME);
    }
    lastBtn = btnState;
  }

  // --- Sensor → estado da porta ---
  if (sensorLeitura != portaAberta)
  {
    portaAberta = sensorLeitura;
    if (portaAberta)
    {
      portaAbertaTempo = now; alertSent = false;
      char lockMsg[32]; snprintf(lockMsg, sizeof(lockMsg), "LOCK|%s", DEVICE_NAME); sendBroadcast(lockMsg);
      if      (strcmp(DEVICE_NAME, DEV_PORTA_A) == 0) { portaAAberta=true;  portaALock=true;  lastStatusPortaA=now; }
      else if (strcmp(DEVICE_NAME, DEV_PORTA_B) == 0) { portaBAberta=true;  portaBLock=true;  lastStatusPortaB=now; }
    }
    else
    {
      portaAbertaTempo = 0;
      char unlockMsg[32]; snprintf(unlockMsg, sizeof(unlockMsg), "UNLOCK|%s", DEVICE_NAME); sendBroadcast(unlockMsg);
      if      (strcmp(DEVICE_NAME, DEV_PORTA_A) == 0) { portaAAberta=false; portaALock=false; lastStatusPortaA=now; }
      else if (strcmp(DEVICE_NAME, DEV_PORTA_B) == 0) { portaBAberta=false; portaBLock=false; lastStatusPortaB=now; }
    }
    sendStatus(); lastStatusSent = now;
    Serial.println(portaAberta ? F("🚪 Sensor: ABERTA") : F("🚪 Sensor: FECHADA"));
  }

  // --- Relé timeout ---
  if (relayAtivo && now - relayStart >= RELAY_TIME)
  {
    digitalWrite(RELAY_PIN, HIGH); digitalWrite(PUPE_PIN, HIGH);
    relayAtivo = false;
    Serial.println(F("🔒 Relé desligado."));

    // Libera o master e invalida o token assim que a ação física terminou.
    // Isso: (a) evita travar se o ACK_OPEN se perder na rede,
    //       (b) fecha a micro janela onde o token ainda existia após o relé parar.
    if (masterBusy)
    {
      masterBusy = false;
      Serial.println(F("✅ Master liberado (relé concluído)."));
    }
    liberarToken(); // invalida token independente de masterBusy

    bool sensorAberto = (digitalRead(SENSOR_PIN) == SENSOR_OPEN);
    if (portaAberta != sensorAberto) { portaAberta = sensorAberto; sendStatus(); }
  }

  // --- Alerta porta aberta demais ---
  if (portaAberta && !alertSent && now - portaAbertaTempo > PORTA_TIMEOUT)
  {
    Serial.println(F("⚠️ Porta aberta por muito tempo!"));
    char alertMsg[64]; snprintf(alertMsg, sizeof(alertMsg), "ALERT|PORTA_ABERTA|%s", DEVICE_NAME);
    sendBroadcast(alertMsg); alertSent = true;
  }

  // --- Retry REQ_OPEN ---
  if (aguardandoAutorizacao && now - lastReqSent >= REQ_RETRY_INTERVAL)
  {
    if (reqRetryCount < REQ_MAX_RETRIES)
    {
      reqRetryCount++;
      Serial.print(F("🔁 Retry REQ_OPEN ("));
      Serial.print(reqRetryCount); Serial.print(F("/")); Serial.print(REQ_MAX_RETRIES); Serial.println(F(")"));
      char reqMsg[64]; snprintf(reqMsg, sizeof(reqMsg), "REQ_OPEN|%s|%s|%lu", reqPorta, DEVICE_NAME, openToken);
      sendBroadcast(reqMsg); lastReqSent = now;
    }
    else
    {
      Serial.println(F("⚠️ Sem resposta do master. Desistindo."));
      aguardandoAutorizacao = false; reqRetryCount = 0; reqPorta[0] = '\0';
    }
  }

  // --- Master busy timeout (segurança: libera caso o relé não acione) ---
  if (masterBusy && now - masterBusyTime > MASTER_BUSY_TIMEOUT)
  {
    Serial.println(F("⚠️ Master busy timeout! Liberando por segurança."));
    masterBusy = false; liberarToken(); portaALock = false; portaBLock = false;
  }

  // --- Eleição de master ---
  checkMasterHealth();
}
