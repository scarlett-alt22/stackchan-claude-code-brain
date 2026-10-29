#ifndef _SD_UTIL_H
#define _SD_UTIL_H

#include <stdint.h>

//★ OWNER 2026-07-27: CoreS3 挂 SD 的唯一入口, 别再直接写 SD.begin()。
//
// 为什么要有这个函数: 本仓库以前**从来没调用过 SPI.begin()**, 直接
// `SD.begin(GPIO_NUM_4, SPI, ...)` 用那个从没按 CoreS3 引脚配置过的默认 SPI
// 对象 —— SD 能工作纯粹是搭了 M5GFX 给屏幕初始化总线的便车。
//
// 🔴 CoreS3 的已知硬件冲突(M5Stack 官方社区/文档均有记载):
//    TFT 把 **MISO(GPIO35) 当 DC 用**。DC 必须是输出, 而 SD 需要 MISO 是输入,
//    两者共用同一条 SPI 总线。屏幕驱动一把该 GPIO 配成输出, SD 的读路就断
//    —— 写 SD 时卡要在 MISO 上回响应令牌, 读不到就表现为 write() 返回值变短、
//    串口刷 `ff_sd_status(): Check status failed`。
//
// 官方要求的正确顺序: 先按 CoreS3 引脚 SPI.begin(), 再 SD.begin()。
//    SCK=36  MISO=35  MOSI=37  CS=4   (docs.m5stack.com/en/arduino/m5cores3/sdcard)
//
// 2026-07-27 修复前的实测基线(修完拿这个对比, 别重测):
//    · sdtest 自检失败率 45%(基线) / 50%(关WiFi) / 79%(停GIF剧场) / 63%(去掉rename)
//    · 20 次开机: 一贯坏 13 / 一贯好 5 / 混合 2 —— **模式在开机时定死**
//    · 重挂 SD 救不回来(0/13)
//    · 换物理新卡失败率一模一样(55.6% vs 55.6%) —— 与卡无关
//★ OWNER 2026-07-28: 上面那个 SPI.begin() 到底是修好了还是修坏了, 至今没有
// 干净证据 —— 07-27 上线当天失败率 45%->83%, 当时把涨幅归因给温度; 07-28 冷机
// 实测 76%(跨 8 次开机, 同口径), **温度假设被推翻**, 于是"这一刀本身是负面的"
// 重新成为活着的嫌疑。
//
// 🔴 别用"回滚代码"做对照: 07-27 那一刀绑了两件事 —— 加 SPI.begin() **和**
//    五处漏改的 25MHz 降到 10MHz。回滚会一次动两个变量。
//
// 所以做成开机时可切的 arm: 存 NVS("sdcfg"/"spibegin"), 串口 `sdspi on|off`
// 设置, **复位后生效**。两臂时钟都是 10MHz, 唯一变量就是那一行 SPI.begin()。
bool sd_spi_begin_enabled();
void sd_spi_begin_set(bool enabled);

bool sd_begin_cores3(uint32_t clockHz = 10000000u);

int read_sd_file(const char* filename, char* buf, int buf_size);
int read_line_from_buf(char* buf, char* out);
bool copySDFileToSPIFFS(const char *path, bool forced);
size_t copySDFileToRAM(const char *path, uint8_t *out, int bufSize);

#endif