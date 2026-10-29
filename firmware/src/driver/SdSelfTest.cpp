#include "SdSelfTest.h"

#include <SD.h>
#include <WiFi.h>

#include "GifPlayer.h"
#include "../share/SDUtil.h"

namespace {

constexpr size_t   CHUNK      = 2048;   //跟 HTTP 上传缓冲同粒度, 别改(见 .h 里的理由)
constexpr uint32_t FNV1A_INIT = 2166136261u;
const char* const  TEST_DIR   = "/app/AiStackChanEx";
const char* const  TEST_NAME  = "/app/AiStackChanEx/sdtest.bin";
const char* const  TEST_TEMP  = "/app/AiStackChanEx/sdtest.bin.tmp";

uint32_t fnv1a(uint32_t h, const uint8_t* p, size_t n)
{
  while (n--) { h ^= *p++; h *= 16777619u; }
  return h;
}

// 内容必须是**每块都不一样**的: 全零或常量填充时, "写错一整块"这种故障
// 会跟正确结果长得一样, 校验和抓不出来 —— 那正是 07-27 要抓的那类错误。
void fillChunk(uint8_t* buf, size_t n, uint32_t seed)
{
  uint32_t x = seed * 2654435761u + 1u;
  for (size_t i = 0; i < n; i++) {
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;   //xorshift32
    buf[i] = (uint8_t)(x & 0xff);
  }
}

struct Tally {
  int ok = 0;
  int writeFail = 0;      //write() 返回值短了 —— 老老实实报错的那类
  int verifyFail = 0;     //写满了但读回来内容不对 —— 静默损坏, 危险的那类
  int openFail = 0;
  int renameFail = 0;
};

}  // namespace

