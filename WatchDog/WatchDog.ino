/*
  WatchDog · 家庭网络看门狗 + 迷你文件服务器
  ------------------------------------------------------------
  硬件：WeMos D1 / NodeMCU 等 ESP8266（4MB Flash）
  依赖：仅官方 ESP8266 core 3.x，无需第三方库

  功能：
    1. 定时探测网络（DNS 往返 + TCP 连接），记录延迟与断网区间
    2. 网页仪表盘：实时状态、延迟曲线、可用率、断网时间线、信号强度
    3. 迷你文件服务器：上传 / 下载 / 删除，Flash 直接当网盘
    4. 首次使用自动开热点（WatchDog-Setup）配网，之后走 STA
    5. 板载 LED 指示：常亮=正常，双闪=断网，快闪=WiFi 掉线
    6. 支持网页 OTA 升级固件

  编译：开发板 Generic ESP8266 Module / Flash 4MB (1M LittleFS) / DOUT / 80MHz
  烧录：0x0 写固件，0x300000 写文件系统镜像
*/

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <ESP8266mDNS.h>
#include <Updater.h>
#include <time.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>

// ─────────────── 常量 ───────────────
#define FW_VERSION      "1.3.2"
#define PIN_LED         2
#define LED_ON          LOW
#define LED_OFF         HIGH
#define RING_SIZE       720          // 最多保留 720 个采样点
#define DEFAULT_AP      "WatchDog-Setup"
#define CONFIG_PATH     "/config.txt"
#define HISTORY_PATH    "/hist.bin"
#define UPLOAD_DIR      "/upload"
#define HEAP_GUARD_MIN  6000       // 空闲堆低于这个值且持续 5 分钟 → 主动重启
#define HEAP_GUARD_MS   300000UL

// ─────────────── 配置 ───────────────
struct Config {
  String  ssid;
  String  pass;
  String  ssid2;                    // 备用 WiFi：主 SSID 连不上时自动切过去
  String  pass2;
  String  webpass;                  // 网页访问密码（空 = 不验证）
  String  host = "www.baidu.com";   // TCP 探测目标
  String  dns  = "223.5.5.5";       // DNS 探测服务器
  String  ip;                       // 固定 IP（空 = 用 DHCP）
  String  gw;                       // 网关（配合固定 IP）
  String  mask;                     // 子网掩码（配合固定 IP，默认 255.255.255.0）
  String  sendkey;                  // Server 酱 SendKey（空 = 不推送）
  uint16_t port = 80;
  uint16_t interval = 60;           // 采样间隔（秒）
  int8_t  tz = 8;                   // 时区（小时）
  uint8_t daily = 1;                // 每日 08:00 简报（1=开）
} cfg;

// ─────────────── 运行时状态 ───────────────
ESP8266WebServer server(80);
DNSServer        dnsServer;
bool  setupMode = false;
bool  timeReady = false;
bool  netOk     = false;
uint16_t rttNow = 0;
uint16_t gwRtt  = 0;
uint32_t lastSampleAt = 0;
uint32_t bootAt = 0;
uint16_t failRun = 0;              // 连续失败次数（用于去抖）

struct Rec {
  uint32_t ts;
  uint16_t rtt;
  uint8_t  ok;
};
Rec      ring[RING_SIZE];
uint16_t ringHead = 0;             // 下一个写入位置
uint16_t ringLen  = 0;
// ─────────────── 健康计数（只增不减，溢出归零也无所谓） ───────────────
uint16_t wifiDrops   = 0;          // 连上后掉线的次数
uint16_t staTries    = 0;          // 本次开机尝试关联的次数
uint8_t  lastProbeErr = 0;         // 0=正常 1=外网断（网关还通） 2=局域网断（网关不通）
uint8_t  activeAp     = 0;         // 当前用的是第几组 WiFi：0=主，1=备

// ─────────────── 断网事件（落盘，重启不丢） ───────────────
// 40 条 × 8 B = 320 B 静态 RAM，固定环形，写满就覆盖最旧的 —— 不会越用越满。
#define OUT_RING 40
#define OUT_PATH "/outages.bin"
struct OutageRec {
  uint32_t ts;                     // 断网开始时间（NTP 时间；没对上时是开机秒数）
  uint16_t dur;                    // 持续秒数
  uint8_t  rssi;                   // 断网时信号（截断成单字节）
};
OutageRec outRing[OUT_RING];
uint8_t  outHead = 0;
uint8_t  outLen  = 0;
bool     inOutage = false;
uint32_t outageStartTs = 0;
uint32_t outageStartMs = 0;

// 分位数统计用的临时缓冲（720 × 2 = 1,440 B 静态内存，固定大小，不随运行时间增长）
static uint16_t statBuf[RING_SIZE];

static int cmpU16(const void *a, const void *b) {
  uint16_t x = *(const uint16_t *)a, y = *(const uint16_t *)b;
  return (x < y) ? -1 : ((x > y) ? 1 : 0);
}

// 一组候选 WiFi（主 / 备）。必须放在全局：Arduino 会把函数原型插到最前面，
// 结构体不前置的话 scanTargets(ApTarget*, ...) 的原型先出现，编译不过。
struct ApTarget {
  String  ssid;
  String  pass;
  int32_t ch       = 0;
  uint8_t bssid[6] = {0};
  int16_t rssi     = -32768;
};


// ─────────────── 工具 ───────────────
String jsonEsc(const String &s) {
  String o; o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if (c == '\n') o += "\\n";
    else if (c == '\r') o += "\\r";
    else if ((uint8_t)c < 0x20) {}
    else o += c;
  }
  return o;
}

uint32_t nowEpoch() {
  time_t t = time(nullptr);
  if (t > 1600000000UL) { timeReady = true; return (uint32_t)t; }
  // 未对时：用启动秒数占位，前端会按"启动后"显示
  return millis() / 1000;
}

uint32_t fmtUptime() { return (millis() - bootAt) / 1000; }

