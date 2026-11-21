// SaarMesh WiFi extension for the MeshCore simple_repeater — see WifiExt.h
#ifdef WITH_WIFI_EXT

#include "WifiExt.h"
#include "MyMesh.h"

#include <WiFi.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>

extern MyMesh the_mesh;

#ifndef WIFI_EXT_CFG_FILE
  #define WIFI_EXT_CFG_FILE "/wifiext.cfg"
#endif
#define WIFI_EXT_VERSION       "1.0.0"
#define TCP_LINE_MAX           160
#define TCP_REPLY_MAX          512
#define TCP_IDLE_TIMEOUT_MS    (15UL * 60UL * 1000UL)
#define TCP_MAX_AUTH_FAILS     3
#define WIFI_RETRY_MS          30000UL
#define MQTT_RETRY_MS          30000UL
#define DUTY_SAMPLE_MS         60000UL
#define DUTY_SLOTS             61          // 60 min window

// ------------------------------------------------------------------ config --
struct WifiExtCfg {
  bool     wifi_en;
  char     ssid[33];
  char     pwd[65];
  char     hostname[33];
  bool     tcp_en;
  uint16_t tcp_port;
  bool     mqtt_en;
  char     mqtt_host[65];
  uint16_t mqtt_port;
  char     mqtt_user[33];
  char     mqtt_pwd[65];
  char     mqtt_prefix[33];
  uint16_t mqtt_interval;   // seconds
  bool     mqtt_cmd;        // accept advert/reboot buttons from HA
  bool     mqtt_discovery;
  bool     ota_en;
};

static WifiExtCfg cfg;
static fs::FS* _fs = NULL;

static void cfgDefaults() {
  memset(&cfg, 0, sizeof(cfg));
  cfg.wifi_en = false;
  cfg.tcp_en = true;
  cfg.tcp_port = 23;
  cfg.mqtt_en = false;
  cfg.mqtt_port = 1883;
  strcpy(cfg.mqtt_prefix, "meshcore");
  cfg.mqtt_interval = 60;
  cfg.mqtt_cmd = true;
  cfg.mqtt_discovery = true;
  cfg.ota_en = true;
}

static void copyStr(char* dst, size_t dst_sz, const char* src) {
  strncpy(dst, src, dst_sz - 1);
  dst[dst_sz - 1] = 0;
}

static bool parseBool(const char* v, bool* out) {
  if (strcmp(v, "on") == 0 || strcmp(v, "1") == 0 || strcmp(v, "true") == 0) { *out = true; return true; }
  if (strcmp(v, "off") == 0 || strcmp(v, "0") == 0 || strcmp(v, "false") == 0) { *out = false; return true; }
  return false;
}

// key/value table — used for both the config file and the CLI
enum CfgType { T_STR, T_U16, T_BOOL };
struct CfgItem { const char* key; CfgType type; void* ptr; size_t size; bool secret; };

static const CfgItem items[] = {
  { "wifi.enabled",   T_BOOL, &cfg.wifi_en,        0, false },
  { "wifi.ssid",      T_STR,  cfg.ssid,            sizeof(cfg.ssid), false },
  { "wifi.pwd",       T_STR,  cfg.pwd,             sizeof(cfg.pwd), true },
  { "wifi.hostname",  T_STR,  cfg.hostname,        sizeof(cfg.hostname), false },
  { "tcp.enabled",    T_BOOL, &cfg.tcp_en,         0, false },
  { "tcp.port",       T_U16,  &cfg.tcp_port,       0, false },
  { "mqtt.enabled",   T_BOOL, &cfg.mqtt_en,        0, false },
  { "mqtt.host",      T_STR,  cfg.mqtt_host,       sizeof(cfg.mqtt_host), false },
  { "mqtt.port",      T_U16,  &cfg.mqtt_port,      0, false },
  { "mqtt.user",      T_STR,  cfg.mqtt_user,       sizeof(cfg.mqtt_user), false },
  { "mqtt.pwd",       T_STR,  cfg.mqtt_pwd,        sizeof(cfg.mqtt_pwd), true },
  { "mqtt.prefix",    T_STR,  cfg.mqtt_prefix,     sizeof(cfg.mqtt_prefix), false },
  { "mqtt.interval",  T_U16,  &cfg.mqtt_interval,  0, false },
  { "mqtt.cmd",       T_BOOL, &cfg.mqtt_cmd,       0, false },
  { "mqtt.discovery", T_BOOL, &cfg.mqtt_discovery, 0, false },
  { "ota.enabled",    T_BOOL, &cfg.ota_en,         0, false },
};
#define NUM_ITEMS (sizeof(items) / sizeof(items[0]))

