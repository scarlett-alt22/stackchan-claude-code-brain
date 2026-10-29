#ifndef _VOICE_CLIP_H
#define _VOICE_CLIP_H

#include <Arduino.h>

// ★ OWNER 2026-07-26: 触摸反馈的"说话"。
//
// 为什么不走 robot->speech()：那条路要打 Mac 上的克隆音色 server，实测首字延迟
// 4~8 秒（10 字 6.2s）。摸一下头、八秒后才"哎呀"——那不是慢，是坏了。所以触摸
// 反馈一律走本机 SD 上的预合成 WAV，出声 <0.5s、不烧 token、断网也在。
//
// 句库怎么来：Mac 上 tools-voice/make_touch_voices.py 用同一个克隆音色 server
// 离线批量合成，写成标准 WAV（改写掉流式头的 0xFFFFFFFF 长度）放到
//   /app/AiStackChanEx/voice/<category>/*.wav
// 每类放几句，播的时候随机抽一句——跟 GIF 剧场同一套做法。目录不存在或是空的
// 就静默跳过，不报错、不影响其它反馈。
namespace VoiceClip {

// 分类目录名。加新类只要在 SD 上建个目录、这里加个常量。
#define VOICE_CLIP_ROOT      "/app/AiStackChanEx/voice"
#define VOICE_CLIP_PAT       VOICE_CLIP_ROOT "/pat"        // 拍头
#define VOICE_CLIP_PET       VOICE_CLIP_ROOT "/pet"        // 顺毛撸
#define VOICE_CLIP_PET_BACK  VOICE_CLIP_ROOT "/pet_back"   // 逆毛撸
#define VOICE_CLIP_SHAKE     VOICE_CLIP_ROOT "/shake"      // 被摇晃

// 开机时把四个分类目录建出来（已存在则跳过）。
// 🔴 必须由设备侧建：机上那个 esp8266FTPServer 的 MKD 是**硬编码返回失败**的
// （ESP8266FtpServer.cpp:619，注释原文 "not support on esp yet"），
// 所以从 Mac 走 FTP 推语音之前，目录必须先在这里存在，否则 STOR 报
// "451 Can't open/create"。要在 SD 挂载之后调用。
void begin(void);

// 从 category 目录里随机抽一句播完（阻塞）。目录空/无 SD/文件坏都返回 false。
bool play(const char *category);

}  // namespace VoiceClip

#endif  // _VOICE_CLIP_H