String urlEncode(const String &s) {
  String o; o.reserve(s.length() * 3);
  const char *hex = "0123456789ABCDEF";
  for (size_t i = 0; i < s.length(); i++) {
    unsigned char c = (unsigned char)s[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
    else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
  }
  return o;
}

// ─────────────── Server 酱推送（断网/恢复/测试） ───────────────
bool pushServerChan(const String &title, const String &desp) {
  if (cfg.sendkey.length() == 0) { Serial.println("[push] no sendkey, skip"); return false; }
  if (WiFi.status() != WL_CONNECTED) { Serial.println("[push] wifi down, skip"); return false; }
  String url = "https://sctapi.ftqq.com/" + cfg.sendkey + ".send";
  WiFiClientSecure client;
  client.setInsecure();                     // 跳过证书校验，省 Flash/RAM
  HTTPClient http;
  http.setTimeout(8000);
  if (!http.begin(client, url)) { Serial.println("[push] begin failed"); return false; }
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  String body = "title=" + urlEncode(title) + "&desp=" + urlEncode(desp);
  int code = http.POST(body);
  http.end();
  Serial.printf("[push] \"%s\" -> HTTP %d\n", title.c_str(), code);
  return (code == 200);
}

String pushTime() {
  time_t t = time(nullptr);
  if (t > 1600000000UL) {
    struct tm *tm = localtime(&t);
    char b[24];
    snprintf(b, sizeof(b), "%04d-%02d-%02d %02d:%02d:%02d",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);
    return String(b);
  }
  return String("uptime ") + fmtUptime() + "s";
}

bool checkAuth() {
  if (cfg.webpass.length() == 0) return true;
  // 用户名固定 admin，密码就是网页上填的「网页密码」。
  // realm 里直接写用户名，浏览器弹出的登录框自己会显示，省得再猜。
  if (server.authenticate("admin", cfg.webpass.c_str())) return true;
  server.requestAuthentication(BASIC_AUTH, "WatchDog (username: admin)");
  return false;
}

// ─────────────── 配置读写 ───────────────
void loadConfig() {
  if (!LittleFS.exists(CONFIG_PATH)) return;
  File f = LittleFS.open(CONFIG_PATH, "r");
  if (!f) return;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    int p = line.indexOf('=');
    if (p <= 0) continue;
    String k = line.substring(0, p);
    String v = line.substring(p + 1);
    if (k == "ssid")      cfg.ssid = v;
    else if (k == "pass")  cfg.pass = v;
    else if (k == "ssid2") cfg.ssid2 = v;
    else if (k == "pass2") cfg.pass2 = v;
    else if (k == "webpass") cfg.webpass = v;
    else if (k == "host")  cfg.host = v;
    else if (k == "dns")   cfg.dns = v;
    else if (k == "ip")    cfg.ip = v;
    else if (k == "gw")    cfg.gw = v;
    else if (k == "mask")  cfg.mask = v;
    else if (k == "sendkey") cfg.sendkey = v;
    else if (k == "port")  cfg.port = v.toInt();
    else if (k == "interval") cfg.interval = v.toInt();
    else if (k == "tz")    cfg.tz = v.toInt();
    else if (k == "daily") cfg.daily = (v.toInt() != 0) ? 1 : 0;
  }
  f.close();
  if (cfg.interval < 10)  cfg.interval = 10;
  if (cfg.interval > 3600) cfg.interval = 3600;
  if (cfg.port == 0) cfg.port = 80;
}

void saveConfig() {
  File f = LittleFS.open(CONFIG_PATH, "w");
  if (!f) return;
  f.println("ssid=" + cfg.ssid);
  f.println("pass=" + cfg.pass);
  f.println("ssid2=" + cfg.ssid2);
  f.println("pass2=" + cfg.pass2);
  f.println("webpass=" + cfg.webpass);
  f.println("host=" + cfg.host);
  f.println("dns=" + cfg.dns);
  f.println("ip=" + cfg.ip);
  f.println("gw=" + cfg.gw);
  f.println("mask=" + cfg.mask);
  f.println("sendkey=" + cfg.sendkey);
  f.println("port=" + String(cfg.port));
  f.println("interval=" + String(cfg.interval));
  f.println("tz=" + String(cfg.tz));
  f.println("daily=" + String(cfg.daily));
  f.close();
}

// ─────────────── 历史记录持久化 ───────────────
void historySave() {
  File f = LittleFS.open(HISTORY_PATH, "w");
  if (!f) return;
  f.write((const uint8_t*)"WDH1", 4);
  f.write((const uint8_t*)&ringHead, 2);
  f.write((const uint8_t*)&ringLen, 2);
  for (uint16_t i = 0; i < ringLen; i++) {
    uint16_t idx = (ringHead + RING_SIZE - ringLen + i) % RING_SIZE;
    f.write((const uint8_t*)&ring[idx], sizeof(Rec));
  }
  f.close();
}

void historyLoad() {
  if (!LittleFS.exists(HISTORY_PATH)) return;
  File f = LittleFS.open(HISTORY_PATH, "r");
  if (!f) return;
  char magic[4];
  if (f.read((uint8_t*)magic, 4) != 4 || memcmp(magic, "WDH1", 4) != 0) { f.close(); return; }
  f.read((uint8_t*)&ringHead, 2);
  f.read((uint8_t*)&ringLen, 2);
  if (ringLen > RING_SIZE || ringHead >= RING_SIZE) { ringHead = 0; ringLen = 0; f.close(); return; }
  for (uint16_t i = 0; i < ringLen; i++) {
    uint16_t idx = (ringHead + RING_SIZE - ringLen + i) % RING_SIZE;
    f.read((uint8_t*)&ring[idx], sizeof(Rec));
  }
  f.close();
}

Rec ringAt(uint16_t i) {               // i=0 最旧
  return ring[(ringHead + RING_SIZE - ringLen + i) % RING_SIZE];
}

// ─────────────── 断网事件落盘 ───────────────
// 只对整个文件 320 B 做一次覆盖写，且只在"恢复"那一刻写一次；
// 一天断网 20 次也只有 20 次块擦写，离 10 万次擦写寿命差着几个数量级。
void outagesSave() {
  File f = LittleFS.open(OUT_PATH, "w");
  if (!f) return;
  f.write((const uint8_t*)"WDO1", 4);
  f.write(&outHead, 1);
  f.write(&outLen, 1);
  for (uint8_t i = 0; i < outLen; i++) {
    uint8_t idx = (outHead + OUT_RING - outLen + i) % OUT_RING;
    f.write((const uint8_t*)&outRing[idx], sizeof(OutageRec));
  }
  f.close();
}

void outagesLoad() {
  if (!LittleFS.exists(OUT_PATH)) return;
  File f = LittleFS.open(OUT_PATH, "r");
  if (!f) return;
  char magic[4];
  if (f.read((uint8_t*)magic, 4) != 4 || memcmp(magic, "WDO1", 4) != 0) { f.close(); return; }
  f.read(&outHead, 1);
  f.read(&outLen, 1);
  if (outLen > OUT_RING || outHead >= OUT_RING) { outHead = 0; outLen = 0; f.close(); return; }
  for (uint8_t i = 0; i < outLen; i++) {
    uint8_t idx = (outHead + OUT_RING - outLen + i) % OUT_RING;
    f.read((uint8_t*)&outRing[idx], sizeof(OutageRec));
  }
  f.close();
  // 若最末一条还是"未结束"（dur==0），说明上次是在断网中途断电/重启的：
  // 继续把它当成进行中的断网，等网络恢复时再补上时长。
  if (outLen) {
    uint8_t last = (outHead + OUT_RING - 1) % OUT_RING;
    if (outRing[last].dur == 0) {
      inOutage = true;
      outageStartTs = outRing[last].ts;
      outageStartMs = millis();
      Serial.printf("[outage] resume open outage from ts=%lu\n", (unsigned long)outageStartTs);
    }
  }
}

// 断网开始：立刻写一条 dur=0 的"进行中"记录，这样中途断电也不会整条丢掉
void outageBegin(uint32_t ts, int8_t rssi) {
  outRing[outHead].ts   = ts;
  outRing[outHead].dur  = 0;
  outRing[outHead].rssi = (uint8_t)(rssi < -120 ? -120 : (rssi > 0 ? 0 : rssi));
  outHead = (outHead + 1) % OUT_RING;
  if (outLen < OUT_RING) outLen++;
  outagesSave();
}

