#include <Arduino.h>
#include <M5Unified.h>
#include <SD.h>
#include <SPIFFS.h>
#include <AudioOutput.h>
#include <AudioFileSourceBuffer.h>
#include <AudioGeneratorMP3.h>
#include "AudioFileSourceHTTPSStream.h"
#include "AudioFileSourceSD.h"
#include "AudioFileSourceSPIFFS.h"
#include "AudioFileSourceFS.h"
#include "AudioOutputM5Speaker.h"
#include "PlayMP3.h"
#include "Avatar.h"
#include <AudioFileSourcePROGMEM.h>
#include "OfflineVoice.h"   //★ 2026-10-28: flash 内嵌离线语音(tools/wav_to_header.py 生成)

using namespace m5avatar;

extern Avatar avatar;
extern bool servo_home;

// ★ OWNER 2026-07-25: AudioFileSourceHttpsPostStream.cpp 里的流式埋点计数器
extern volatile uint32_t g_httpsStreamBytes;
extern volatile uint32_t g_httpsStreamStalls;
extern volatile uint32_t g_httpsStreamStallMs;
extern volatile uint32_t g_httpsStreamMaxStall;

/// set M5Speaker virtual channel (0-7)
//static constexpr uint8_t m5spk_virtual_channel = 0;
uint8_t m5spk_virtual_channel = 0;

AudioOutputM5Speaker out(&M5.Speaker, m5spk_virtual_channel);
AudioGeneratorMP3 *mp3;
AudioGeneratorWAV *wav;

// ★ OWNER 2026-07-24: 30KB -> 96KB。
// 30KB @ 48KB/s(24kHz/mono/16bit) 只有 **0.64 秒** 余量。网络一抖就欠载，而
// AudioFileSourceHttpsPostStream 阻塞读 500ms 之后就 http.end() **永久**断开 ——
// 声音戛然而止，Mac 侧同时报 BrokenPipe。07-24 实测设备侧 ping 20~40% 丢包，
// 这个余量根本不够。96KB ≈ 2 秒，给丢包留出恢复窗口。
// 注意这是**容错**不是提速：它不会让合成变快，只让已经开口的话不容易被掐断。
int preallocateBufferSize = 96*1024;
uint8_t *preallocateBuffer;




void mp3_init(void)
{
    mp3 = new AudioGeneratorMP3();
    wav = new AudioGeneratorWAV();
    //out = new AudioOutputM5Speaker(&M5.Speaker, m5spk_virtual_channel);

    //TTS MP3用バッファ （PSRAMから確保される）
    // ★ OWNER 2026-07-24: 原来写的是裸 malloc + 注释说「PSRAMから確保される」——
    // 那依赖 CONFIG_SPIRAM_USE_MALLOC 的隐式行为，不保证。扩到 96KB 后如果落在内部
    // RAM 上会挤掉 WiFi/音频的堆，所以显式指定 PSRAM，失败再退回内部 RAM。
    // mp3_init() 是在 setup() 里调用的（不是全局对象构造），此刻 PSRAM 已初始化 ——
    // 不会踩 07-24 SubWindow 那个「全局构造函数里分配 PSRAM 拿到空指针」的坑。
    preallocateBuffer = (uint8_t *)heap_caps_malloc(preallocateBufferSize, MALLOC_CAP_SPIRAM);
    if (!preallocateBuffer) {
        Serial.println("[audio] PSRAM alloc failed, falling back to internal RAM");
        preallocateBuffer = (uint8_t *)malloc(preallocateBufferSize);
    }
    if (!preallocateBuffer) {
        M5.Display.printf("FATAL ERROR:  Unable to preallocate %d bytes for app\n", preallocateBufferSize);
        for (;;) { delay(1000); }
    }
    Serial.printf("[audio] stream buffer %d KB @ %s\n", preallocateBufferSize / 1024,
                  esp_ptr_external_ram(preallocateBuffer) ? "PSRAM" : "internal RAM");

    audioLogger = &Serial;
}

