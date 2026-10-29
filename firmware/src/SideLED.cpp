#include "SideLED.h"
#include <Arduino.h>
#include <M5Unified.h>
#include <math.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <drivers/PY32IOExpander_Class/PY32IOExpander_Class.hpp>

// ── 可调参数(改完重烧) ──────────────────────────────────────────────
#define SC_LED_COUNT       12       // 机身灯带灯数
#define SC_LED_R           255      // 橙色 R
#define SC_LED_G           80       // 橙色 G  (更红=调小, 更黄/琥珀=调大)
#define SC_LED_B           0        // 橙色 B
#define SC_LED_BREATH_MIN  25       // 呼吸最暗亮度(0-255)，不全灭
#define SC_LED_BREATH_STEP 0.10f    // 每帧相位增量，越大呼吸越快(~2 秒/次)
#define SC_LED_FRAME_MS    33       // 呼吸刷新周期 ~30fps
// ────────────────────────────────────────────────────────────────────

// PY32 IO 扩展器(0x6F, 内部 I²C)。begin() 只读版本寄存器确认芯片在，不复位、不扰动舵机；
// LED 用独立寄存器(0x24/0x30+)，与舵机 GPIO 寄存器不重叠。In_I2C 底层走 IDF i2c 驱动
// (per-port 互斥)，与摸头/电源轮询并发安全(现有 battery_check 任务已是同款先例)。
static m5::PY32IOExpander_Class sc_ioex(0x6F, &M5.In_I2C);

static volatile bool     sc_breathing     = false;  // 持续呼吸开关
static volatile uint32_t sc_breathe_until = 0;       // 呼吸截止时刻(millis)

// 把 12 颗灯一次性设成"橙色 × scale/255"的亮度并刷新(scale=255 满亮)。
// 用 setLedData 一笔写完 24 字节(12×RGB565)，比逐颗 setLedColor 少 11 笔 I²C 事务。
static void sc_led_write(uint8_t scale)
{
  uint8_t r = (uint16_t)SC_LED_R * scale / 255;
  uint8_t g = (uint16_t)SC_LED_G * scale / 255;
  uint8_t b = (uint16_t)SC_LED_B * scale / 255;
  uint16_t c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);   // RGB888 → RGB565
  uint8_t buf[SC_LED_COUNT * 2];
  for (int i = 0; i < SC_LED_COUNT; i++) { buf[i * 2] = c & 0xFF; buf[i * 2 + 1] = (c >> 8) & 0xFF; }
  sc_ioex.setLedData(buf, sizeof(buf));
  sc_ioex.setLedCount(SC_LED_COUNT);   // OWNER 2026-07-19: 重写 count 只动 CFG[5:0]，顺带清掉 refresh 的 bit6，
                                       // 让下一次 refreshLeds 有 0→1 边沿(否则呼吸帧只写进 RAM 不锁存 → 停在上一帧)
  sc_ioex.refreshLeds();
}

static void sc_led_task(void*)
{
  // The PY32 controller can come up after the ESP32 following a USB reset.
  // Do not send an LED frame until it has answered its version probe; retrying
  // here keeps the existing breathing behaviour while recovering from that
  // startup race.
  while (!sc_ioex.begin()) {
    vTaskDelay(pdMS_TO_TICKS(200));
  }

  // The RGB data line is routed through PY32 GPIO13.  The factory BSP
  // explicitly enables that output before using the LED engine; after a power
  // loss it is not retained, so omitting this leaves every LED dark.
  sc_ioex.setDirection(13, true);
  sc_ioex.setPullMode(13, true);
  sc_ioex.setDriveMode(13, false);
  sc_ioex.setLedCount(SC_LED_COUNT);
  vTaskDelay(pdMS_TO_TICKS(200));
  sc_led_write(0);                   // 空闲时熄灭；呼吸触发后仍由下方原逻辑驱动
  float phase = 0.0f;
  bool  idle_dark = true;
  for (;;) {
    bool breathe = sc_breathing || (int32_t)(sc_breathe_until - millis()) > 0;
    if (breathe) {
      phase += SC_LED_BREATH_STEP;
      if (phase > 6.2831853f) phase -= 6.2831853f;
      float s = (sinf(phase) + 1.0f) * 0.5f;                       // 0..1
      uint8_t scale = SC_LED_BREATH_MIN + (uint8_t)(s * (255 - SC_LED_BREATH_MIN));
      sc_led_write(scale);
      idle_dark = false;
      vTaskDelay(pdMS_TO_TICKS(SC_LED_FRAME_MS));
    } else {
      if (!idle_dark) { sc_led_write(0); idle_dark = true; phase = 0.0f; }  // 呼吸结束→熄灭
      vTaskDelay(pdMS_TO_TICKS(60));
    }
  }
}

void sc_led_begin()
{
  xTaskCreate(sc_led_task, "sc_led", 4096, nullptr, 1, nullptr);
}

void sc_led_breathe(bool on)          { sc_breathing = on; }
void sc_led_breathe_hold(uint32_t ms) { sc_breathe_until = millis() + ms; }
