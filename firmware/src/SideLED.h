#pragma once
#include <cstdint>

// ★ OWNER 2026-07-18: 头顶两侧 12 颗 WS2812C 灯带。
//   硬件：灯带不在 ESP32 GPIO 上，而是挂在机身 PY32L020 IO 扩展器(I²C 0x6F, 走 M5.In_I2C
//   =内部 I²C GPIO11/12)后面，由其 IO14 驱动。用 stackchan-arduino 现成的 PY32 驱动设色。
//   行为：空闲=熄灭；说话/思考/摸头=橙色呼吸(只变亮度、不变色)。
//   颜色/呼吸参数在 SideLED.cpp 顶部的宏里，想调改那里重烧。

// 启动侧灯带任务(空闲熄灭 + 后续呼吸调度)。在 setup() 末尾调用一次。
void sc_led_begin();

// 持续呼吸开/关。用于有明确起止的时段(思考、说话)：起点 true、终点 false。
void sc_led_breathe(bool on);

// 呼吸再保持至少 ms 毫秒(到点自动熄灭)。用于瞬时/重复触发(摸头)：
// 每次事件刷新截止时刻，无需配对关闭。
void sc_led_breathe_hold(uint32_t ms);
