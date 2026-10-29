#ifndef _PLAY_MP3_H
#define _PLAY_MP3_H

#include <Arduino.h>
#include <FS.h>
#include <M5Unified.h>
#include <AudioFileSourceBuffer.h>
#include <AudioGeneratorMP3.h>
#include <AudioGeneratorWAV.h>
#include "AudioFileSourceHTTPSStream.h"
#include "AudioOutputM5Speaker.h"

extern uint8_t m5spk_virtual_channel;

extern AudioOutputM5Speaker out;
//extern AudioOutputM5Speaker *out;
extern AudioGeneratorMP3 *mp3;
extern AudioGeneratorWAV *wav;
extern AudioFileSourceBuffer *buff;
extern AudioFileSourceHTTPSStream *file;
extern int preallocateBufferSize;
extern uint8_t *preallocateBuffer;

extern void mp3_init(void);
// prefillBytes: 开播前先攒多少字节(0 = 不预填, 本地文件用)。网络 TTS 传 MP3_STREAM_PREFILL_BYTES。
extern void playMP3(AudioFileSourceBuffer *buff, uint32_t prefillBytes = 0);
// ★ OWNER 2026-10-04: 8KB ≈ 1s @ 64kbps(MiniMax mp3 默认码率)。码率改 32k 则≈2s。
static const uint32_t MP3_STREAM_PREFILL_BYTES = 8 * 1024;
static const uint32_t MP3_PREFILL_TIMEOUT_MS   = 1500;
extern bool playMP3SPIFFS(const char *filename);
extern bool playMP3SD(const char *filename);

// ★ OWNER 2026-07-12: Mac 上の Claude 音色 server は WAV しか返さない
// (MP3 エンコードは ffmpeg 依存で、その ffmpeg が入っていないため)。
// playMP3() と同じ流儀の再生ループ。
extern void playWAV(AudioFileSourceBuffer *buff);

// ★ OWNER 2026-07-26: 本地文件系统上的预合成 WAV 直接播。
// playWAV() 那条是给 HTTP 流用的(带预填/断料埋点/g_httpsStream* 计数器),本地是就地读、
// 不存在 TCP 窗口往返,那套仪表全不适用,所以另起一个干净的循环,不要复用。
// 也不带 playMP3SD() 里那两个副作用(setExpression(Happy) / servo_home=false)——
// 触摸反馈自己决定表情和舵机。
// ★ OWNER 2026-07-28: 泛型化成 fs::FS——触摸语音镜像进 SPIFFS 之后,
// 同一条播放路径要能吃 SPIFFS 和 SD 两种来源(SpiffsMirror.h 那套的一部分)。
extern bool playWAVFS(fs::FS &fs, const char *filename);
extern bool playWAVSD(const char *filename);   // = playWAVFS(SD, ...)

// ★ 2026-10-28: flash 内嵌的离线语音(driver/OfflineVoice.h)用与 TTS 相同的
// AudioGeneratorWAV + AudioOutputM5Speaker 路径播放,不走 SD/SPIFFS。
// 内置 30 秒限频(STT/LLM 两处调用共用):实际播放返回 true,被限频跳过返回 false。
extern bool playOfflineVoice();

#endif