// 断网结束：补时长（优先补"进行中"那条，没有才新增一条）
void outageEnd(uint16_t dur) {
  if (outLen) {
    uint8_t last = (outHead + OUT_RING - 1) % OUT_RING;
    if (outRing[last].dur == 0) {
      outRing[last].dur = dur;
      outagesSave();
      return;
    }
  }
  outageBegin(nowEpoch(), WiFi.RSSI());
  uint8_t last = (outHead + OUT_RING - 1) % OUT_RING;
  outRing[last].dur = dur;
  outagesSave();
}

// 每日简报的"上次推送日"（年*1000+yday），存 4 字节，重启不会重复推
uint32_t lastReportKey = 0;
void reportKeySave() {
  File f = LittleFS.open("/rep.day", "w");
  if (!f) return;
  f.write((const uint8_t*)&lastReportKey, 4);
  f.close();
}
void reportKeyLoad() {
  if (!LittleFS.exists("/rep.day")) return;
  File f = LittleFS.open("/rep.day", "r");
  if (!f) return;
  f.read((uint8_t*)&lastReportKey, 4);
  f.close();
}

void ringPush(uint32_t ts, uint16_t rtt, bool ok) {
  ring[ringHead].ts  = ts;
  ring[ringHead].rtt = rtt;
  ring[ringHead].ok  = ok ? 1 : 0;
  ringHead = (ringHead + 1) % RING_SIZE;
  if (ringLen < RING_SIZE) ringLen++;
}

// ─────────────── 网络探测 ───────────────
uint16_t tcpProbe(const String &host, uint16_t port, uint16_t timeoutMs) {
  if (host.length() == 0) return 0;
  IPAddress ip;
  if (!WiFi.hostByName(host.c_str(), ip, timeoutMs)) return 0;
  WiFiClient c;
  uint32_t t0 = millis();
  bool ok = c.connect(ip, port);
  uint16_t rtt = ok ? (uint16_t)(millis() - t0) : 0;
  c.stop();
  return rtt;
}

int encodeName(uint8_t *buf, const String &name) {
  int p = 0;
  int start = 0;
  for (int i = 0; i <= (int)name.length(); i++) {
    if (i == (int)name.length() || name[i] == '.') {
      int len = i - start;
      if (len <= 0 || len > 63) return -1;
      buf[p++] = (uint8_t)len;
      for (int j = 0; j < len; j++) buf[p++] = (uint8_t)name[start + j];
      start = i + 1;
    }
  }
  buf[p++] = 0;
  return p;
}

uint16_t dnsProbe(const String &server, const String &qname, uint16_t timeoutMs) {
  IPAddress ip;
  if (!ip.fromString(server)) return 0;
  WiFiUDP udp;
  if (!udp.begin(0)) return 0;

  uint8_t pkt[96];
  uint16_t id = (uint16_t)(millis() & 0xFFFF);
  pkt[0] = id >> 8; pkt[1] = id & 0xFF;
  pkt[2] = 0x01; pkt[3] = 0x00;          // 标准查询 + 递归
  pkt[4] = 0x00; pkt[5] = 0x01;          // QDCOUNT = 1
  for (int i = 6; i < 12; i++) pkt[i] = 0x00;
  int n = encodeName(pkt + 12, qname);
  if (n < 0) { udp.stop(); return 0; }
  int len = 12 + n;
  pkt[len++] = 0x00; pkt[len++] = 0x01;  // A
  pkt[len++] = 0x00; pkt[len++] = 0x01;  // IN

  udp.beginPacket(ip, 53);
  udp.write(pkt, len);
  udp.endPacket();

  uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    int sz = udp.parsePacket();
    if (sz >= 12) {
      uint8_t r[12];
      udp.read(r, 12);
      if (r[0] == pkt[0] && r[1] == pkt[1]) {
        uint16_t rtt = (uint16_t)(millis() - t0);
        udp.stop();
        return rtt;
      }
    }
    delay(2);
    yield();
  }
  udp.stop();
  return 0;
}

// ─────────────── 每日简报 ───────────────
// 用 ring 里的全部采样（最多 720 点）算一份摘要推给 Server 酱。
// 注意：ring 只有 720 点，interval=60 时约 12 小时，所以标题写"近 N 分钟"而不是"昨天"。
void sendDailyReport() {
  if (!cfg.sendkey.length() || ringLen < 2) return;

  uint16_t okCnt = 0, m = 0;
  uint32_t sum = 0;
  for (uint16_t i = 0; i < ringLen; i++) {
    Rec r = ringAt(i);
    if (r.ok && r.rtt) { okCnt++; sum += r.rtt; statBuf[m++] = r.rtt; }
  }
  uint16_t avg = m ? (uint16_t)(sum / m) : 0, p95 = 0, mx = 0;
  if (m) {
    qsort(statBuf, m, sizeof(uint16_t), cmpU16);
    uint16_t k = (uint16_t)((uint32_t)m * 95 / 100);
    if (k >= m) k = m - 1;
    p95 = statBuf[k];
    mx  = statBuf[m - 1];
  }

  uint16_t runs = 0, longest = 0, runLen = 0;
  for (uint16_t i = 0; i < ringLen; i++) {
    if (!ringAt(i).ok) {
      if (runLen == 0) runs++;
      runLen++;
      if (runLen > longest) longest = runLen;
    } else runLen = 0;
  }
  float pct = ringLen ? (okCnt * 100.0f / ringLen) : 100.0f;
  uint32_t windowMin = (uint32_t)ringLen * cfg.interval / 60UL;

  String title = "📊 WatchDog 日报";
  String desp  = "**" + cfg.ssid + "** 近 " + String(windowMin) + " 分钟\n\n";
  desp += "- 可用率：" + String(pct, 1) + "%\n";
  desp += "- 断网：" + String(runs) + " 次";
  if (longest) desp += "（最长约 " + String((uint32_t)longest * cfg.interval / 60UL) + " 分钟）";
  desp += "\n";
  desp += "- 平均延迟：" + String(avg) + "ms · p95 " + String(p95) + "ms · 最大 " + String(mx) + "ms\n";
  desp += "- 信号：" + String(WiFi.RSSI()) + " dBm\n";
  desp += "- 设备：" + WiFi.localIP().toString() + " · 已运行 " + fmtUptime();
  Serial.println("[report] daily summary pushed");
  pushServerChan(title, desp);
}