static const CfgItem* findItem(const char* key) {
  for (size_t i = 0; i < NUM_ITEMS; i++) {
    if (strcmp(items[i].key, key) == 0) return &items[i];
  }
  return NULL;
}

static bool setItem(const CfgItem* it, const char* val) {
  switch (it->type) {
    case T_STR:  copyStr((char*)it->ptr, it->size, val); return true;
    case T_BOOL: return parseBool(val, (bool*)it->ptr);
    case T_U16: {
      long n = atol(val);
      if (n < 0 || n > 65535) return false;
      *(uint16_t*)it->ptr = (uint16_t)n;
      return true;
    }
  }
  return false;
}

static void formatItem(const CfgItem* it, char* out, size_t out_sz) {
  switch (it->type) {
    case T_STR:
      if (it->secret) snprintf(out, out_sz, "%s", ((char*)it->ptr)[0] ? "******" : "(leer)");
      else snprintf(out, out_sz, "%s", ((char*)it->ptr)[0] ? (char*)it->ptr : "(leer)");
      break;
    case T_BOOL: snprintf(out, out_sz, "%s", *(bool*)it->ptr ? "on" : "off"); break;
    case T_U16:  snprintf(out, out_sz, "%u", *(uint16_t*)it->ptr); break;
  }
}

static void cfgLoad() {
  cfgDefaults();
  if (!_fs || !_fs->exists(WIFI_EXT_CFG_FILE)) return;
  File f = _fs->open(WIFI_EXT_CFG_FILE, "r");
  if (!f) return;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    int eq = line.indexOf('=');
    if (eq <= 0) continue;
    String k = line.substring(0, eq);
    String v = line.substring(eq + 1);
    const CfgItem* it = findItem(k.c_str());
    if (it) setItem(it, v.c_str());
  }
  f.close();
}

static bool cfgSave() {
  if (!_fs) return false;
  File f = _fs->open(WIFI_EXT_CFG_FILE, "w");
  if (!f) return false;
  for (size_t i = 0; i < NUM_ITEMS; i++) {
    const CfgItem* it = &items[i];
    f.print(it->key); f.print('=');
    switch (it->type) {
      case T_STR:  f.print((char*)it->ptr); break;
      case T_BOOL: f.print(*(bool*)it->ptr ? "on" : "off"); break;
      case T_U16:  f.print(*(uint16_t*)it->ptr); break;
    }
    f.print('\n');
  }
  f.close();
  return true;
}

// ------------------------------------------------------------------- state --
static char node_id[13];            // first 6 bytes of pubkey, hex
static bool wifi_started = false;
static bool net_services_up = false;
static unsigned long next_wifi_retry = 0;
static char status_line[32] = "WiFi aus";

static WiFiServer* tcp_server = NULL;
static WiFiClient tcp_client;
static bool tcp_authed = false;
static uint8_t tcp_auth_fails = 0;
static unsigned long tcp_last_activity = 0;
static char tcp_line[TCP_LINE_MAX];
static int tcp_line_len = 0;
static uint8_t tcp_iac_skip = 0;
static unsigned long tcp_lock_until = 0;
static unsigned long mqtt_backoff = MQTT_RETRY_MS;

static WiFiClient mqtt_net;
static PubSubClient mqtt(mqtt_net);
static unsigned long next_mqtt_retry = 0;
static unsigned long next_mqtt_publish = 0;
static bool mqtt_discovery_sent = false;
static unsigned long reboot_at = 0;

static unsigned long duty_ms[DUTY_SLOTS];
static unsigned long duty_air[DUTY_SLOTS];
static int duty_count = 0, duty_head = 0;
static unsigned long next_duty_sample = 0;

static bool ota_running = false;

// ------------------------------------------------------------------- WiFi --
static void stopNetServices() {
  if (tcp_client) tcp_client.stop();
  tcp_authed = false;
  if (tcp_server) { tcp_server->end(); delete tcp_server; tcp_server = NULL; }
  if (mqtt.connected()) mqtt.disconnect();
  if (ota_running) { ArduinoOTA.end(); ota_running = false; }
  net_services_up = false;
  mqtt_discovery_sent = false;
}

static void wifiStop() {
  stopNetServices();
  if (wifi_started) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
  wifi_started = false;
  strcpy(status_line, "WiFi aus");
}