void sd_self_test_run(const SdSelfTestOpts& opts)
{
  const bool gifWasOn = gif_theater_on();
  const bool wifiWasUp = (WiFi.getMode() != WIFI_OFF);

  Serial.printf("\n[SDTest] 开始: rounds=%d size=%u chunk=%u wifi=%s gif=%s rename=%s\n",
                opts.rounds, (unsigned)opts.fileBytes, (unsigned)CHUNK,
                opts.wifiOff ? "off" : "on",
                opts.gifOff  ? "off" : "on",
                opts.noRename ? "skip" : "on");

  if (opts.gifOff && gifWasOn) {
    //⚠️ 停剧场**不等于**让出 SPI: 07-26 实测停掉会 avatar.resumeDraw() 把持续重绘
    // 唤回来, 屏幕反而更占总线(见 WebAPI.cpp 里那段注释)。这里保留这个开关是为了
    // **量**它到底哪边好, 不是因为认定停掉更好。
    gif_theater_end();
    Serial.println("[SDTest] 已停 GIF 剧场(注意: avatar 会恢复重绘, 屏幕更占总线)");
  }
  if (opts.wifiOff && wifiWasUp) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(200);
    Serial.println("[SDTest] 已关 WiFi");
  }

  uint8_t* buf = (uint8_t*)malloc(CHUNK);
  if (!buf) { Serial.println("[SDTest] ❌ 分配缓冲失败"); return; }

  if (!SD.exists(TEST_DIR)) SD.mkdir(TEST_DIR);

  Tally t;
  const uint32_t t0 = millis();

  for (int r = 1; r <= opts.rounds; r++) {
    SD.remove(TEST_TEMP);
    const char* target = opts.noRename ? TEST_NAME : TEST_TEMP;

    File f = SD.open(target, FILE_WRITE);
    if (!f) {
      t.openFail++;
      Serial.printf("[SDTest] %3d/%d  ❌ open 失败\n", r, opts.rounds);
      continue;
    }

    uint32_t want = FNV1A_INIT;
    size_t   written = 0;
    bool     shortWrite = false;
    for (size_t off = 0; off < opts.fileBytes && !shortWrite; off += CHUNK) {
      const size_t n = min(CHUNK, opts.fileBytes - off);
      fillChunk(buf, n, (uint32_t)(r * 100000u + off));
      const size_t w = f.write(buf, n);
      want = fnv1a(want, buf, w);
      written += w;
      if (w != n) shortWrite = true;
    }
    f.close();

    if (shortWrite) {
      t.writeFail++;
      Serial.printf("[SDTest] %3d/%d  ❌ 写入断在 %u / %u 字节 (= %u x %u)\n",
                    r, opts.rounds, (unsigned)written, (unsigned)opts.fileBytes,
                    (unsigned)(written / CHUNK), (unsigned)CHUNK);
      SD.remove(target);
      continue;
    }

    //读回校验 —— 抓"写满了但内容错"的那类, 07-27 实测约 11.5%
    uint32_t got = FNV1A_INIT;
    File b = SD.open(target, FILE_READ);
    bool readOk = (bool)b;
    if (readOk) {
      if (b.size() != written) {
        readOk = false;
        Serial.printf("[SDTest] %3d/%d  ❌ 读回大小不符 %u != %u\n",
                      r, opts.rounds, (unsigned)b.size(), (unsigned)written);
      } else {
        while (int n = b.read(buf, CHUNK)) {
          if (n < 0) { readOk = false; break; }
          got = fnv1a(got, buf, (size_t)n);
        }
      }
      b.close();
    }

    if (!readOk || got != want) {
      t.verifyFail++;
      Serial.printf("[SDTest] %3d/%d  ❌ **静默损坏** 写满 %u 字节但校验不符 %08x != %08x\n",
                    r, opts.rounds, (unsigned)written, (unsigned)got, (unsigned)want);
      SD.remove(target);
      continue;
    }

    if (!opts.noRename) {
      SD.remove(TEST_NAME);
      if (!SD.rename(TEST_TEMP, TEST_NAME)) {
        t.renameFail++;
        Serial.printf("[SDTest] %3d/%d  ❌ rename 失败(写和校验都过了)\n", r, opts.rounds);
        SD.remove(TEST_TEMP);
        continue;
      }
    }

    t.ok++;
    Serial.printf("[SDTest] %3d/%d  ✅ %u 字节\n", r, opts.rounds, (unsigned)written);
  }

  const uint32_t dt = millis() - t0;
  free(buf);
  SD.remove(TEST_NAME);
  SD.remove(TEST_TEMP);

  const int fail = t.writeFail + t.verifyFail + t.openFail + t.renameFail;
  Serial.printf("\n[SDTest] ===== 共 %d 次, 成功 %d, 失败 %d (%.1f%%) =====\n",
                opts.rounds, t.ok, fail, opts.rounds ? 100.0 * fail / opts.rounds : 0.0);
  Serial.printf("[SDTest]   写入断裂 %d / **静默损坏 %d** / open 失败 %d / rename 失败 %d\n",
                t.writeFail, t.verifyFail, t.openFail, t.renameFail);
  Serial.printf("[SDTest]   耗时 %lu ms, 写入总量 %u KB\n",
                (unsigned long)dt, (unsigned)(opts.rounds * opts.fileBytes / 1024));
  Serial.println("[SDTest]   对照: 07-27 走 HTTP 上传口 36 次失败 36.1%(重插卡后)/55.6%(重插前)");
  Serial.println("[SDTest]   本测试**不碰网络**: 失败率接近 -> 问题在 SD/SPI/供电;"
                 " 明显更低 -> 问题在跟 WiFi 抢资源\n");

  if (opts.wifiOff && wifiWasUp) {
    Serial.println("[SDTest] ⚠️ WiFi 仍是关的, 要恢复请复位设备(本测试不自己重连, "
                   "免得把重连过程混进下一轮测量)");
  }
  if (opts.gifOff && gifWasOn) {
    gif_theater_begin();
    Serial.println("[SDTest] 已恢复 GIF 剧场");
  }
}

namespace {

// 只写不校验的快速探测: 返回失败个数。用来判"当前是好模式还是坏模式"。
int probeFailures(int n, size_t bytes)
{
  uint8_t* buf = (uint8_t*)malloc(CHUNK);
  if (!buf) return n;
  int fail = 0;
  for (int r = 0; r < n; r++) {
    SD.remove(TEST_TEMP);
    File f = SD.open(TEST_TEMP, FILE_WRITE);
    if (!f) { fail++; continue; }
    uint32_t want = FNV1A_INIT;
    size_t written = 0; bool shortWrite = false;
    for (size_t off = 0; off < bytes && !shortWrite; off += CHUNK) {
      const size_t k = min(CHUNK, bytes - off);
      fillChunk(buf, k, (uint32_t)(r * 7919u + off));
      const size_t w = f.write(buf, k);
      want = fnv1a(want, buf, w);
      written += w;
      if (w != k) shortWrite = true;
    }
    f.close();
    if (shortWrite) { fail++; SD.remove(TEST_TEMP); continue; }
    //静默损坏也算失败 —— 那才是真正危险的那类
    uint32_t got = FNV1A_INIT;
    File b = SD.open(TEST_TEMP, FILE_READ);
    bool ok = (bool)b && b.size() == written;
    if (ok) { while (int k = b.read(buf, CHUNK)) { if (k < 0) { ok = false; break; } got = fnv1a(got, buf, (size_t)k); } }
    if (b) b.close();
    if (!ok || got != want) fail++;
    SD.remove(TEST_TEMP);
  }
  free(buf);
  return fail;
}

}  // namespace