void doSample() {
  uint16_t rtt = dnsProbe(cfg.dns, cfg.host, 1200);
  if (rtt == 0) rtt = tcpProbe(cfg.host, cfg.port, 1500);   // DNS 不行就走 TCP

  bool ok = (rtt > 0);
  // 去抖：连续 2 次失败才判离线，避免偶发丢包画满断网段
  if (!ok && failRun < 250) failRun++;
  if (ok) failRun = 0;
  netOk = ok || (failRun < 2);

  rttNow = rtt;
  gwRtt  = tcpProbe(WiFi.gatewayIP().toString(), 80, 300);
  if (gwRtt == 0) gwRtt = tcpProbe(WiFi.gatewayIP().toString(), 53, 300);

  // 分层判定：网关也不通 = 局域网/路由器挂；网关通但外网不通 = 外网断
  if (netOk) lastProbeErr = 0;
  else       lastProbeErr = (gwRtt == 0) ? 2 : 1;

  ringPush(nowEpoch(), rtt, netOk);
  if (ringLen % 10 == 0) historySave();
  Serial.printf("[probe] ok=%d rtt=%u gw=%u heap=%u\n", netOk, rtt, gwRtt, ESP.getFreeHeap());

  // ── 断网事件：开始就落盘一条 dur=0 的记录，恢复时补时长（中途断电也不丢）──
  if (!netOk && !inOutage) {
    inOutage = true;
    outageStartTs = nowEpoch();
    outageStartMs = millis();
    outageBegin(outageStartTs, WiFi.RSSI());
    Serial.println("[outage] begin (saved as open)");
  } else if (netOk && inOutage) {
    inOutage = false;
    uint32_t dur;
    if (timeReady && outageStartTs > 100000000UL && nowEpoch() > outageStartTs)
      dur = nowEpoch() - outageStartTs;
    else
      dur = (millis() - outageStartMs) / 1000UL;
    if (dur > 65535UL) dur = 65535UL;
    outageEnd((uint16_t)dur);
    Serial.printf("[outage] %us end (total %u records)\n", (unsigned)dur, outLen);
  }

  // ── Server 酱推送：仅在状态翻转时触发（断网/恢复各推一次，避免刷屏）──
  static bool prevNetOk = true;
  if (!setupMode && cfg.sendkey.length() && netOk != prevNetOk) {
    if (!netOk) {
      if (lastProbeErr == 2) {
        pushServerChan("🔴 WatchDog 局域网断开",
          "**" + cfg.ssid + "** 网关和外网都不通（路由器/局域网问题）\n\n"
          "- 时间：" + pushTime() + "\n"
          "- SSID：" + cfg.ssid + "（" + String(WiFi.RSSI()) + " dBm）\n"
          "- 设备 IP：" + WiFi.localIP().toString() + "\n"
          "- 网关：" + WiFi.gatewayIP().toString() + "（无响应）\n\n"
          "先去看看路由器是不是重启/断电了。");
      } else {
        pushServerChan("🔴 WatchDog 外网中断",
          "**" + cfg.ssid + "** 网关还通，但外网探测失败\n\n"
          "- 时间：" + pushTime() + "\n"
          "- SSID：" + cfg.ssid + "（" + String(WiFi.RSSI()) + " dBm）\n"
          "- 设备 IP：" + WiFi.localIP().toString() + "\n"
          "- 探测目标：" + cfg.host + "（DNS " + cfg.dns + "）\n"
          "- 网关延迟：" + String(gwRtt) + "ms（正常）\n\n"
          "路由器在线，问题多半在运营商侧。");
      }
    } else {
      pushServerChan("✅ WatchDog 网络恢复",
        "**" + cfg.ssid + "** 已恢复联网\n\n"
        "- 时间：" + pushTime() + "\n"
        "- 当前延迟：" + String(rttNow) + "ms\n"
        "- 网关延迟：" + String(gwRtt) + "ms\n"
        "- 信号强度：" + String(WiFi.RSSI()) + " dBm\n"
        "- 本次断网：" + (outLen ? String(outRing[(outHead + OUT_RING - 1) % OUT_RING].dur) + " 秒" : "未知"));
    }
    prevNetOk = netOk;
  }
}

// ─────────────── 页面 ───────────────
// 直接用 streamFile 有两个坑：① 空闲一段时间后 ESP8266 的 WiFi 自动省电（modem sleep）
// 会让第一包卡 5-40 秒；② client.write() 超时后被截断的数据 streamFile 不会重发，
// 于是浏览器拿到的 index.html 只有几百/几千字节 → 白屏、转圈、"网页打不开"。
// 下面这版：关省电 + 512B 分块 + 短写重发 + 关连接前排空 TX 队列。
void sendFileSafe(const char *path, const char *type) {
  if (!LittleFS.exists(path)) { server.send(404, "text/plain", "not found"); return; }
  File f = LittleFS.open(path, "r");
  if (!f) { server.send(500, "text/plain", "fs error"); return; }

  size_t len = f.size();
  WiFiClient &cl = server.client();
  cl.setNoDelay(true);
  cl.setTimeout(8000);                       // 单次 write 的等待上限

  server.sendHeader("Cache-Control", "no-cache");
  server.setContentLength(len);
  server.send(200, type, "");

  uint8_t  buf[512];
  size_t   off = 0;
  uint32_t stallFrom = 0;
  while (off < len) {
    size_t n = f.read(buf, sizeof(buf));
    if (n == 0) break;
    size_t sent = 0;
    while (sent < n) {
      size_t w = cl.write(buf + sent, n - sent);
      if (w == 0) {
        if (stallFrom == 0) stallFrom = millis();
        if (millis() - stallFrom > 25000) break;   // 彻底写不动就放弃，别把整机卡死
        delay(2);
        yield();
        continue;
      }
      sent += w;
      stallFrom = 0;
      yield();
    }
    off += sent;
    if (sent < n) break;
  }
  cl.flush();                                // 关连接前把发送队列排空，否则尾包会丢
  f.close();
  if (off < len) Serial.printf("[web] %s short send %u/%u\n", path, (unsigned)off, (unsigned)len);
}

void serveFile(const char *path, const char *type) { sendFileSafe(path, type); }

void handleRoot()      { if (!checkAuth()) return; serveFile("/index.html", "text/html;charset=utf-8"); }
void handleFilesPage() { if (!checkAuth()) return; serveFile("/files.html", "text/html;charset=utf-8"); }
void handleSetupPage() { serveFile("/setup.html", "text/html;charset=utf-8"); }
void handleCss()       { serveFile("/ui.css", "text/css;charset=utf-8"); }
void handleJs()        { serveFile("/ui.js", "application/javascript;charset=utf-8"); }