static void wifiStart() {
  wifiStop();
  if (!cfg.wifi_en || cfg.ssid[0] == 0) return;
  const char* hn = cfg.hostname[0] ? cfg.hostname : NULL;
  char def_hn[33];
  if (!hn) { snprintf(def_hn, sizeof(def_hn), "meshcore-%.6s", node_id); hn = def_hn; }
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(hn);
  WiFi.setAutoReconnect(true);
  WiFi.begin(cfg.ssid, cfg.pwd);
  wifi_started = true;
  next_wifi_retry = millis() + WIFI_RETRY_MS;
  strcpy(status_line, "WiFi verbinde...");
  Serial.printf("WiFi: verbinde mit '%s' als %s\n", cfg.ssid, hn);
}

static void startNetServices() {
  if (cfg.tcp_en && cfg.tcp_port) {
    tcp_server = new WiFiServer(cfg.tcp_port);
    tcp_server->begin();
    tcp_server->setNoDelay(true);
  }
  if (cfg.ota_en) {
    const char* hn = WiFi.getHostname();
    if (hn) ArduinoOTA.setHostname(hn);
    ArduinoOTA.setPassword(the_mesh.getNodePrefs()->password);
    ArduinoOTA.setMdnsEnabled(true);
    ArduinoOTA.onStart([]() { Serial.println("OTA: Start"); });
    ArduinoOTA.onEnd([]() { Serial.println("OTA: fertig, Neustart"); });
    ArduinoOTA.onError([](ota_error_t e) { Serial.printf("OTA: Fehler %u\n", e); });
    ArduinoOTA.begin();
    ota_running = true;
  }
  next_mqtt_retry = 0;
  mqtt_backoff = MQTT_RETRY_MS;
  net_services_up = true;
}

// --------------------------------------------------------------- TCP CLI --
static void tcpPrompt() {
  if (tcp_authed) tcp_client.print("> ");
  else tcp_client.print("Passwort: ");
}

static void tcpHandleLine(char* line) {
  if (!tcp_authed) {
    if (strcmp(line, the_mesh.getNodePrefs()->password) == 0) {
      tcp_authed = true;
      tcp_auth_fails = 0;
      tcp_client.printf("\r\nOK - angemeldet an %s (%s, WiFi-Ext %s)\r\n", the_mesh.getNodeName(), FIRMWARE_VERSION, WIFI_EXT_VERSION);
      tcp_client.print("Befehle wie ueber USB. 'exit' beendet die Sitzung.\r\n");
    } else {
      tcp_auth_fails++;
      Serial.printf("TCP-CLI: falsches Passwort von %s\n", tcp_client.remoteIP().toString().c_str());
      tcp_lock_until = millis() + 1500;   // slow down brute force (non-blocking)
      if (tcp_auth_fails >= TCP_MAX_AUTH_FAILS) {
        tcp_client.print("\r\nZu viele Fehlversuche.\r\n");
        tcp_client.stop();
        return;
      }
      tcp_client.print("\r\nFalsches Passwort.\r\n");
    }
    tcpPrompt();
    return;
  }

  if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) {
    tcp_client.print("Tschuess.\r\n");
    tcp_client.stop();
    tcp_authed = false;
    return;
  }
  if (line[0]) {
    static char reply[TCP_REPLY_MAX];
    reply[0] = 0;
    the_mesh.handleCommand(0, line, reply);
    if (reply[0]) { tcp_client.print("  -> "); tcp_client.print(reply); tcp_client.print("\r\n"); }
  }
  tcpPrompt();
}