// ★ OWNER 2026-07-25: 「一个字一个字说话/音被截断/凑不成完整句子」的真凶。
// 生成器(wav->loop() 返回 false)只代表**解码**到了流末尾, 不代表**喇叭**已经放完 ——
// AudioOutputM5Speaker 是把样本推进 M5.Speaker 的 DMA/混音队列, 队列里还压着东西。
// 原来紧接着就 M5.Speaker.end(), 而 end() 会直接丢弃队列里没出去的样本。
//
// 为什么偏偏短句难听、长句没事(实测,不是推测):
//   长句 3.83s 音频 -> 生成器被数据到达速度限流, 到 EOF 时队列几乎是空的, 只丢 13ms;
//   短句整句一次到齐 -> 生成器一路飙到 EOF, 队列压到最满, 丢 65~119ms,
//   而 65~119ms 正好是最后一个音节 —— 听感就是"音被截断、凑不成一个完整句子"。
// 判据: 串口那行 elapsed 明显小于 bytes 换算出来的 audio 秒数, 就是这里在吃尾巴。
static void drainSpeaker(void)
{
  uint32_t t0 = millis();
  while (M5.Speaker.isPlaying() && (millis() - t0 < 3000)) {
    delay(1);   // 让出 CPU; 3 秒上限纯粹是防呆, 正常几十毫秒就排空
  }
}

// ★ OWNER 2026-10-04: 预填抽成公用函数(原来内联在 playWAV 里,逻辑一字未改),
// playMP3 的网络流也要用。原理见下方 playWAV 前的长注释。
// read(&scratch, 0) 是零消费的"推一下": 首次调用会走 AudioFileSourceBuffer 的
// 阻塞首读(等第一批数据), 之后每次内部都会调 fill() 机会主义地收走已到达的字节,
// 而 len=0 保证一个字节都不会被消费掉。
static void prefillBuffer(AudioFileSourceBuffer *buff, uint32_t target, uint32_t timeoutMs)
{
  uint8_t scratch = 0;
  uint32_t prefillStart = millis();
  buff->read(&scratch, 0);
  while (buff->getFillLevel() < target &&
         buff->isOpen() &&                                  // 流已 EOF 就别再等了
         (millis() - prefillStart) < timeoutMs) {
    buff->read(&scratch, 0);
    delay(2);
  }
  Serial.printf("[audio] prefill %lu B in %lums (target=%lu, open=%d)\n",
                (unsigned long)buff->getFillLevel(),
                (unsigned long)(millis() - prefillStart),
                (unsigned long)target, (int)buff->isOpen());
}

void playMP3(AudioFileSourceBuffer *buff, uint32_t prefillBytes){

  M5.Mic.end();
  M5.Speaker.begin();

  // ★ OWNER 2026-10-04(旅行): 网络 TTS 改走 MiniMax 原生 mp3(64kbps=8KB/s)后,
  // 绕海外 VPS 的链路下主人听着"有点卡"。playWAV 有预填、playMP3 原来没有——收到一点就开播,
  // 网络一停顿就断。只给网络流预填(OpenAITTS 传 MP3_STREAM_PREFILL_BYTES);
  // 本地 SD/SPIFFS 文件传 0 = 原行为——本地文件比目标小时 isOpen 一直为真,会白等到超时。
  if (prefillBytes > 0) {
    prefillBuffer(buff, prefillBytes, MP3_PREFILL_TIMEOUT_MS);
  }

  mp3->begin(buff, &out);
  Serial.println("mp3 start");

  while(mp3->isRunning()) {
    if (!mp3->loop()) {
      mp3->stop();
      Serial.println("mp3 stop");
    }
    delay(1);
  }

  drainSpeaker();   // ★ OWNER 2026-07-25: 见 drainSpeaker() 注释, 不排空直接 end() 会吃掉尾音
  M5.Speaker.end();
  M5.Mic.begin();

}