// ─────────────── API：状态 ───────────────
void handleStatus() {
  if (!checkAuth()) return;
  uint16_t okCnt = 0, downRuns = 0; bool prevOk = true;
  for (uint16_t i = 0; i < ringLen; i++) {
    Rec r = ringAt(i);
    if (r.ok) okCnt++;
    else if (prevOk) { downRuns++; prevOk = false; }
    if (r.ok) prevOk = true;
  }

  // p50 / p95 / max / 抖动：只统计成功的采样
  uint16_t m = 0;
  for (uint16_t i = 0; i < ringLen; i++) {
    Rec r = ringAt(i);
    if (r.ok && r.rtt) statBuf[m++] = r.rtt;
  }
  uint16_t p50 = 0, p95 = 0, mx = 0, jitter = 0;
  if (m) {
    qsort(statBuf, m, sizeof(uint16_t), cmpU16);
    p50 = statBuf[m / 2];
    uint16_t k = (uint16_t)((uint32_t)m * 95 / 100);
    if (k >= m) k = m - 1;
    p95 = statBuf[k];
    mx  = statBuf[m - 1];
  }
  int32_t prevRtt = -1; uint32_t jsum = 0; uint16_t jn = 0;
  for (uint16_t i = 0; i < ringLen; i++) {
    Rec r = ringAt(i);
    if (!r.ok || !r.rtt) continue;
    if (prevRtt >= 0) { jsum += (uint32_t)abs((int)r.rtt - (int)prevRtt); jn++; }
    prevRtt = r.rtt;
  }
  if (jn) jitter = (uint16_t)(jsum / jn);

  String j = "{";
  j += "\"ver\":\"" FW_VERSION "\",";
  j += "\"mode\":\"" + String(setupMode ? "setup" : "sta") + "\",";
  j += "\"ssid\":\"" + jsonEsc(WiFi.SSID()) + "\",";
  j += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  j += "\"gw\":\"" + WiFi.gatewayIP().toString() + "\",";
  j += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  j += "\"ok\":" + String(netOk ? "true" : "false") + ",";
  j += "\"rtt\":" + String(rttNow) + ",";
  j += "\"gw_rtt\":" + String(gwRtt) + ",";
  j += "\"p50\":" + String(p50) + ",";
  j += "\"p95\":" + String(p95) + ",";
  j += "\"max_rtt\":" + String(mx) + ",";
  j += "\"jitter\":" + String(jitter) + ",";
  j += "\"err\":" + String((int)lastProbeErr) + ",";
  j += "\"drops\":" + String(wifiDrops) + ",";
  j += "\"samples\":" + String(ringLen) + ",";
  j += "\"up_pct\":" + String(ringLen ? (okCnt * 100.0 / ringLen) : 100.0, 1) + ",";
  j += "\"outages\":" + String(downRuns) + ",";
  j += "\"uptime\":" + String(fmtUptime()) + ",";
  j += "\"heap\":" + String(ESP.getFreeHeap()) + ",";
  j += "\"time_ok\":" + String(timeReady ? "true" : "false") + ",";
  j += "\"interval\":" + String(cfg.interval) + ",";
  j += "\"host\":\"" + jsonEsc(cfg.host) + "\",";
  j += "\"dns\":\"" + jsonEsc(cfg.dns) + "\",";
  j += "\"has_pass\":" + String(cfg.webpass.length() ? "true" : "false");
  j += "}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json;charset=utf-8", j);
}

// ─────────────── API：健康 / 复位原因 ───────────────
const char *resetReasonText(uint8_t r) {
  switch (r) {
    case REASON_DEFAULT_RST:      return "上电";
    case REASON_WDT_RST:          return "硬件看门狗";
    case REASON_EXCEPTION_RST:    return "异常复位";
    case REASON_SOFT_WDT_RST:     return "软件看门狗";
    case REASON_SOFT_RESTART:     return "软件重启";
    case REASON_DEEP_SLEEP_AWAKE: return "深睡唤醒";
    case REASON_EXT_SYS_RST:      return "外部复位";
    default:                      return "未知";
  }
}

void handleHealth() {
  if (!checkAuth()) return;
  rst_info *ri = ESP.getResetInfoPtr();
  uint32_t upSec = (millis() - bootAt) / 1000UL;
  String j = "{";
  j += "\"ver\":\"" FW_VERSION "\",";
  j += "\"reason\":" + String((int)ri->reason) + ",";
  j += "\"reason_txt\":\"" + String(resetReasonText(ri->reason)) + "\",";
  j += "\"exccause\":" + String((uint32_t)ri->exccause) + ",";
  j += "\"epc\":" + String((uint32_t)ri->epc1) + ",";
  j += "\"heap\":" + String(ESP.getFreeHeap()) + ",";
  j += "\"frag\":" + String(ESP.getHeapFragmentation()) + ",";
  j += "\"maxblock\":" + String(ESP.getMaxFreeBlockSize()) + ",";
  j += "\"uptime\":" + String(upSec) + ",";
  j += "\"mode\":\"" + String(setupMode ? "setup" : "sta") + "\",";
  j += "\"drops\":" + String(wifiDrops) + ",";
  j += "\"tries\":" + String(staTries) + ",";
  j += "\"last_err\":" + String((int)lastProbeErr) + ",";
  j += "\"ap\":\"" + jsonEsc(WiFi.SSID()) + "\",";
  j += "\"ap_idx\":" + String(activeAp) + ",";
  j += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  j += "\"samples\":" + String(ringLen) + ",";
  j += "\"outages\":" + String(outLen) + ",";
  j += "\"guard_kb\":" + String(HEAP_GUARD_MIN / 1024) + ",";
  j += "\"has_backup\":" + String(cfg.ssid2.length() ? "true" : "false");
  j += "}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json;charset=utf-8", j);
}

// GET /api/clear?what=outages|hist|all —— 清空记录（文件一起删，免得重启又读回来）
void handleClear() {
  if (!checkAuth()) return;
  String what = server.arg("what");
  bool did = false;
  if (what == "outages" || what == "all") {
    outLen = 0; outHead = 0; inOutage = false;
    LittleFS.remove(OUT_PATH);
    Serial.println("[clear] outages");
    did = true;
  }
  if (what == "hist" || what == "all") {
    ringLen = 0; ringHead = 0;
    LittleFS.remove(HISTORY_PATH);
    Serial.println("[clear] history");
    did = true;
  }
  if (what == "all") LittleFS.remove("/rep.day");
  server.send(200, "application/json", String("{\"ok\":") + (did ? "true" : "false") + "}");
}

void handleOutages() {
  if (!checkAuth()) return;
  String j;
  j.reserve(64 + outLen * 44);
  j = "{\"total\":" + String(outLen) + ",\"items\":[";
  for (uint8_t i = 0; i < outLen; i++) {
    uint8_t idx = (outHead + OUT_RING - outLen + i) % OUT_RING;   // 旧 → 新
    if (i) j += ",";
    j += "{\"ts\":" + String(outRing[idx].ts) +
         ",\"dur\":" + String(outRing[idx].dur) +
         ",\"rssi\":" + String((int)(int8_t)outRing[idx].rssi) + "}";
  }
  j += "]}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json;charset=utf-8", j);
}

// ─────────────── API：历史（流式 CSV） ───────────────
void handleHistory() {
  if (!checkAuth()) return;
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/csv;charset=utf-8", "");
  String buf; buf.reserve(256);
  for (uint16_t i = 0; i < ringLen; i++) {
    Rec r = ringAt(i);
    buf += String(r.ts) + "," + String(r.rtt) + "," + String(r.ok) + "\n";
    if (buf.length() > 200) { server.sendContent(buf); buf = ""; }
  }
  if (buf.length()) server.sendContent(buf);
  server.sendContent("");
}

