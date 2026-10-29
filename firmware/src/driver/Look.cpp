#include "Look.h"

#include <Arduino.h>
#include <Preferences.h>
#include "Robot.h"
#include "llm/LLMBase.h"        // chatHistory
#include "driver/GifPlayer.h"
#if defined(ENABLE_CAMERA)
#include "driver/Camera.h"
#endif

namespace Look {

namespace {
bool lookOn = true;
bool lookLoaded = false;
bool inLook = false;

void loadSetting()
{
  if (lookLoaded) {
    return;
  }
  lookLoaded = true;
  Preferences p;
  if (p.begin("look", true)) {
    lookOn = p.getBool("on", true);
    p.end();
  }
}

bool isWordByte(uint8_t b)
{
  // 网关是 Python 的 \b(Unicode):look 后面紧跟中文也算词内,不当标记。UTF-8 多字节一律按词内处理。
  return isalnum(b) || b == '_' || b >= 0x80;
}

// s[i] 起是一个 [[look…]] 标记就返回它结束后的下标,否则 -1。
// 对应网关 LOOK_TAG_RE = r"\[\[\s*look\b[^\]]*\]\]"(IGNORECASE)。
int tagEndAt(const String& s, int i)
{
  const int n = s.length();
  if (i + 1 >= n || s[i] != '[' || s[i + 1] != '[') return -1;
  int j = i + 2;
  while (j < n && isspace((uint8_t)s[j])) j++;
  if (j + 4 > n) return -1;
  const char* word = "look";
  for (int k = 0; k < 4; k++) {
    if (tolower((uint8_t)s[j + k]) != word[k]) return -1;
  }
  j += 4;
  if (j < n && isWordByte((uint8_t)s[j])) return -1;
  while (j < n && s[j] != ']') j++;
  if (j + 1 < n && s[j + 1] == ']') return j + 2;
  return -1;
}
}  // namespace

bool enabled(void)
{
  loadSetting();
  return lookOn;
}

void setEnabled(bool on)
{
  lookLoaded = true;
  lookOn = on;
}

bool stripTags(String& text)
{
  bool found = false;
  String out;
  out.reserve(text.length());
  const int n = text.length();
  int i = 0;
  while (i < n) {
    int end = tagEndAt(text, i);
    if (end > 0) {
      found = true;
      i = end;
    } else {
      out += text[i];
      i++;
    }
  }
  if (found) {
    out.trim();
    text = out;
  }
  return found;
}

bool busy(void)
{
  return inLook;
}

// 说一句,同时记进对话历史(下一轮提问时设备会把历史带给网关)
static void say(const String& text, bool remember)
{
  robot->speech(text);
  if (remember) {
    chatHistory.push_back(String("assistant"), String(""), text);
  }
}

void lookAndAnswer(void)
{
  if (inLook) return;
  inLook = true;
  Serial.printf("[look] t=%lu begin\n", millis());

#if defined(ENABLE_CAMERA)
#ifdef USE_SERVO
  // 回正再拍(moveXY 是阻塞的,转完才返回)。摄像头在屏幕那面,"正前方"＝主人那边。
  if (robot->servo != nullptr) robot->servo->moveToOrigin();
#endif
  camera_shutter_tone();
  // 侧灯:speech() 末尾已 sc_led_breathe(false),灯任务 ≤100ms 内写完最后一帧就不再碰内部 I2C;
  // 上面回正(~1s)+快门(~0.3s)足够隔开,拍照时不会和 SCCB 抢总线。别把拍照挪到这两步前面。

  uint8_t* jpg = nullptr;
  size_t jpg_len = 0;
  if (!camera_snap_jpeg(&jpg, &jpg_len)) {
    Serial.println("[look] snap failed");
    say("相机没拍成，你再让我看一次吧。", false);
    inLook = false;
    return;
  }

  String photoUrl = camera_photo_url(robot->m_config.getExConfig().llm.customEndpoint);
  String reply, lookError;
  bool uploaded = false;
  if (photoUrl.length() == 0) {
    Serial.printf("[look] cannot derive /v1/photos from customEndpoint: %s\n",
                  robot->m_config.getExConfig().llm.customEndpoint.c_str());
  } else {
    // 等回答这二三十秒播思考场景(拍完摄像头已 deinit、I2C 已还,和剧场不冲突)
    if (gif_theater_on()) gif_scene_start(GIF_SCENE_THINKING);
    String apiKey = robot->m_config.getAPISetting()->ai_service;
    String rootCA = robot->m_config.getExConfig().llm.customRootCA;
    uploaded = camera_upload_photo_look(photoUrl.c_str(), apiKey.c_str(), rootCA.c_str(),
                                        jpg, jpg_len, reply, lookError);
    gif_scene_stop();
  }
  free(jpg);
  Serial.printf("[look] t=%lu bytes=%u uploaded=%d reply_len=%u error=%s\n", millis(),
                (unsigned)jpg_len, (int)uploaded, (unsigned)reply.length(), lookError.c_str());

  stripTags(reply);   // 网关已经剥过,双保险:念回答这一轮不能再触发 look
  if (!uploaded) {
    say("照片没传上去，网络可能不太好。", false);
  } else if (reply.length() > 0) {
    say(reply, true);
  } else if (lookError == "look_disabled") {
    say("看的功能在电脑那边关着。", false);
  } else {
    say("这次没看成，你再问我一次吧。", false);
  }
#else
  say("我这版没有开相机，看不了。", false);
#endif

  inLook = false;
  Serial.printf("[look] t=%lu end\n", millis());
}

}  // namespace Look