// ★ OWNER 2026-07-25: 起播前先把环缓冲喂到一定水位, 再让喇叭开口。
//
// 为什么需要它 —— 07-25 串口埋点实测(一句 1.37s 的回复):
//   bytes=65804(一字节不差) / stalls=13 / stallMs=1045 / maxStall=104ms / underruns=5/9
//   同一轮 Mac 侧 t_first_audio == t_done == 6.64 且 silence_filled=0.0,
//   也就是**合成在播放开始前就全做完了、数据早就备齐**。
//   所以断料跟合成、跟 Mac 的起播调度都无关 —— 是设备取不动。
//
// 算一下就知道卡在哪: 65760 字节 / 13 次阻塞 = 每次 5058 字节, 这几乎就是 lwIP 默认
// 接收窗口(TCP_WND 5744)。非阻塞时间只有 161ms, 读本身跑到 408KB/s —— 不是带宽不够,
// 是**每收满一个窗口就要等一次 ~80ms 往返**。有效上限 ~70KB/s, 而播放要吃 48KB/s,
// 余量只有 14%, 且单次空档 104ms > 输出块 85ms -> 必然反复见底。
//
// 治法: 开口前先攒 512ms 音频当缓冲垫(≈5 倍于实测最坏空档 104ms)。之后播放边放边收,
// 平均进比出快(~55KB/s vs 48KB/s), 垫子只会变厚不会变薄。
//
// 🔴 别改成在 readInternal 里"凑满 len 再返回" —— 那条路被实测否过, 见
//    AudioFileSourceHttpsPostStream.cpp 顶部: 加 120ms gather 后 3.5s 音频要播 13.06s。
//    预填只做一次, 不进每次读的热路径, 这是两回事。
// ⚠️ 代价诚实记账: 起播晚 ~350~450ms(按实测 55~70KB/s 取 24KB)。拿首字延迟换不卡。
static const uint32_t PREFILL_BYTES      = 24 * 1024;  // 512ms @ 48KB/s
static const uint32_t PREFILL_TIMEOUT_MS = 1200;       // 攒不够也不能无限等

void playWAV(AudioFileSourceBuffer *buff){

  M5.Mic.end();
  M5.Speaker.begin();

  // ---- 预填: 先攒够垫子再开口(实现见 prefillBuffer) ----
  prefillBuffer(buff, PREFILL_BYTES, PREFILL_TIMEOUT_MS);

  wav->begin(buff, &out);
  //OWNER 2026-07-24: 排查"只听到一个字就没声了"——量实际播放耗时和loop()跑了几圈,
  //跟Mac那边算出的应有时长(t8125日志的d_est)对比,看是不是提前断流/播放循环提前退出。
  uint32_t startMs = millis();
  uint32_t loopCount = 0;
  Serial.println("wav start");

  while(wav->isRunning()) {
    loopCount++;
    if (!wav->loop()) {
      wav->stop();
      // ★ OWNER 2026-07-25: 分辨"一个字一个字/丢音"的判据全在这一行里。
      //   audio  = 实际收到的音频秒数(字节/48000), 拿去跟 Mac 端日志的 headstart/d_est 对
      //   elapsed= 实际播放墙钟。elapsed >> audio 就是播放侧被拖慢;
      //   stalls = 因为 socket 空而阻塞的次数, stallMs = 累计等待。
      //   stalls≈0 而 elapsed 仍 >> audio  -> 本地抢资源(GIF 解码/I2S), 跟网络无关;
      //   stalls 大 -> 真是网络/合成供不上。
      uint32_t el = millis() - startMs;
      float audioSec = g_httpsStreamBytes / 48000.0f;
      // underrun = 喇叭真的断过料的次数, 每次 ≈ 一段可听见的空档(chunk 毫秒数见 chunkMs)。
      // 这是"一个字一个字/很多音被截断"的直接度量: 要听起来干净, 它必须是 0。
      Serial.printf("wav stop, elapsed=%lums, loopCount=%lu, bytes=%lu (audio=%.2fs), "
                    "stalls=%lu, stallMs=%lu, maxStall=%lums, "
                    "underruns=%u/%u (chunk=%lums)\n",
                    el, loopCount, (unsigned long)g_httpsStreamBytes, audioSec,
                    (unsigned long)g_httpsStreamStalls, (unsigned long)g_httpsStreamStallMs,
                    (unsigned long)g_httpsStreamMaxStall,
                    (unsigned)out.getUnderruns(), (unsigned)out.getFlushes(),
                    (unsigned long)out.getChunkMs());
    }
    delay(1);
  }

  // ★ 兜底排空(真正的修复在 AudioOutputM5Speaker::stop() 里, 见那边注释:
  // 队列是被 M5.Speaker::stop() 清掉的, 发生在 generator 的 loop() 内部, 比这里早)。
  // 这里保留只是防止别的路径把音频排进队列后没人等它放完; 正常应恒为个位数毫秒。
  uint32_t beforeDrain = millis();
  drainSpeaker();
  Serial.printf("wav drained, tail=%lums\n", millis() - beforeDrain);

  M5.Speaker.end();
  M5.Mic.begin();

}