static void tcpLoop() {
  if (!tcp_server) return;

  if (tcp_server->hasClient()) {
    WiFiClient incoming = tcp_server->available();
    if (tcp_client && tcp_client.connected()) {
      incoming.print("Belegt - es ist bereits eine Sitzung aktiv.\r\n");
      incoming.stop();
    } else {
      tcp_client = incoming;
      tcp_authed = false;
      tcp_auth_fails = 0;
      tcp_line_len = 0;
      tcp_iac_skip = 0;
      tcp_last_activity = millis();
      tcp_client.printf("MeshCore Repeater CLI - %s\r\n", the_mesh.getNodeName());
      tcpPrompt();
    }
  }

  if (!tcp_client) return;
  if (!tcp_client.connected()) { tcp_client.stop(); tcp_authed = false; return; }

  if ((long)(millis() - tcp_last_activity) > (long)TCP_IDLE_TIMEOUT_MS) {
    tcp_client.print("\r\nTimeout.\r\n");
    tcp_client.stop();
    tcp_authed = false;
    return;
  }

  if (tcp_lock_until && (long)(millis() - tcp_lock_until) < 0) return;
  tcp_lock_until = 0;

  int budget = 256;   // don't starve the mesh loop
  while (tcp_client.available() && budget-- > 0) {
    int b = tcp_client.read();
    if (b < 0) break;
    tcp_last_activity = millis();
    if (tcp_iac_skip) { tcp_iac_skip--; continue; }
    if (b == 0xFF) { tcp_iac_skip = 2; continue; }      // telnet IAC sequence
    if (b == '\r' || b == '\n') {
      if (tcp_line_len == 0 && b == '\n') continue;      // \r\n pair
      tcp_line[tcp_line_len] = 0;
      tcp_line_len = 0;
      tcpHandleLine(tcp_line);
      if (!tcp_client) return;
      continue;
    }
    if (b == 0x08 || b == 0x7F) { if (tcp_line_len > 0) tcp_line_len--; continue; }
    if (b < 0x20) continue;
    if (tcp_line_len < TCP_LINE_MAX - 1) tcp_line[tcp_line_len++] = (char)b;
  }
}

// ------------------------------------------------------------------- MQTT --
static void topic(char* out, size_t sz, const char* leaf) {
  snprintf(out, sz, "%s/%s/%s", cfg.mqtt_prefix, node_id, leaf);
}

struct SensorDef {
  const char* key; const char* name; const char* unit; const char* dev_class;
  const char* state_class; bool diagnostic; const char* icon; int precision;
};

static const SensorDef sensors_def[] = {
  { "batt_v",      "Batteriespannung",          "V",   "voltage",         "measurement",      false, NULL, 2 },
  { "batt_pct",    "Akku",                      "%",   "battery",         "measurement",      false, NULL, 0 },
  { "noise_floor", "Grundrauschen",             "dBm", "signal_strength", "measurement",      false, "mdi:waveform", 0 },
  { "last_rssi",   "Letzter RSSI",              "dBm", "signal_strength", "measurement",      false, NULL, 0 },
  { "last_snr",    "Letzter SNR",               "dB",  NULL,              "measurement",      false, "mdi:signal-variant", 2 },
  { "duty_1h",     "Sendeanteil (1 h)",         "%",   NULL,              "measurement",      false, "mdi:radio-tower", 2 },
  { "neighbours",  "Nachbarn",                  NULL,  NULL,              "measurement",      false, "mdi:access-point-network", -1 },
  { "tx_queue",    "Sendewarteschlange",        NULL,  NULL,              "measurement",      false, "mdi:tray-full", -1 },
  { "pkt_recv",    "Pakete empfangen",          NULL,  NULL,              "total_increasing", false, "mdi:download", -1 },
  { "pkt_sent",    "Pakete gesendet",           NULL,  NULL,              "total_increasing", false, "mdi:upload", -1 },
  { "recv_flood",  "Empfangen Flood",           NULL,  NULL,              "total_increasing", false, "mdi:download-multiple", -1 },
  { "recv_direct", "Empfangen Direkt",          NULL,  NULL,              "total_increasing", false, "mdi:download", -1 },
  { "sent_flood",  "Gesendet Flood",            NULL,  NULL,              "total_increasing", false, "mdi:upload-multiple", -1 },
  { "sent_direct", "Gesendet Direkt",           NULL,  NULL,              "total_increasing", false, "mdi:upload", -1 },
  { "flood_dups",  "Duplikate Flood",           NULL,  NULL,              "total_increasing", true,  "mdi:content-duplicate", -1 },
  { "direct_dups", "Duplikate Direkt",          NULL,  NULL,              "total_increasing", true,  "mdi:content-duplicate", -1 },
  { "recv_errors", "Empfangsfehler",            NULL,  NULL,              "total_increasing", true,  "mdi:alert-circle-outline", -1 },
  { "air_tx",      "Sendezeit gesamt",          "s",   "duration",        "total_increasing", true,  NULL, 0 },
  { "air_rx",      "Empfangszeit gesamt",       "s",   "duration",        "total_increasing", true,  NULL, 0 },
  { "uptime",      "Laufzeit",                  "s",   "duration",        "total_increasing", true,  NULL, 0 },
  { "mcu_temp",    "MCU-Temperatur",            "°C",  "temperature",     "measurement",      true,  NULL, 1 },
  { "wifi_rssi",   "WLAN-Signal",               "dBm", "signal_strength", "measurement",      true,  NULL, 0 },
  { "heap_free",   "Freier Heap",               "B",   "data_size",       "measurement",      true,  NULL, 0 },
  { "err_flags",   "Fehlerflags",               NULL,  NULL,              NULL,               true,  "mdi:alert", -1 },
  { "ip",          "IP-Adresse",                NULL,  NULL,              NULL,               true,  "mdi:ip-network", -1 },
  { "firmware",    "Firmware",                  NULL,  NULL,              NULL,               true,  "mdi:chip", -1 },
};
#define NUM_SENSORS (sizeof(sensors_def) / sizeof(sensors_def[0]))

