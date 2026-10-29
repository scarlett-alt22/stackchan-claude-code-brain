#ifndef _HEAD_TOUCH_SENSOR_H
#define _HEAD_TOUCH_SENSOR_H

#include <Arduino.h>

namespace HeadTouchSensor {

enum class Gesture {
  None,
  Press,
  Release,
  //★ OWNER 2026-07-26: Click = 拍头。按下后在 CLICK_MAX_MS 内松手、且中途没滑成
  //  Swipe。官方 StackChan-BSP 的 examples/TouchSensor 把这条触摸带也是抽象成
  //  wasClicked / wasSwipedForward / wasSwipedBackward 三个事件,这里对齐它的语义。
  Click,
  SwipeForward,
  SwipeBackward,
};

void begin(void);
Gesture update(void);
bool isAvailable(void);
bool isPetGesture(Gesture gesture);
const char *gestureName(Gesture gesture);

//★ OWNER 2026-10-11: 触摸门槛。Si12T 每通道输出 0~3(NONE/LOW/MID/HIGH),
//  任一通道 >= 门槛才算摸到。原来是 1(LOW 就算),10-10 主人没碰它却自己说"拍头"语句、
//  还进过一次 STT(录到空白,whisper 编出"字幕志愿者")→ 默认提到 2。
//  4 = 永远够不到 = 头顶触摸整个关掉(拍头/拍两下都不响应)。
//  存在 NVS "htouch"/"min";没存过 = 默认值。
constexpr uint8_t TOUCH_MIN_LEVEL_DEFAULT = 2;
uint8_t minLevel(void);
void setMinLevel(uint8_t level);   // 夹到 1~4,只改内存;存不存 NVS 由调用方决定

}  // namespace HeadTouchSensor

#endif  // _HEAD_TOUCH_SENSOR_H