bool playMP3SPIFFS(const char *filename)
{
  bool result;

  if (SPIFFS.exists(filename)) {
    AudioFileSourceSPIFFS *file_mp3 = new AudioFileSourceSPIFFS(filename);
    Serial.println("Open mp3");
    
    if( !file_mp3->isOpen() ){
      delete file_mp3;
      file_mp3 = nullptr;
      Serial.println("failed to open mp3 file");
      result = false;
    }
    else{
      AudioFileSourceBuffer *buff = new AudioFileSourceBuffer(file_mp3, preallocateBuffer, preallocateBufferSize);
      avatar.setExpression(Expression::Happy);
      servo_home = false;

      playMP3(buff);
      
      avatar.setExpression(Expression::Neutral);
      servo_home = true;

      delete file_mp3;
      delete buff;
      result = true;
    }
  }else{
    Serial.println("mp3 file is not exist");
    result = false;
  }
  return result;
}


bool playMP3SD(const char *filename)
{
  bool result;

  if (SD.exists(filename)) {

    AudioFileSourceSD *file_mp3 = new AudioFileSourceSD(filename);
    Serial.println("Open mp3");
    
    if( !file_mp3->isOpen() ){
      delete file_mp3;
      //file_mp3 = nullptr;
      Serial.println("failed to open mp3 file");
      result = false;
    }
    else{
      AudioFileSourceBuffer *buff = new AudioFileSourceBuffer(file_mp3, preallocateBuffer, preallocateBufferSize);
      avatar.setExpression(Expression::Happy);
      servo_home = false;

      playMP3(buff);
      
      avatar.setExpression(Expression::Neutral);
      servo_home = true;

      delete file_mp3;
      delete buff;
      result = true;
    }
  }else{
    Serial.println("mp3 file is not exist");
    result = false;
  }

  return result;
}


