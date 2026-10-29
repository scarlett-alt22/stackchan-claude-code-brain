/*
  AudioFileSourceHttpsPostStream
  Streaming HTTP source

  Copyright (C) 2017  Earle F. Philhower, III

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#if defined(ESP32) || defined(ESP8266)

#include "AudioFileSourceHttpsPostStream.h"
//#include "rootCACertificate.h"

// TTS の合成は生成側(クラウド or ローカルの音声合成サーバ)で時間がかかる。
// HTTPClient の既定タイムアウトは 5 秒しかなく、Mac 上の Chatterbox のように
// 1 文あたり 10〜17 秒かかるサーバでは POST が必ずタイムアウトし、症状は
// 「デバイスが黙るだけ」で原因が非常に追いにくい。LLM 側(ChatGPT.cpp)が
// setTimeout(65000) を張っているのと同じ理由で、ここでも明示的に延ばす。
static const uint16_t POST_TIMEOUT_MS = 65000;

// ★ OWNER 2026-07-25: 「只听到一个字」的直接机制就在下面 readInternal 里。
// 旧逻辑 = 「500ms 内没凑够 len 字节 -> 当成 EOF -> http.end()」。这条判据是错的:
//   ① 断了不可逆: reconnectTries 恒为 0, 重连循环一次都不跑;
//   ② 上层 AudioGeneratorWAV::GetBufferedData 只要拿到 0 字节就 stop(), 播放永久结束;
//   ③ 500ms 在这条链路上远远不够 —— Qwen3 RTF≈2.7, 服务端每块要 2.5~5.5s 才合成出来,
//      块间静默是常态; 且本机实测设备 RTT 均值 594ms/峰值 1808ms, 光网络抖动就能空过 500ms。
// 正确判据是「对端关没关」, 不是「等了多久」: 慢只是慢, 只有收到 FIN 才是流结束。
static const uint32_t STALL_TIMEOUT_MS = 15000;  // 对端还开着时最长等多久(远大于最坏一块 5.5s)

// ★ OWNER 2026-07-25 埋点: 分辨"一个字一个字/丢音"到底是网络饿死还是设备本地抢资源。
// 判据: 播完之后如果 stalls≈0 && bytes≈期望字节数, 说明数据一个不少、也没等过网络,
// 那卡顿就只能是本地(I2S/ConsumeSample 被 GIF 解码等抢走 CPU/总线) —— 跟流式链路无关。
volatile uint32_t g_httpsStreamBytes    = 0;   // 本次流累计读到的字节
volatile uint32_t g_httpsStreamStalls   = 0;   // 阻塞读时 socket 是空的次数
volatile uint32_t g_httpsStreamStallMs  = 0;   // 累计因为等数据而阻塞的毫秒
volatile uint32_t g_httpsStreamMaxStall = 0;   // 单次最长阻塞毫秒

AudioFileSourceHttpsPostStream::AudioFileSourceHttpsPostStream()
{
  pos = 0;
  reconnectTries = 0;
  saveURL[0] = 0;
  client = nullptr;
  isHttps = false;
  rootCACertificate = nullptr;
}

AudioFileSourceHttpsPostStream::AudioFileSourceHttpsPostStream(const char *url, const char* root_ca)
{
  saveURL[0] = 0;
  reconnectTries = 0;
  rootCACertificate = root_ca;
  client = nullptr;
  isHttps = false;
  //open(url);
}

void AudioFileSourceHttpsPostStream::releaseClient()
{
  if (!client) return;
  // WiFiClient のデストラクタは非仮想なので、実体の型で delete する
  // (そうしないと ~WiFiClientSecure() が走らず TLS の資源が解放されない)。
  if (isHttps) {
    delete static_cast<WiFiClientSecure*>(client);
  } else {
    delete client;
  }
  client = nullptr;
}

void AudioFileSourceHttpsPostStream::addHeader(const String& name, const String& value)
{
  http.addHeader(name, value);
}

void AudioFileSourceHttpsPostStream::addData(String json)
{
  jsonData = json;
}

bool AudioFileSourceHttpsPostStream::open(const char *url)
{
  pos = 0;
  g_httpsStreamBytes = g_httpsStreamStalls = g_httpsStreamStallMs = g_httpsStreamMaxStall = 0;

  // 再接続(readInternal からの open) でも取りこぼさないよう、まず前回の実体を捨てる。
  releaseClient();

  isHttps = (strncmp(url, "https://", 8) == 0);
  if (isHttps) {
    if (rootCACertificate == nullptr) {
      cb.st(STATUS_HTTPFAIL, PSTR("https:// with no root CA"));
      Serial.println("Refusing request: https:// with no root CA");
      return false;
    }
    WiFiClientSecure *secure_client = new WiFiClientSecure;
    if (secure_client) {
      secure_client->setCACert(rootCACertificate);
    }
    client = secure_client;
  } else {
    client = new WiFiClient;
  }
  if (!client) {
    cb.st(STATUS_HTTPFAIL, PSTR("Unable to create client"));
    Serial.println("Unable to create client");
    return false;
  }

  http.begin(*client, url);
  http.setTimeout(POST_TIMEOUT_MS);
  http.setReuse(true);
#ifndef ESP32
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
#endif
  //int code = http.GET();
  int code = http.POST(jsonData);
  if (code != HTTP_CODE_OK) {
    http.end();
    cb.st(STATUS_HTTPFAIL, PSTR("Can't open HTTP request"));
    Serial.printf("Can't open HTTP request. code = %d\n", code);
    return false;
  }
  size = http.getSize();
  strncpy(saveURL, url, sizeof(saveURL));
  saveURL[sizeof(saveURL)-1] = 0;
  return true;
}

AudioFileSourceHttpsPostStream::~AudioFileSourceHttpsPostStream()
{
  // HTTPClient を先に畳んでから実体を解放する(逆順だと使用中の client を消すことになる)。
  http.end();
  releaseClient();
}

uint32_t AudioFileSourceHttpsPostStream::read(void *data, uint32_t len)
{
  if (data==NULL) {
    audioLogger->printf_P(PSTR("ERROR! AudioFileSourceHttpsPostStream::read passed NULL data\n"));
    return 0;
  }
  return readInternal(data, len, false);
}

uint32_t AudioFileSourceHttpsPostStream::readNonBlock(void *data, uint32_t len)
{
  if (data==NULL) {
    audioLogger->printf_P(PSTR("ERROR! AudioFileSourceHttpsPostStream::readNonBlock passed NULL data\n"));
    return 0;
  }
  return readInternal(data, len, true);
//  return readInternal(data, len, false);
}

uint32_t AudioFileSourceHttpsPostStream::readInternal(void *data, uint32_t len, bool nonBlock)
{
retry:
  if (!http.connected()) {
    cb.st(STATUS_DISCONNECTED, PSTR("Stream disconnected"));
    http.end();
    for (int i = 0; i < reconnectTries; i++) {
      char buff[64];
      sprintf_P(buff, PSTR("Attempting to reconnect, try %d"), i);
      cb.st(STATUS_RECONNECTING, buff);
      delay(reconnectDelayMs);
      if (open(saveURL)) {
        cb.st(STATUS_RECONNECTED, PSTR("Stream reconnected"));
        break;
      }
    }
    if (!http.connected()) {
      cb.st(STATUS_DISCONNECTED, PSTR("Unable to reconnect"));
      return 0;
    }
  }
  if ((size > 0) && (pos >= size)) return 0;

  WiFiClient *stream = http.getStreamPtr();

  // Can't read past EOF...
  if ( (size > 0) && (len > (uint32_t)(pos - size)) ) len = pos - size;

  if (!nonBlock) {
    // ★ OWNER 2026-07-25: 见文件顶部注释。等待条件从「凑够 len / 500ms」
    // 改成「有没有数据 && 对端还开着」。
    uint32_t start = millis();
    bool wasEmpty = !stream->available();
    // ① 只要对端还开着, 就一直等真数据 —— 合成慢/网络抖都不是流结束
    while (!stream->available() && stream->connected() &&
           (millis() - start < STALL_TIMEOUT_MS)) {
      delay(2);   // delay 会让出 CPU 给 lwIP; 项目看门狗 180s, 15s 阻塞有充足余量
    }
    if (wasEmpty) {
      uint32_t waited = millis() - start;
      g_httpsStreamStalls++;
      g_httpsStreamStallMs += waited;
      if (waited > g_httpsStreamMaxStall) g_httpsStreamMaxStall = waited;
    }
  }
  // ⚠️ 别在这儿加"凑满 len 再返回"的 gather 等待: 上层 AudioFileSourceBuffer 传进来的
  // len 就是整个 96KB 环缓冲, 而一句话总共往往还不到 96KB —— 凑批条件永远不成立,
  // 每次读都白等满一个 gather 周期。实测加了 120ms gather 后, 3.5s 的音频要播 13.06s
  // (203 次 loop 每次多等 ~47ms)。有多少拿多少即可, 批量累积交给 fill() 的
  // readNonBlock 机会主义地做。

  size_t avail = stream->available();
  if (!nonBlock && !avail) {
    // 走到这里 = 对端真的 FIN 了(或彻底哑了 15s) -> 这才是真 EOF, 可以收摊
    cb.st(STATUS_NODATA, PSTR("No stream data available"));
    Serial.printf("[audio] stream EOF: connected=%d waited>=%ums\n",
                  (int)stream->connected(), (unsigned)STALL_TIMEOUT_MS);
    http.end();
    //goto retry;   //ElevenLabsだと無限に繰り返してしまうためreturnに変更
    return 0;
  }
  if (avail == 0) return 0;
  if (avail < len) len = avail;

  int read = stream->read(reinterpret_cast<uint8_t*>(data), len);
  pos += read;
  if (read > 0) g_httpsStreamBytes += read;
  return read;
}

bool AudioFileSourceHttpsPostStream::seek(int32_t pos, int dir)
{
  audioLogger->printf_P(PSTR("ERROR! AudioFileSourceHttpsPostStream::seek not implemented!"));
  (void) pos;
  (void) dir;
  return false;
}

bool AudioFileSourceHttpsPostStream::close()
{
  http.end();
  return true;
}

bool AudioFileSourceHttpsPostStream::isOpen()
{
  return http.connected();
}

uint32_t AudioFileSourceHttpsPostStream::getSize()
{
  return size;
}

uint32_t AudioFileSourceHttpsPostStream::getPos()
{
  return pos;
}

#endif