// ─────────────── API：配置 ───────────────
void handleConfigGet() {
  if (!checkAuth()) return;
  String j = "{";
  j += "\"ssid\":\"" + jsonEsc(cfg.ssid) + "\",";
  j += "\"ssid2\":\"" + jsonEsc(cfg.ssid2) + "\",";
  j += "\"host\":\"" + jsonEsc(cfg.host) + "\",";
  j += "\"dns\":\"" + jsonEsc(cfg.dns) + "\",";
  j += "\"ip\":\"" + jsonEsc(cfg.ip) + "\",";
  j += "\"gw\":\"" + jsonEsc(cfg.gw) + "\",";
  j += "\"mask\":\"" + jsonEsc(cfg.mask) + "\",";
  j += "\"port\":" + String(cfg.port) + ",";
  j += "\"interval\":" + String(cfg.interval) + ",";
  j += "\"tz\":" + String(cfg.tz) + ",";
  j += "\"daily\":" + String(cfg.daily) + ",";
  j += "\"has_sendkey\":" + String(cfg.sendkey.length() ? "true" : "false") + ",";
  j += "\"has_pass2\":" + String(cfg.pass2.length() ? "true" : "false") + ",";
  j += "\"has_pass\":" + String(cfg.webpass.length() ? "true" : "false");
  j += "}";
  server.send(200, "application/json;charset=utf-8", j);
}

void handleConfigSet() {
  if (server.hasArg("ssid"))    cfg.ssid = server.arg("ssid");
  if (server.hasArg("ssid2"))   cfg.ssid2 = server.arg("ssid2");
  // ⚠ 密码类字段"留空 = 不修改"：网页出于安全不回显密码，提交上来的空串
  //   绝不等于"把密码清掉"。v1.3.0 之前这里是无条件赋值，结果改任何一项
  //   （比如只改 SendKey）都会顺手把 WiFi 密码清空 → 重启后连不上 → 网页打不开。
  if (server.hasArg("pass")    && server.arg("pass").length())    cfg.pass    = server.arg("pass");
  if (server.hasArg("pass2")   && server.arg("pass2").length())   cfg.pass2   = server.arg("pass2");
  if (server.hasArg("webpass") && server.arg("webpass").length()) cfg.webpass = server.arg("webpass");
  if (server.hasArg("sendkey") && server.arg("sendkey").length()) cfg.sendkey = server.arg("sendkey");
  // 想清空就走显式开关（网页上的"清除网页密码 / 关闭推送"勾选框）
  if (server.hasArg("webpass_clear")) cfg.webpass = "";
  if (server.hasArg("sendkey_clear")) cfg.sendkey = "";
  if (server.hasArg("host") && server.arg("host").length()) cfg.host = server.arg("host");
  if (server.hasArg("dns") && server.arg("dns").length())   cfg.dns  = server.arg("dns");
  if (server.hasArg("ip"))      cfg.ip   = server.arg("ip");
  if (server.hasArg("gw"))      cfg.gw   = server.arg("gw");
  if (server.hasArg("mask"))    cfg.mask = server.arg("mask");
  if (server.hasArg("port"))    cfg.port = server.arg("port").toInt();
  if (server.hasArg("interval")) cfg.interval = server.arg("interval").toInt();
  if (server.hasArg("tz"))      cfg.tz = server.arg("tz").toInt();
  if (server.hasArg("daily"))   cfg.daily = (server.arg("daily") == "0" || server.arg("daily") == "off") ? 0 : 1;
  if (cfg.port == 0) cfg.port = 80;
  if (cfg.interval < 10) cfg.interval = 10;
  if (cfg.interval > 3600) cfg.interval = 3600;
  saveConfig();
  server.send(200, "application/json", "{\"ok\":true,\"reboot\":true}");
  delay(600);
  ESP.restart();
}

// ─────────────── API：文件 ───────────────
String safeName(const String &raw) {
  String n = raw;
  n.replace("\\", "/");
  while (n.startsWith("/")) n.remove(0, 1);
  n.replace("..", "");
  return n;
}

void handleFileList() {
  if (!checkAuth()) return;
  FSInfo fi;
  LittleFS.info(fi);
  String j = "{\"files\":[";
  Dir dir = LittleFS.openDir(UPLOAD_DIR);
  bool first = true;
  while (dir.next()) {
    if (dir.isDirectory()) continue;
    if (!first) j += ",";
    first = false;
    j += "{\"n\":\"" + jsonEsc(dir.fileName()) + "\",\"s\":" + String(dir.fileSize()) + "}";
  }
  j += "],\"used\":" + String(fi.usedBytes) +
       ",\"total\":" + String(fi.totalBytes) + "}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json;charset=utf-8", j);
}

File uploadFile;

void handleUploadDone() {
  if (!checkAuth()) return;
  if (server.upload().status == UPLOAD_FILE_END || uploadFile) { uploadFile.close(); }
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleUpload() {
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    String name = safeName(up.filename);
    if (!name.length()) name = "unnamed.bin";
    if (!LittleFS.exists(UPLOAD_DIR)) LittleFS.mkdir(UPLOAD_DIR);
    uploadFile = LittleFS.open(String(UPLOAD_DIR) + "/" + name, "w");
    Serial.printf("[upload] %s\n", name.c_str());
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) uploadFile.write(up.buf, up.currentSize);
  } else if (up.status == UPLOAD_FILE_END) {
    if (uploadFile) { uploadFile.close(); Serial.printf("[upload] done %u\n", up.totalSize); }
  }
}

void handleDownload() {
  if (!checkAuth()) return;
  String name = safeName(server.arg("n"));
  String path = String(UPLOAD_DIR) + "/" + name;
  if (!LittleFS.exists(path)) { server.send(404, "text/plain", "not found"); return; }
  File f = LittleFS.open(path, "r");
  server.sendHeader("Content-Disposition", "attachment; filename=\"" + name + "\"");
  server.streamFile(f, "application/octet-stream");
  f.close();
}

void handleDelete() {
  if (!checkAuth()) return;
  String name = safeName(server.arg("n"));
  String path = String(UPLOAD_DIR) + "/" + name;
  bool ok = LittleFS.exists(path) && LittleFS.remove(path);
  server.send(200, "application/json", String("{\"ok\":") + (ok ? "true" : "false") + "}");
}

// ─────────────── API：扫描 / 重启 / OTA ───────────────
void handleScan() {
  int n = WiFi.scanNetworks();
  String j = "{\"nets\":[";
  bool first = true;
  for (int i = 0; i < n && i < 20; i++) {
    if (!first) j += ",";
    first = false;
    j += "{\"s\":\"" + jsonEsc(WiFi.SSID(i)) + "\",\"r\":" + String(WiFi.RSSI(i)) +
         ",\"e\":" + String(WiFi.encryptionType(i)) + "}";
  }
  j += "]}";
  WiFi.scanDelete();
  server.send(200, "application/json;charset=utf-8", j);
}

void handleReboot() {
  if (!checkAuth()) return;
  server.send(200, "application/json", "{\"ok\":true}");
  delay(500);
  ESP.restart();
}