// ★ OWNER 2026-07-26: SD 上的预合成 WAV 直接播(触摸反馈用)。
// 跟 playWAV() 的区别都是有意的:
//   - 不预填。预填是为了扛设备侧 TCP 接收窗口的往返(07-25 那次 stalls=13/underruns=5),
//     SD 是本地读、几百 KB/s 起步,加预填只会白等。
//   - 不打 g_httpsStream* 那套埋点。那些计数器归 HTTP 流所有,在这里读到的是上一次
//     网络播放的残值,写进日志只会误导下一次排查。
//   - 不碰 avatar 表情、不碰 servo_home。触摸反馈自己安排表情和舵机顺序。
bool playWAVFS(fs::FS &fs, const char *filename)
{
  //★ OWNER 2026-07-26: SD 抽风时 exists() 会瞬时假阴性(实测 shake/02 第一次
  //  报 not exist、隔十几秒同一个文件就正常播了)。重试一次代价极小,
  //  换掉的是"摸了没声音、还查不出为什么"。(SPIFFS 不抽风, 重试对它是空转)
  if (!fs.exists(filename)) {
    delay(30);
    if (!fs.exists(filename)) {
      Serial.printf("[VoiceClip] wav not exist: %s\n", filename);
      return false;
    }
    Serial.printf("[VoiceClip] exists() 首次假阴性, 重试命中: %s\n", filename);
  }

  AudioFileSourceFS *src = new AudioFileSourceFS(fs, filename);
  if (!src->isOpen()) {
    delete src;
    Serial.printf("[VoiceClip] failed to open: %s\n", filename);
    return false;
  }

  AudioFileSourceBuffer *sdbuf =
      new AudioFileSourceBuffer(src, preallocateBuffer, preallocateBufferSize);

  M5.Mic.end();
  M5.Speaker.begin();

  wav->begin(sdbuf, &out);
  const uint32_t startMs = millis();
  while (wav->isRunning()) {
    if (!wav->loop()) {
      wav->stop();
    }
    delay(1);
  }
  drainSpeaker();

  M5.Speaker.end();
  M5.Mic.begin();

  Serial.printf("[VoiceClip] played %s in %lums (underruns=%u/%u)\n",
                filename, (unsigned long)(millis() - startMs),
                (unsigned)out.getUnderruns(), (unsigned)out.getFlushes());

  delete sdbuf;
  delete src;
  return true;
}

bool playWAVSD(const char *filename)
{
  return playWAVFS(SD, filename);
}

// ★ 2026-10-28: 离线语音播放。Mac 不可达时机器人用自己的声音说明原因。
// 音源是 flash 内嵌的完整 WAV(OfflineVoice.h,由 tools/wav_to_header.py 生成),
// 不用网络、不用 SD/SPIFFS —— 卡平时不插,分区布局也不动。
// 播放路径与 playWAVFS 完全一致(全局 wav 生成器 + out + drainSpeaker),
// 区别只有数据源:那边是 AudioFileSourceFS,这边是 AudioFileSourcePROGMEM。
// AudioGeneratorWAV 需要带 RIFF 头的完整 WAV 字节流,这就是头文件存整段
// WAV 而不是裸 PCM 的原因。
// 限频:最多每 30 秒一次,STT 侧和 LLM 侧的 NETWORK 共用这一个计数 ——
// 断网时一轮对话最多出声一次,不会连着喊。
bool playOfflineVoice()
{
  static bool playedOnce = false;
  static uint32_t lastPlayedMs = 0;
  const uint32_t now = millis();
  if (playedOnce && (now - lastPlayedMs) < 30000) {
    Serial.printf("[OfflineVoice] skipped: rate limited (%lums since last)\n",
                  (unsigned long)(now - lastPlayedMs));
    return false;
  }

  AudioFileSourcePROGMEM *src =
      new AudioFileSourcePROGMEM(OFFLINE_VOICE_WAV, OFFLINE_VOICE_WAV_LEN);
  AudioFileSourceBuffer *buf =
      new AudioFileSourceBuffer(src, preallocateBuffer, preallocateBufferSize);

  M5.Mic.end();
  M5.Speaker.begin();

  wav->begin(buf, &out);
  const uint32_t startMs = millis();
  while (wav->isRunning()) {
    if (!wav->loop()) {
      wav->stop();
    }
    delay(1);
  }
  drainSpeaker();

  M5.Speaker.end();
  M5.Mic.begin();

  delete buf;
  delete src;

  playedOnce = true;
  lastPlayedMs = millis();
  Serial.printf("[OfflineVoice] played embedded wav (%lu bytes) in %lums\n",
                (unsigned long)OFFLINE_VOICE_WAV_LEN,
                (unsigned long)(millis() - startMs));
  return true;
}