struct ButtonDef { const char* key; const char* name; const char* payload; const char* dev_class; const char* icon; };
static const ButtonDef buttons_def[] = {
  { "advert",       "Advert senden (Zero-Hop)", "advert",       NULL,      "mdi:bullhorn" },
  { "flood_advert", "Advert senden (Flood)",    "flood_advert", NULL,      "mdi:bullhorn-variant" },
  { "reboot",       "Neustart",                 "reboot",       "restart", NULL },
};
#define NUM_BUTTONS (sizeof(buttons_def) / sizeof(buttons_def[0]))

static void deviceBlock(char* out, size_t sz) {
  snprintf(out, sz,
    "\"dev\":{\"ids\":[\"meshcore_%s\"],\"name\":\"%s\",\"mf\":\"Heltec\",\"mdl\":\"WiFi LoRa 32 V4 - MeshCore Repeater\",\"sw\":\"%s+wifi%s\"}",
    node_id, the_mesh.getNodeName(), FIRMWARE_VERSION, WIFI_EXT_VERSION);
}

static void publishDiscovery() {
  char t[128], dev[256], avail[96], state[96], payload[900];
  deviceBlock(dev, sizeof(dev));
  topic(avail, sizeof(avail), "availability");
  topic(state, sizeof(state), "state");

  for (size_t i = 0; i < NUM_SENSORS; i++) {
    const SensorDef& s = sensors_def[i];
    snprintf(t, sizeof(t), "homeassistant/sensor/meshcore_%s/%s/config", node_id, s.key);
    int n = snprintf(payload, sizeof(payload),
      "{\"name\":\"%s\",\"uniq_id\":\"meshcore_%s_%s\",\"stat_t\":\"%s\",\"avty_t\":\"%s\","
      "\"val_tpl\":\"{{ value_json.%s }}\",%s",
      s.name, node_id, s.key, state, avail, s.key, dev);
    if (s.unit)        n += snprintf(payload + n, sizeof(payload) - n, ",\"unit_of_meas\":\"%s\"", s.unit);
    if (s.dev_class)   n += snprintf(payload + n, sizeof(payload) - n, ",\"dev_cla\":\"%s\"", s.dev_class);
    if (s.state_class) n += snprintf(payload + n, sizeof(payload) - n, ",\"stat_cla\":\"%s\"", s.state_class);
    if (s.diagnostic)  n += snprintf(payload + n, sizeof(payload) - n, ",\"ent_cat\":\"diagnostic\"");
    if (s.icon)        n += snprintf(payload + n, sizeof(payload) - n, ",\"ic\":\"%s\"", s.icon);
    if (s.precision >= 0) n += snprintf(payload + n, sizeof(payload) - n, ",\"sug_dsp_prc\":%d", s.precision);
    snprintf(payload + n, sizeof(payload) - n, "}");
    mqtt.publish(t, payload, true);
  }

  char cmd[96];
  topic(cmd, sizeof(cmd), "cmd");
  for (size_t i = 0; i < NUM_BUTTONS; i++) {
    const ButtonDef& b = buttons_def[i];
    snprintf(t, sizeof(t), "homeassistant/button/meshcore_%s/%s/config", node_id, b.key);
    if (!cfg.mqtt_cmd) { mqtt.publish(t, "", true); continue; }   // remove buttons
    int n = snprintf(payload, sizeof(payload),
      "{\"name\":\"%s\",\"uniq_id\":\"meshcore_%s_btn_%s\",\"cmd_t\":\"%s\",\"pl_prs\":\"%s\",\"avty_t\":\"%s\",%s",
      b.name, node_id, b.key, cmd, b.payload, avail, dev);
    if (b.dev_class) n += snprintf(payload + n, sizeof(payload) - n, ",\"dev_cla\":\"%s\"", b.dev_class);
    if (b.icon)      n += snprintf(payload + n, sizeof(payload) - n, ",\"ic\":\"%s\"", b.icon);
    snprintf(payload + n, sizeof(payload) - n, "}");
    mqtt.publish(t, payload, true);
  }
  mqtt_discovery_sent = true;
}

