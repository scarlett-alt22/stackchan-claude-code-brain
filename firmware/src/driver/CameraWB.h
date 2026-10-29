#ifndef _CAMERA_WB_H
#define _CAMERA_WB_H

#include <Arduino.h>

//★ code-to-muse 20261028-174524-stackchan-firmware-wb-and-phot: GC0308 白平衡手调。
// 背景: esp32-camera 2.0.4 里 GC0308 的 set_wb_mode / set_whitebal / set_awb_gain
// 全是 set_dummy(sensors/gc0308.c:438/445/450), sensor->set_wb_mode() 是空转;
// 上游 PR #814(espressif/esp32-camera, "Proper support for more sensor
// parameters of GC0308", 已合入并发在 v2.1.5)才把它们做实。不升级库, 直接用
// 2.0.4 已经暴露的寄存器写入口: sensor_t::set_reg(driver/include/sensor.h:249;
// GC0308 在 sensors/gc0308.c:461 挂上, 实现在 gc0308.c:344: 读-改-写,
// value 先按 mask 取位再合并, 所以整字节写用 mask=0xFF, 单 bit 用该 bit 的掩码)。
// 寄存器图照 PR #814 的 gc0308_regs.h + set_wb_mode(全部 page 0):
//   页选: 寄存器 0xFE 写 0x00 (RESET_RELATED 的 bit[0] 是页选, 见 gc0308_settings.h:9)
//   AAAA_EN = 0x22, bit1(掩码 0x02) = AWB 使能 (bit0=AEC, bit2=AGC, 别碰)
//   AWB_R_GAIN = 0x5A, AWB_G_GAIN = 0x5B, AWB_B_GAIN = 0x5C
// 预设增益同 PR #814 的 set_wb_mode 原值:
//   0 auto: AWB 开, 0x56/0x40/0x4A
//   1 sunny: 0x74/0x52/0x40    2 cloudy: 0x8C/0x50/0x40
//   3 office: 0x48/0x40/0x5C   4 home:  0x40/0x42/0x50   (1~4 AWB 关)
// 存 NVS 自己的命名空间 "camerawb"(键 mode/custom/r/g/b), 路子同 /volume、
// /headtouch。没存过 = 本模块什么都不做: 不读也不写寄存器, 照片行为和
// 从前一字不差; 存过才在每次相机 init 后、抓帧前应用一次并打一行日志。
namespace CameraWB {

constexpr uint8_t MODE_AUTO = 0;
constexpr uint8_t MODE_MAX = 4;

struct Setting {
  uint8_t mode;    // 0~4; custom 时只作记录, 增益以下面 r/g/b 为准
  bool custom;     // true = 手动增益(AWB 关)
  uint8_t r, g, b;
};

// 当前生效设定(内存值; 首次访问时从 NVS 懒加载)。没设定过返回 false。
bool current(Setting& out);
// NVS 里有没有存值(给 /wb 的 saved 字段; 每次现查, 不走缓存)
bool savedInNVS(void);
// 只改内存: 相机开着时调用方应再调 applyNow() 立即生效, 否则下次 init 后应用
void setPreset(uint8_t mode);
void setCustom(uint8_t r, uint8_t g, uint8_t b);
// 把内存设定存进 NVS / 删 NVS 存值并清内存(回到"没存过"的状态)
void saveToNVS(void);
void clearSaved(void);
// 相机在不在(esp_camera_sensor_get() != NULL)
bool cameraOn(void);
// 把内存设定写进传感器寄存器; 没设定/相机没开/写失败返回 false
bool applyNow(void);
// 每次相机 init 成功后调: 有设定才写寄存器并打一行日志; 没设定什么都不做
void applySavedAfterInit(void);

}  // namespace CameraWB

#endif  // _CAMERA_WB_H
