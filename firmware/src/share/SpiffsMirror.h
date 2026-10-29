#ifndef _SPIFFS_MIRROR_H
#define _SPIFFS_MIRROR_H

#include <Arduino.h>

// ★ OWNER 2026-07-28: SD -> SPIFFS 配置/语音镜像。
//
// 目的: 让 SD 彻底退出运行关键路径。此前 WiFi 凭据、三份 yaml 配置、CA 证书、
// 触摸语音全压在 SD 上, 而这台 CoreS3 的 SD 读写半数开机不可靠(板级缺陷,
// 见 SDUtil.h), 卡的 FAT 三天内报废三次, 每次 = 整机变砖(开机死循环)。
//
// 做法: SD 挂载成功的每次开机, 把下列文件镜像进 SPIFFS(变了才写, 写完读回
// FNV 校验, 校验不过删掉镜像并保留上一份的缺失状态——绝不留半截文件):
//   /app/AiStackChanEx/SC_ExConfig.yaml -> /SC_ExConfig.yaml   (SPIFFS 31 字符上限, 全路径放不下)
//   /yaml/SC_BasicConfig.yaml           -> 同路径
//   /yaml/SC_SecConfig.yaml             -> 同路径
//   SD 根目录 *.pem                      -> 同路径 (yaml 里 customRootCAFile 引用的就是根路径)
//   /app/AiStackChanEx/voice/<cat>/*.wav -> /v/<cat>/*.wav
// SD 挂载失败的开机, main.cpp 直接 loadConfig(SPIFFS, ...) 用镜像起机。
//
// 策略: yaml/pem 每次开机做内容级比对(FNV, 文件小, 开销可忽略);
// 语音只比"缺失或大小不同"(920KB 全量 FNV 每次开机太贵), 内容级的新鲜度由
// /voice_upload 成功后当场调 spiffs_mirror_voice_file() 保证。
void spiffs_mirror_from_sd(bool force = false);

// 单个语音文件 SD -> SPIFFS 同步(FNV 读回校验)。/voice_upload 成功后调用,
// 这样换台词不用等下次开机、也不依赖"大小恰好变了"。
bool spiffs_mirror_voice_file(const String& sdPath);

// 删除语音的 SPIFFS 镜像副本。/asset_delete 成功后调用。
void spiffs_mirror_voice_remove(const String& sdPath);

// 串口命令 "mirror" = 强制全量重镜像(force=true)。认得返回 true。
bool spiffs_mirror_handle_command(const String& cmd);

#endif  // _SPIFFS_MIRROR_H