static float dutyLastHour() {
  if (duty_count < 2) return 0.0f;
  int newest = (duty_head - 1 + DUTY_SLOTS) % DUTY_SLOTS;
  int oldest = (duty_head - duty_count + DUTY_SLOTS) % DUTY_SLOTS;
  unsigned long dt = duty_ms[newest] - duty_ms[oldest];
  if (dt == 0) return 0.0f;
  unsigned long dair = duty_air[newest] - duty_air[oldest];
  return 100.0f * (float)dair / (float)dt;
}

static void sampleDuty() {
  duty_ms[duty_head] = millis();
  duty_air[duty_head] = the_mesh.getTxAirMillis();
  duty_head = (duty_head + 1) % DUTY_SLOTS;
  if (duty_count < DUTY_SLOTS) duty_count++;
}

static void escapeJson(char* out, size_t sz, const char* in) {
  size_t j = 0;
  for (size_t i = 0; in[i] && j + 2 < sz; i++) {
    char c = in[i];
    if (c == '"' || c == '\\') { out[j++] = '\\'; out[j++] = c; }
    else if ((uint8_t)c >= 0x20) out[j++] = c;
  }
  out[j] = 0;
}

static void publishState() {
  RepeaterStats st;
  the_mesh.fillStats(st);
  float v = st.batt_milli_volts / 1000.0f;
  int pct = (int)((st.batt_milli_volts - 3300) * 100L / (4200 - 3300));
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  float temp = board.getMCUTemperature();
  char temp_s[16];
  if (isnan(temp)) strcpy(temp_s, "null"); else snprintf(temp_s, sizeof(temp_s), "%.1f", temp);

  char fw[48];
  snprintf(fw, sizeof(fw), "%s+wifi%s", FIRMWARE_VERSION, WIFI_EXT_VERSION);
  char name[64];
  escapeJson(name, sizeof(name), the_mesh.getNodeName());

  char payload[900];
  snprintf(payload, sizeof(payload),
    "{\"name\":\"%s\",\"batt_mv\":%u,\"batt_v\":%.3f,\"batt_pct\":%d,\"noise_floor\":%d,\"last_rssi\":%d,"
    "\"last_snr\":%.2f,\"duty_1h\":%.3f,\"neighbours\":%d,\"tx_queue\":%u,\"pkt_recv\":%lu,\"pkt_sent\":%lu,"
    "\"recv_flood\":%lu,\"recv_direct\":%lu,\"sent_flood\":%lu,\"sent_direct\":%lu,\"flood_dups\":%u,"
    "\"direct_dups\":%u,\"recv_errors\":%lu,\"air_tx\":%lu,\"air_rx\":%lu,\"uptime\":%lu,\"mcu_temp\":%s,"
    "\"wifi_rssi\":%d,\"heap_free\":%u,\"err_flags\":%u,\"ip\":\"%s\",\"firmware\":\"%s\"}",
    name, st.batt_milli_volts, v, pct, st.noise_floor, st.last_rssi,
    st.last_snr / 4.0f, dutyLastHour(), the_mesh.getNeighbourCount(), st.curr_tx_queue_len,
    (unsigned long)st.n_packets_recv, (unsigned long)st.n_packets_sent,
    (unsigned long)st.n_recv_flood, (unsigned long)st.n_recv_direct,
    (unsigned long)st.n_sent_flood, (unsigned long)st.n_sent_direct, st.n_flood_dups,
    st.n_direct_dups, (unsigned long)st.n_recv_errors, (unsigned long)st.total_air_time_secs,
    (unsigned long)st.total_rx_air_time_secs, (unsigned long)st.total_up_time_secs, temp_s,
    (int)WiFi.RSSI(), (unsigned)ESP.getFreeHeap(), st.err_events,
    WiFi.localIP().toString().c_str(), fw);

  char t[96];
  topic(t, sizeof(t), "state");
  mqtt.publish(t, payload, true);
}