void sd_self_test_reroll(const SdRerollOpts& opts)
{
  Serial.printf("\n[SDReroll] 开始: 最多 %d 次, 每次探测 %d 个文件, 重挂时钟 %.1f MHz\n",
                opts.tries, opts.probe, opts.clockHz / 1e6);

  const bool gifWasOn = gif_theater_on();
  const size_t bytes = 48u * 1024u;

  for (int i = 1; i <= opts.tries; i++) {
    const int fail = probeFailures(opts.probe, bytes);
    Serial.printf("[SDReroll] 第%d次探测: %d/%d 失败%s\n",
                  i, fail, opts.probe, fail <= 1 ? "  ← ✅ 好模式" : "");
    if (fail <= 1) {
      Serial.printf("[SDReroll] ===== 摇到好模式, 用了 %d 次重挂 =====\n", i - 1);
      return;
    }
    if (i == opts.tries) break;

    //🔴 重挂前必须先停 GIF 剧场: 它在另一个 FreeRTOS task 里读 SD,
    //   SD.end() 撞上正在进行的读会崩。停完再重开。
    if (gifWasOn) gif_theater_end();
    SD.end();
    delay(120);
    const bool ok = sd_begin_cores3(opts.clockHz);   //跟开机走同一条挂载路径
    Serial.printf("[SDReroll]   重挂 %s\n", ok ? "成功" : "❌ 失败");
    if (gifWasOn) gif_theater_begin();
    if (!ok) { Serial.println("[SDReroll] 挂不上了, 停"); return; }
  }
  Serial.printf("[SDReroll] ===== %d 次重挂都没摇出好模式 =====\n", opts.tries);
  Serial.println("[SDReroll]   -> 模式不由挂载决定, 该往供电/温度查, 别在挂载上耗了");
}

bool sd_self_test_handle_command(const String& line)
{
  String s = line;
  s.trim();

  //★ OWNER 2026-07-28: A/B 那一行 SPI.begin() 的 arm 开关。写 NVS, 复位后生效。
  //   `sdspi`(不带参数) 只报当前值, 不改。
  if (s.startsWith("sdspi")) {
    const String arg = s.substring(5);
    if (arg.indexOf("on") >= 0)       sd_spi_begin_set(true);
    else if (arg.indexOf("off") >= 0) sd_spi_begin_set(false);
    else Serial.printf("[SDcfg] 当前(本次开机生效中的) arm=%s\n",
                       sd_spi_begin_enabled() ? "on" : "off");
    return true;
  }

  if (s.startsWith("sdreroll")) {
    SdRerollOpts o;
    int from = 8;
    while (from < (int)s.length()) {
      int sp = s.indexOf(' ', from);
      if (sp < 0) sp = s.length();
      const String tok = s.substring(from, sp);
      from = sp + 1;
      if (tok.length() == 0) continue;
      if      (tok.startsWith("tries=")) o.tries = tok.substring(6).toInt();
      else if (tok.startsWith("probe=")) o.probe = tok.substring(6).toInt();
      else if (tok.startsWith("clk="))   o.clockHz = (uint32_t)(tok.substring(4).toFloat() * 1e6);
      else Serial.printf("[SDReroll] 不认得的参数: %s\n", tok.c_str());
    }
    if (o.tries <= 0) o.tries = 8;
    if (o.probe <= 0) o.probe = 6;
    if (o.clockHz == 0) o.clockHz = 10000000u;
    sd_self_test_reroll(o);
    return true;
  }

  if (!s.startsWith("sdtest")) return false;

  SdSelfTestOpts o;
  int from = 6;
  while (from < (int)s.length()) {
    int sp = s.indexOf(' ', from);
    if (sp < 0) sp = s.length();
    const String tok = s.substring(from, sp);
    from = sp + 1;
    if (tok.length() == 0) continue;

    if      (tok.startsWith("rounds=")) o.rounds    = tok.substring(7).toInt();
    else if (tok.startsWith("kb="))     o.fileBytes = (size_t)tok.substring(3).toInt() * 1024u;
    else if (tok == "wifi=off")         o.wifiOff   = true;
    else if (tok == "gif=off")          o.gifOff    = true;
    else if (tok == "norename")         o.noRename  = true;
    else Serial.printf("[SDTest] 不认得的参数: %s\n", tok.c_str());
  }
  if (o.rounds <= 0)    o.rounds = 18;
  if (o.fileBytes == 0) o.fileBytes = 48u * 1024u;

  sd_self_test_run(o);
  return true;
}