// 手动触发一次 Server 酱推送，用于验证 SendKey 是否有效
void handleTestPush() {
  if (!checkAuth()) return;
  String desp = "如果你收到这条消息，说明 Server 酱配置正常。\n\n"
                "- 设备：" + WiFi.localIP().toString() + "（" + WiFi.SSID() + "）\n"
                "- 时间：" + pushTime();
  bool ok = pushServerChan("✅ WatchDog 测试推送", desp);
  server.send(200, "application/json", String("{\"ok\":") + (ok ? "true" : "false") + "}");
}

void handleOtaInfo() {
  server.send(200, "application/json", "{\"size\":65536,\"free\":" + String(ESP.getFreeSketchSpace()) + "}");
}

void handleOtaDone() {
  if (Update.hasError()) server.send(500, "text/plain", "ota failed");
  else { server.send(200, "text/plain", "ok, rebooting"); delay(500); ESP.restart(); }
}

void handleOtaUpload() {
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    uint32_t maxSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
    if (!Update.begin(maxSpace)) Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (Update.write(up.buf, up.currentSize) != up.currentSize) Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_END) {
    if (!Update.end(true)) Update.printError(Serial);
    else Serial.println("[ota] success");
  }
}

// ─────────────── 启动辅助 ───────────────
void startSetupAP() {
  setupMode = true;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(DEFAULT_AP);
  delay(200);
  Serial.printf("[setup] AP %s  http://%s\n", DEFAULT_AP, WiFi.softAPIP().toString().c_str());
  dnsServer.start(53, "*", WiFi.softAPIP());
}

// 连上路由器之后要做的收尾（正常开机 / 配网模式后补连上都走这里）
void onStaConnected() {
  setupMode = false;
  // 关掉 WiFi 省电：开着的时候空闲后的第一个请求会卡 5-40 秒，表现就是"网页打不开/一直转圈"
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  Serial.printf("[sta] connected '%s' (candidate %u) ip=%s rssi=%d dBm ch=%d\n",
                WiFi.SSID().c_str(), activeAp + 1, WiFi.localIP().toString().c_str(),
                WiFi.RSSI(), WiFi.channel());
  if (WiFi.RSSI() < -80)
    Serial.println("[sta] warning: signal is weak (< -80 dBm), web may drop");
  MDNS.begin("watchdog");
  MDNS.addService("http", "tcp", 80);
  configTime(cfg.tz * 3600, 0, "ntp.aliyun.com", "ntp1.aliyun.com", "pool.ntp.org");
}

// 在 budgetMs 内反复尝试关联；弱信号（-78 dBm 左右）时首次关联可能要 20-40 秒，
// 一次 begin 等不到就要重发，否则会误判成"连不上"而退回配网模式（网页打不开）。
// ch/bssid 来自开机扫描：按 BSSID+信道直连比盲连快，也更不容易被同信道干扰带偏。
bool trySta(const String &ssid, const String &pass, uint32_t budgetMs, int32_t ch, const uint8_t *bssid) {
  uint32_t t0 = millis(), lastTry = 0;
  uint8_t  tries = 0;
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < budgetMs) {
    if (tries == 0 || millis() - lastTry >= 10000) {
      tries++;
      staTries++;
      applyStaticIp();                 // 每次 begin 前重设：模式切换/断开会把静态配置清掉
      if (tries <= 3 && ch > 0 && bssid)
        WiFi.begin(ssid.c_str(), pass.c_str(), (uint8_t)ch, bssid);
      else
        WiFi.begin(ssid.c_str(), pass.c_str());   // 直连不成就退回普通关联
      lastTry = millis();
      Serial.printf("[sta] try %u '%s' ch=%d status=%d\n", tries, ssid.c_str(), (int)ch, (int)WiFi.status());
    }
    delay(200);
    yield();
  }
  return WiFi.status() == WL_CONNECTED;
}

// 固定 IP：cfg.ip 非空就用静态地址，空则照旧走 DHCP。
// 必须在 WiFi.begin() 之前调用，否则 SDK 已经按 DHCP 开工了。
void applyStaticIp() {
  if (!cfg.ip.length()) return;
  IPAddress ip, gw, mask, dns;
  if (!ip.fromString(cfg.ip)) { Serial.println("[net] 静态 IP 格式不对，退回 DHCP"); return; }
  if (!gw.fromString(cfg.gw.length() ? cfg.gw : "0.0.0.0")) return;
  if (!mask.fromString(cfg.mask.length() ? cfg.mask : "255.255.255.0")) return;
  if (!dns.fromString(cfg.dns.length() ? cfg.dns : "223.5.5.5")) dns = IPAddress(223, 5, 5, 5);
  WiFi.config(ip, gw, mask, dns);
  Serial.printf("[net] static ip=%s gw=%s mask=%s dns=%s\n",
                cfg.ip.c_str(), cfg.gw.c_str(), cfg.mask.c_str(), cfg.dns.c_str());
}

void startSta() {
  WiFi.persistent(false);          // 不把临时连接记录写进 SDK 区，避免下次开机被旧记录带偏
  WiFi.mode(WIFI_STA);
  WiFi.hostname("watchdog");
  WiFi.setOutputPower(20.5);       // 发射功率拉满（信号只有 -78 dBm，能多 1dB 是 1dB）
  // 注意：这里不能设固定 IP —— 下面 disconnect(true)+mode() 会把配置清掉，
  // 反而留下一个"localIP 是静态地址、实际没网"的假象。统一放到 trySta() 里 begin 之前设。
  // 冷启动直接 begin 有时会一直停在 DISCONNECTED（实测能卡满 45 秒、一次都连不上），
  // 所以先断开再扫一遍：既预热射频，又拿到目标 AP 的信道/BSSID。
  ApTarget t[2];
  t[0].ssid = cfg.ssid;   t[0].pass = cfg.pass;
  t[1].ssid = cfg.ssid2;  t[1].pass = cfg.pass2;
  uint8_t cnt = t[1].ssid.length() ? 2 : 1;
  // 一次扫描同时找主/备 SSID 的最强 AP（同一趟扫描，不额外花时间）
  WiFi.disconnect(true);           // 清掉 SDK 里可能残留的旧关联
  delay(200);
  WiFi.mode(WIFI_STA);             // disconnect(true) 会关射频，扫描前必须重新打开
  delay(200);
  int n = WiFi.scanNetworks(false, true);
  Serial.printf("[sta] scan -> %d ap%s\n", n, n <= 0 ? "（扫描失败，下面直接盲连）" : "");
  for (uint8_t k = 0; k < cnt; k++) {
    if (!t[k].ssid.length()) continue;
    for (int i = 0; i < n; i++) {
      if (WiFi.SSID(i) == t[k].ssid && WiFi.RSSI(i) > t[k].rssi) {
        t[k].rssi = WiFi.RSSI(i);
        t[k].ch   = WiFi.channel(i);
        memcpy(t[k].bssid, WiFi.BSSID(i), 6);
      }
    }
  }
  WiFi.scanDelete();
  delay(100);

  for (uint8_t k = 0; k < cnt; k++) {
    if (!t[k].ssid.length()) continue;
    Serial.printf("[sta] candidate %u/%u '%s' (ch=%d rssi=%d %s)\n", k + 1, cnt,
                  t[k].ssid.c_str(), (int)t[k].ch, (int)t[k].rssi,
                  t[k].rssi == -32768 ? "扫描未发现" : "已发现");
    if (trySta(t[k].ssid, t[k].pass, cnt > 1 ? 22000 : 45000,
               t[k].ch, t[k].rssi == -32768 ? NULL : t[k].bssid)) {
      activeAp = k;
      onStaConnected();
      return;
    }
    Serial.printf("[sta] '%s' 没连上 (status=%d)\n", t[k].ssid.c_str(), (int)WiFi.status());
    WiFi.disconnect(true);
    delay(200);
  }
  Serial.println("[sta] all candidates failed, fallback to setup AP");
  startSetupAP();
}

