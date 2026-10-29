#include "HeadTouchSensor.h"

#include <Preferences.h>

#if defined(ARDUINO_M5STACK_CORES3)
#include <M5Unified.h>
#include "drivers/Si12T/Si12T.h"
#endif

namespace HeadTouchSensor {

namespace {

uint8_t touchMinLevel = TOUCH_MIN_LEVEL_DEFAULT;

#if defined(ARDUINO_M5STACK_CORES3)
enum class TouchState { Idle, Touched, Swiping };

si12t_handle_t si12tHandle = nullptr;
bool initialized = false;
bool initTried = false;
unsigned long lastSampleMs = 0;
TouchState touchState = TouchState::Idle;
int16_t initialPosition = 0;
unsigned long touchStartedMs = 0;
int16_t maxTravel = 0;   // 本次触摸期间质心位置离出发点的最大距离（绝对值）
uint8_t peakLevel = 0;   // 本次触摸期间三通道里出现过的最大读数(0~3),打进 release 那行
//★ OWNER 2026-10-11: 低于门槛的读数不算触摸,但要留痕——这是"没碰却自己响"
//  被挡下来的证据。噪声可能每 50ms 都来,所以最多每秒打一行,带上这一秒里挡了几次。
unsigned long belowLogMs = 0;
uint16_t belowCount = 0;
uint8_t belowPeak = 0;

constexpr uint32_t I2C_FREQ = 100000;
constexpr unsigned long SAMPLE_INTERVAL_MS = 50;
//★ OWNER 2026-07-26: 40 -> 25。实测主人一次自然的撸头常常够不到 40（位置刻度
//  是 -100~+100 的三通道加权质心），够不到就不算 swipe，配上下面的 Click 判定
//  会被误报成"拍头"。真实幅度由 [HeadTouch/raw] 那行埋点给出，别再拍脑袋调。
constexpr int16_t SWIPE_THRESHOLD = 25;
//★ 拍头必须"几乎没横移"。原来只看时长（500ms 内松手就算 Click），于是一次
//  幅度不够的轻撸会掉进 Click 被当成拍头——这是我 07-26 加 Click 时漏掉的一层，
//  改之前这种情况是什么都不发生，加了 Click 之后才变成误报。
constexpr int16_t CLICK_MAX_TRAVEL = 15;
//★ OWNER 2026-07-26: 按住超过这个时长再松手就不算"拍",只发 Release。
//  采样周期 50ms,所以真正的一次拍(手掌接触 100~300ms)至少能采到 2~6 次 touched;
//  比 50ms 还快的弹指式轻点有可能整个落在两次采样之间被漏掉——真拍不会那么快。
constexpr unsigned long CLICK_MAX_MS = 500;

Gesture updateGesture(const uint8_t channel[3])
{
  const uint16_t total = channel[0] + channel[1] + channel[2];
  int16_t currentPosition = 0;
  if (total != 0) {
    const int32_t weighted = channel[0] * (-100) + channel[1] * 0 + channel[2] * 100;
    currentPosition = static_cast<int16_t>(weighted / total);
  }

  const uint8_t maxIntensity = max(channel[0], max(channel[1], channel[2]));
  const bool touched = maxIntensity >= touchMinLevel;
  Gesture gesture = Gesture::None;
  if (touched && maxIntensity > peakLevel) peakLevel = maxIntensity;

  switch (touchState) {
    case TouchState::Idle:
      if (touched) {
        touchState = TouchState::Touched;
        initialPosition = currentPosition;
        touchStartedMs = millis();
        maxTravel = 0;
        peakLevel = maxIntensity;
        gesture = Gesture::Press;
      } else if (maxIntensity > 0) {
        belowCount++;
        if (maxIntensity > belowPeak) belowPeak = maxIntensity;
        const unsigned long now = millis();
        if (now - belowLogMs >= 1000) {
          Serial.printf("[HeadTouch/raw] below min=%u: %u reading(s), peak=%u (ch=%u/%u/%u)\n",
                        touchMinLevel, belowCount, belowPeak, channel[0], channel[1], channel[2]);
          belowLogMs = now;
          belowCount = 0;
          belowPeak = 0;
        }
      }
      break;

    case TouchState::Touched:
      if (!touched) {
        touchState = TouchState::Idle;
        //★ OWNER 2026-07-26: 松手时分三种。
        //  拍头(Click) = 短按 **且几乎没横移**。只看时长是不够的:一次幅度不到
        //  SWIPE_THRESHOLD 的轻撸也会在 500ms 内松手,那不是拍头。
        const unsigned long held = millis() - touchStartedMs;
        const bool quick = held <= CLICK_MAX_MS;
        const bool still = maxTravel < CLICK_MAX_TRAVEL;
        gesture = (quick && still) ? Gesture::Click : Gesture::Release;
        Serial.printf("[HeadTouch/raw] release held=%lums travel=%d peak=%u -> %s\n",
                      held, (int)maxTravel, peakLevel, gestureName(gesture));
      } else {
        const int16_t delta = currentPosition - initialPosition;
        const int16_t travel = delta < 0 ? -delta : delta;
        if (travel > maxTravel) maxTravel = travel;
        //★ OWNER 2026-07-26: 极性在这里掰正,不要挪到调用方去改。
        //  实测(真机、单动作测):主人"从前往后顺着撸"产生的是 **delta 为负**
        //  (ch=0/3/3, delta=-50, maxTravel 到 150)。三通道的物理排列跟
        //  channel[0]=-100 / channel[2]=+100 那个假设正好相反。
        //  在这里对调之后,SwipeForward 就真的等于"顺毛",上层读起来不拧巴。
        if (delta < -SWIPE_THRESHOLD) {
          touchState = TouchState::Swiping;
          gesture = Gesture::SwipeForward;
          Serial.printf("[HeadTouch/raw] swipe delta=%d (ch=%u/%u/%u) -> Forward(顺毛)\n",
                        (int)delta, channel[0], channel[1], channel[2]);
        } else if (delta > SWIPE_THRESHOLD) {
          touchState = TouchState::Swiping;
          gesture = Gesture::SwipeBackward;
          Serial.printf("[HeadTouch/raw] swipe delta=%d (ch=%u/%u/%u) -> Backward(逆毛)\n",
                        (int)delta, channel[0], channel[1], channel[2]);
        }
      }
      break;

    case TouchState::Swiping:
      if (!touched) {
        touchState = TouchState::Idle;
        gesture = Gesture::Release;
        Serial.printf("[HeadTouch/raw] swipe end, maxTravel=%d peak=%u\n", (int)maxTravel, peakLevel);
      } else {
        const int16_t delta = currentPosition - initialPosition;
        const int16_t travel = delta < 0 ? -delta : delta;
        if (travel > maxTravel) maxTravel = travel;
      }
      break;
  }

  return gesture;
}
#endif

}  // namespace

void begin(void)
{
#if defined(ARDUINO_M5STACK_CORES3)
  if (initTried) {
    return;
  }
  initTried = true;

  {
    Preferences p;
    if (p.begin("htouch", true)) {
      setMinLevel(p.getUChar("min", TOUCH_MIN_LEVEL_DEFAULT));
      p.end();
    }
  }
  Serial.printf("[HeadTouch] min level=%u\n", touchMinLevel);

  si12t_config_t config = {};
  config.i2c = &M5.In_I2C;
  config.dev_addr = SI12T_GND_ADDRESS;
  config.freq = I2C_FREQ;

  esp_err_t ret = si12t_init(&config, &si12tHandle);
  if (ret != ESP_OK) {
    Serial.printf("[HeadTouch] Si12T init failed: %s\n", esp_err_to_name(ret));
    return;
  }

  ret = si12t_setup(si12tHandle, SI12T_TYPE_LOW, SI12T_SENSITIVITY_LEVEL_3);
  if (ret != ESP_OK) {
    Serial.printf("[HeadTouch] Si12T setup failed: %s\n", esp_err_to_name(ret));
    si12t_delete(si12tHandle);
    si12tHandle = nullptr;
    return;
  }

  initialized = true;
  Serial.println("[HeadTouch] Si12T initialized");
#endif
}

Gesture update(void)
{
#if defined(ARDUINO_M5STACK_CORES3)
  begin();
  if (!initialized) {
    return Gesture::None;
  }

  const unsigned long now = millis();
  if (now - lastSampleMs < SAMPLE_INTERVAL_MS) {
    return Gesture::None;
  }
  lastSampleMs = now;

  uint8_t touchResult = 0;
  uint8_t channel[3] = {0, 0, 0};
  esp_err_t ret = si12t_read_touch_result(si12tHandle, &touchResult);
  if (ret != ESP_OK) {
    Serial.printf("[HeadTouch] read failed: %s\n", esp_err_to_name(ret));
    return Gesture::None;
  }

  si12t_parse_touch_result_to(touchResult, channel);
  return updateGesture(channel);
#else
  return Gesture::None;
#endif
}

bool isAvailable(void)
{
#if defined(ARDUINO_M5STACK_CORES3)
  begin();
  return initialized;
#else
  return false;
#endif
}

uint8_t minLevel(void)
{
  return touchMinLevel;
}

void setMinLevel(uint8_t level)
{
  if (level < 1) level = 1;
  if (level > 4) level = 4;
  touchMinLevel = level;
}

bool isPetGesture(Gesture gesture)
{
  return gesture == Gesture::SwipeForward || gesture == Gesture::SwipeBackward;
}

const char *gestureName(Gesture gesture)
{
  switch (gesture) {
    case Gesture::Press:
      return "Press";
    case Gesture::Release:
      return "Release";
    case Gesture::Click:
      return "Click";
    case Gesture::SwipeForward:
      return "SwipeForward";
    case Gesture::SwipeBackward:
      return "SwipeBackward";
    case Gesture::None:
    default:
      return "None";
  }
}

}  // namespace HeadTouchSensor