static void mqttCallback(char* tp, byte* data, unsigned int len) {
  if (!cfg.mqtt_cmd) return;
  char msg[32];
  if (len >= sizeof(msg)) return;
  memcpy(msg, data, len);
  msg[len] = 0;
  Serial.printf("MQTT cmd: %s\n", msg);
  if (strcmp(msg, "advert") == 0) {
    the_mesh.sendSelfAdvertisement(1500, false);
  } else if (strcmp(msg, "flood_advert") == 0) {
    the_mesh.sendSelfAdvertisement(1500, true);
  } else if (strcmp(msg, "reboot") == 0) {
    reboot_at = millis() + 2000;   // let MQTT flush first
  } else if (strcmp(msg, "publish") == 0) {
    next_mqtt_publish = 0;
  }
}

static void mqttLoop() {
  if (!cfg.mqtt_en || cfg.mqtt_host[0] == 0) {
    if (mqtt.connected()) mqtt.disconnect();
    return;
  }
  if (!mqtt.connected()) {
    if (next_mqtt_retry && (long)(millis() - next_mqtt_retry) < 0) return;
    next_mqtt_retry = millis() + mqtt_backoff;
    if (mqtt_backoff < 300000UL) mqtt_backoff *= 2;   // 30 s .. 5 min while broker is down
    mqtt.setServer(cfg.mqtt_host, cfg.mqtt_port);
    mqtt.setBufferSize(1024);
    mqtt.setKeepAlive(60);
    mqtt.setSocketTimeout(3);
    mqtt.setCallback(mqttCallback);
    char client_id[32], avail[96];
    snprintf(client_id, sizeof(client_id), "meshcore-%s", node_id);
    topic(avail, sizeof(avail), "availability");
    bool ok = mqtt.connect(client_id,
                           cfg.mqtt_user[0] ? cfg.mqtt_user : NULL,
                           cfg.mqtt_user[0] ? cfg.mqtt_pwd : NULL,
                           avail, 0, true, "offline");
    if (!ok) {
      Serial.printf("MQTT: Verbindung zu %s:%u fehlgeschlagen (rc=%d)\n", cfg.mqtt_host, cfg.mqtt_port, mqtt.state());
      return;
    }
    Serial.println("MQTT: verbunden");
    mqtt_backoff = MQTT_RETRY_MS;
    mqtt.publish(avail, "online", true);
    char cmd[96];
    topic(cmd, sizeof(cmd), "cmd");
    mqtt.subscribe(cmd);
    mqtt_discovery_sent = false;
    next_mqtt_publish = millis() + 3000;   // first state shortly after discovery
  }
  mqtt.loop();
  if (cfg.mqtt_discovery && !mqtt_discovery_sent) publishDiscovery();
  if ((long)(millis() - next_mqtt_publish) >= 0) {
    next_mqtt_publish = millis() + (unsigned long)max((int)cfg.mqtt_interval, 10) * 1000UL;
    publishState();
  }
}

// -------------------------------------------------------------- CLI hook --
static void statusReply(char* reply) {
  if (!cfg.wifi_en) { strcpy(reply, "WiFi: aus"); return; }
  if (WiFi.status() == WL_CONNECTED) {
    sprintf(reply, "WiFi: %s %s %ddBm | TCP:%s | MQTT:%s | OTA:%s",
      cfg.ssid, WiFi.localIP().toString().c_str(), (int)WiFi.RSSI(),
      tcp_server ? (tcp_client && tcp_client.connected() ? "Sitzung" : "an") : "aus",
      !cfg.mqtt_en ? "aus" : (mqtt.connected() ? "verbunden" : "getrennt"),
      ota_running ? "an" : "aus");
  } else {
    sprintf(reply, "WiFi: verbinde mit '%s' (Status %d)", cfg.ssid, (int)WiFi.status());
  }
}