// ─────────────── LED ───────────────
void ledTask() {
  static uint32_t t = 0;
  static uint8_t  phase = 0;
  uint32_t now = millis();
  if (setupMode) {                       // 配网模式：慢闪
    if (now - t > 700) { t = now; phase ^= 1; digitalWrite(PIN_LED, phase ? LED_ON : LED_OFF); }
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {   // WiFi 掉线：快闪
    if (now - t > 120) { t = now; phase ^= 1; digitalWrite(PIN_LED, phase ? LED_ON : LED_OFF); }
    return;
  }
  if (!netOk) {                          // 断网：双闪
    if (now - t > 180) {
      t = now;
      phase = (phase + 1) % 6;
      digitalWrite(PIN_LED, (phase == 0 || phase == 2) ? LED_ON : LED_OFF);
    }
    return;
  }
  digitalWrite(PIN_LED, LED_ON);         // 正常：常亮
}

// ─────────────── setup / loop ───────────────
void registerRoutes() {
  server.on("/", handleRoot);
  server.on("/index.html", handleRoot);
  server.on("/files", handleFilesPage);
  server.on("/setup", handleSetupPage);
  server.on("/ui.css", handleCss);
  server.on("/ui.js", handleJs);

  server.on("/api/status", handleStatus);
  server.on("/api/health", handleHealth);
  server.on("/api/outages", handleOutages);
  server.on("/api/clear", handleClear);
  server.on("/api/history", handleHistory);
  server.on("/api/config", HTTP_GET, handleConfigGet);
  server.on("/api/config", HTTP_POST, handleConfigSet);
  server.on("/api/scan", handleScan);
  server.on("/api/reboot", handleReboot);
  server.on("/api/testpush", HTTP_POST, handleTestPush);

  server.on("/api/files", handleFileList);
  server.on("/api/upload", HTTP_POST, handleUploadDone, handleUpload);
  server.on("/dl", handleDownload);
  server.on("/api/delete", handleDelete);

  server.on("/api/ota", HTTP_POST, handleOtaDone, handleOtaUpload);

  server.onNotFound([]() {
    if (setupMode) serveFile("/setup.html", "text/html;charset=utf-8");
    else serveFile("/index.html", "text/html;charset=utf-8");
  });
}

void setup() {
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LED_OFF);
  Serial.begin(115200);
  Serial.println("\n\n=== WatchDog v" FW_VERSION " ===");
  bootAt = millis();

  if (!LittleFS.begin()) {
    Serial.println("[fs] mount failed, formatting...");
    LittleFS.format();
    LittleFS.begin();
  }
  loadConfig();
  historyLoad();
  outagesLoad();
  reportKeyLoad();
  if (!LittleFS.exists(UPLOAD_DIR)) LittleFS.mkdir(UPLOAD_DIR);

  if (cfg.ssid.length() == 0) startSetupAP();
  else startSta();

  registerRoutes();
  server.begin();
  Serial.println("[web] started");
  delay(200);
  doSample();                      // 开机立刻测一次
  lastSampleAt = millis();
}

void loop() {
  server.handleClient();
  if (setupMode) dnsServer.processNextRequest();
  else MDNS.update();

  uint32_t now = millis();
  if (now - lastSampleAt >= (uint32_t)cfg.interval * 1000UL) {
    lastSampleAt = now;
    doSample();
  }

  // 掉线计数：连着→断了才算一次（快速闪断也算，方便判断是不是信号问题）
  static bool wasLinked = false;
  bool linked = (WiFi.status() == WL_CONNECTED);
  if (wasLinked && !linked) wifiDrops++;
  wasLinked = linked;

  // WiFi 掉线重连（每 10 秒试一次）
  static uint32_t retryAt = 0;
  if (!setupMode && !linked && now - retryAt > 10000) {
    retryAt = now;
    Serial.println("[sta] reconnect...");
    WiFi.reconnect();
  }

  // 停在配网模式时也别放弃：每分钟轮换主/备 WiFi 再试，连上了就自动退出 AP
  static uint32_t apRetryAt = 0;
  static uint8_t  apRetryIdx = 0;
  if (setupMode && now - apRetryAt > 60000) {
    apRetryAt = now;
    bool useBackup = (apRetryIdx++ % 2) && cfg.ssid2.length();
    const String &s = useBackup ? cfg.ssid2 : cfg.ssid;
    if (s.length()) {
      Serial.printf("[sta] retry from AP mode: %s\n", s.c_str());
      activeAp = useBackup ? 1 : 0;
      applyStaticIp();
      WiFi.begin(s.c_str(), useBackup ? cfg.pass2.c_str() : cfg.pass.c_str());
    }
  }
  if (setupMode && linked) {
    dnsServer.stop();
    WiFi.softAPdisconnect(true);
    Serial.println("[setup] joined home WiFi, leaving AP mode");
    onStaConnected();
  }

  // heap 看护：可用堆长期见底就主动重启，比某天自己崩在半路上强
  static uint32_t lowHeapSince = 0;
  if (!setupMode && ESP.getFreeHeap() < HEAP_GUARD_MIN) {
    if (!lowHeapSince) lowHeapSince = now ? now : 1;
    else if (now - lowHeapSince > HEAP_GUARD_MS) {
      Serial.printf("[guard] heap %u < %u for 5min, restarting\n", ESP.getFreeHeap(), HEAP_GUARD_MIN);
      delay(200);
      ESP.restart();
    }
  } else {
    lowHeapSince = 0;
  }

  // 每日简报：08:00 之后第一次进 loop 时推一次（推过就记 /rep.day，重启不重推）
  if (cfg.daily && cfg.sendkey.length() && !setupMode && timeReady) {
    static uint32_t lastCheck = 0;
    if (now - lastCheck > 30000UL) {
      lastCheck = now;
      time_t tt = time(nullptr);
      struct tm tmNow;
      localtime_r(&tt, &tmNow);
      uint32_t key = (uint32_t)(tmNow.tm_year + 1900) * 1000UL + (uint32_t)tmNow.tm_yday;
      if (tmNow.tm_hour >= 8 && key != lastReportKey) {
        lastReportKey = key;
        reportKeySave();
        sendDailyReport();
      }
    }
  }

  ledTask();
  yield();
}
