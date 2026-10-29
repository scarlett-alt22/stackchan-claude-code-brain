#ifndef _SD_SELF_TEST_H_
#define _SD_SELF_TEST_H_

//★ OWNER 2026-07-27: SD 写入自检 —— 把"写 SD 失败约半数"这条链路二分。
//
// 07-27 的实验全是走 /voice_upload 打的, 那条路把三件事混在一起:
//   WiFi 收包 + HTTP multipart 解析 + SD 写入
// 失败率 ~50% 到底出在哪一段, 那个测法分不出来。
//
// 本自检**完全不碰网络**: 数据在设备本地生成、写盘、读回、校验。
//   · 本地写也失败 ~50% -> WiFi/HTTP 被排除, 问题在 SD/SPI/供电
//   · 本地写干净        -> 问题在跟 WiFi 抢资源
//
// 写入刻意用 **2048 字节一块**, 跟 HTTP 上传缓冲同粒度 —— 07-27 实测失败点
// 全部落在 2048 的整数倍上, 换个粒度就不是同一个现象了。
// 校验用 FNV-1a, 跟 WebAPI.cpp 那份同一套, 能抓"写满了但内容是错的"。

#include <Arduino.h>

struct SdSelfTestOpts {
  int      rounds     = 18;      // 写多少个文件(默认对齐 07-27 那批 18 段语音)
  size_t   fileBytes  = 48u * 1024u;
  bool     wifiOff    = false;   // 测试期间关掉 WiFi
  bool     gifOff     = false;   // 测试期间停掉 GIF 剧场
  bool     noRename   = false;   // 跳过 .tmp+rename, 直接写目标名(少三次目录操作)
};

// 阻塞运行, 结果打串口。只能从 loop() 里调, 别在中断/任务里调。
void sd_self_test_run(const SdSelfTestOpts& opts);

//★ 2026-07-27 实测: SD 的好/坏模式**在挂载时就定死**(20 次开机里 18 次
// 整个会话一贯到底), 且坏模式占 ~65%。于是有个不用搞清物理原因的修法:
// 重挂 SD 相当于重新摇一次骰子。本函数就是量它到底成不成立 ——
// 反复 {写几个文件 -> 坏就 SD.end()+SD.begin() 重挂} 直到摇出好模式或用完次数。
//   能在几次内摇到好模式 -> 开机自检+重挂是条真出路
//   重挂多少次都还是坏     -> 模式不由挂载决定, 得往供电/温度查
struct SdRerollOpts {
  int      tries    = 8;
  int      probe    = 6;          // 每次探测写几个文件
  uint32_t clockHz  = 10000000u;  // 重挂时用的 SPI 时钟(10MHz 以下从没试过)
};
void sd_self_test_reroll(const SdRerollOpts& opts);

// 解析串口命令行:
//   "sdtest rounds=20 wifi=off gif=off"
//   "sdreroll tries=8 clk=4"        (clk 单位 MHz)
// 认得就跑并返回 true, 不认得返回 false。
bool sd_self_test_handle_command(const String& line);

#endif  //_SD_SELF_TEST_H_