bool wifi_ext_handle_command(uint32_t sender_timestamp, const char* command, char* reply) {
  if (strcmp(command, "wifi") == 0 || strcmp(command, "wifi.status") == 0 || strcmp(command, "get wifi") == 0) {
    statusReply(reply);
    return true;
  }
  if (strcmp(command, "wifi.reconnect") == 0) {
    wifiStart();
    strcpy(reply, cfg.wifi_en ? "OK - WiFi wird neu verbunden" : "Err - WiFi ist aus (set wifi.enabled on)");
    return true;
  }
  if (strcmp(command, "mqtt.publish") == 0) {
    if (!mqtt.connected()) { strcpy(reply, "Err - MQTT nicht verbunden"); return true; }
    mqtt_discovery_sent = false;
    next_mqtt_publish = 0;
    strcpy(reply, "OK - Discovery + Status werden gesendet");
    return true;
  }
  if (strcmp(command, "wifi.config") == 0) {
    // compact overview without secrets (fits the LoRa reply size)
    int n = 0;
    char v[70];
    reply[0] = 0;
    for (size_t i = 0; i < NUM_ITEMS && n < 150; i++) {
      if (items[i].secret) continue;
      formatItem(&items[i], v, sizeof(v));
      int w = snprintf(reply + n, 160 - n, "%s%s=%s", n ? " " : "", items[i].key, v);
      if (w < 0 || n + w >= 159) { reply[n] = 0; break; }
      n += w;
    }
    return true;
  }

  bool is_set = memcmp(command, "set ", 4) == 0;
  bool is_get = memcmp(command, "get ", 4) == 0;
  if (!is_set && !is_get) return false;

  const char* p = command + 4;
  if (memcmp(p, "wifi.", 5) && memcmp(p, "tcp.", 4) && memcmp(p, "mqtt.", 5) && memcmp(p, "ota.", 4)) return false;

  char key[24];
  int k = 0;
  while (p[k] && p[k] != ' ' && k < (int)sizeof(key) - 1) { key[k] = p[k]; k++; }
  key[k] = 0;
  const char* val = p + k;
  while (*val == ' ') val++;

  const CfgItem* it = findItem(key);
  if (!it) { sprintf(reply, "Err - unbekannt: %s", key); return true; }

  if (is_get) {
    char v[70];
    formatItem(it, v, sizeof(v));
    sprintf(reply, "> %s", v);
    return true;
  }

  if (!setItem(it, val)) { strcpy(reply, "Err - ungueltiger Wert"); return true; }
  if (!cfgSave()) { strcpy(reply, "Err - Speichern fehlgeschlagen"); return true; }

  // apply changes
  if (memcmp(key, "wifi.", 5) == 0) {
    if (strcmp(key, "wifi.enabled") == 0 || cfg.wifi_en) wifiStart();
    strcpy(reply, "OK - WiFi wird neu gestartet");
  } else if (memcmp(key, "tcp.", 4) == 0 || memcmp(key, "ota.", 4) == 0) {
    if (net_services_up) { stopNetServices(); }   // restarted by loop when connected
    strcpy(reply, "OK");
  } else {  // mqtt.*
    if (mqtt.connected()) mqtt.disconnect();
    next_mqtt_retry = 0;
    mqtt_backoff = MQTT_RETRY_MS;
    mqtt_discovery_sent = false;
    strcpy(reply, "OK");
  }
  return true;
}

// ----------------------------------------------------------------- public --
void wifi_ext_begin(fs::FS* fs) {
  _fs = fs;
  mesh::Utils::toHex(node_id, the_mesh.getSelfId().pub_key, 6);
  for (char* c = node_id; *c; c++) *c = tolower(*c);
  cfgLoad();
  if (cfg.wifi_en && cfg.ssid[0]) wifiStart();
  else WiFi.mode(WIFI_OFF);
}

void wifi_ext_loop() {
  if ((long)(millis() - next_duty_sample) >= 0) {
    next_duty_sample = millis() + DUTY_SAMPLE_MS;
    sampleDuty();
  }

  if (reboot_at && (long)(millis() - reboot_at) >= 0) {
    if (mqtt.connected()) {
      char avail[96];
      topic(avail, sizeof(avail), "availability");
      mqtt.publish(avail, "offline", true);
      mqtt.disconnect();
    }
    board.reboot();
  }

  if (!wifi_started) return;

  if (WiFi.status() == WL_CONNECTED) {
    if (!net_services_up) {
      startNetServices();
      snprintf(status_line, sizeof(status_line), "IP %s", WiFi.localIP().toString().c_str());
      Serial.printf("WiFi: verbunden, %s (%d dBm)\n", WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
    }
    if (ota_running) ArduinoOTA.handle();
    tcpLoop();
    mqttLoop();
  } else {
    if (net_services_up) {
      stopNetServices();
      strcpy(status_line, "WiFi getrennt");
      Serial.println("WiFi: Verbindung verloren");
    }
    // don't fight the built-in 'start ota' hotspot (AP mode)
    if (((long)(millis() - next_wifi_retry) >= 0) && !(WiFi.getMode() & WIFI_AP)) {
      next_wifi_retry = millis() + WIFI_RETRY_MS;
      WiFi.disconnect();
      WiFi.begin(cfg.ssid, cfg.pwd);
    }
  }
}

bool wifi_ext_is_active() {
  return wifi_started;
}

const char* wifi_ext_status_line() {
  return status_line;
}

#endif // WITH_WIFI_EXT
