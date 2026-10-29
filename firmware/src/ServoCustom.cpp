#include "ServoCustom.h"
#include <Preferences.h>

static const char* NVS_NS  = "servo";
static const char* NVS_KEY = "originY";

void ServoCustom::moveToOrigin(){
  moveXY(_init_param.servo[AXIS_X].start_degree, _init_param.servo[AXIS_Y].start_degree, 1000);
}

void ServoCustom::moveTo(int degX, int degY){
  moveXY(_init_param.servo[AXIS_X].start_degree + degX, _init_param.servo[AXIS_Y].start_degree + degY, 1000);
}

void ServoCustom::moveTo(int degX, int degY, uint32_t millis_for_move){
  moveXY(_init_param.servo[AXIS_X].start_degree + degX, _init_param.servo[AXIS_Y].start_degree + degY, millis_for_move);
}

//★ OWNER 2026-10-04: 待机俯仰角。主人要"屏幕和舵机保持水平,不要抬头"。
static int clampOriginY(int degY) {
  if (degY < ServoCustom::ORIGIN_Y_MIN) return ServoCustom::ORIGIN_Y_MIN;
  if (degY > ServoCustom::ORIGIN_Y_MAX) return ServoCustom::ORIGIN_Y_MAX;
  return degY;
}

bool ServoCustom::hasSavedOriginY() const {
  Preferences p;
  if (!p.begin(NVS_NS, true)) return false;   // 只读打开,命名空间没建过=没存值
  bool has = p.isKey(NVS_KEY);
  p.end();
  return has;
}

void ServoCustom::applySavedOrigin() {
  _yaml_origin_y = _init_param.servo[AXIS_Y].start_degree;
  Preferences p;
  if (!p.begin(NVS_NS, true)) {
    Serial.printf("[Servo] 待机俯仰 Y=%d (yaml)\n", _yaml_origin_y);
    return;
  }
  if (p.isKey(NVS_KEY)) {
    int y = clampOriginY(p.getShort(NVS_KEY, _yaml_origin_y));
    _init_param.servo[AXIS_Y].start_degree = y;
    Serial.printf("[Servo] 待机俯仰 Y=%d (NVS 存值,yaml=%d)\n", y, _yaml_origin_y);
  } else {
    Serial.printf("[Servo] 待机俯仰 Y=%d (yaml)\n", _yaml_origin_y);
  }
  p.end();
}

int ServoCustom::setOriginY(int degY, bool persist) {
  int y = clampOriginY(degY);
  _init_param.servo[AXIS_Y].start_degree = y;
  if (persist) {
    Preferences p;
    if (p.begin(NVS_NS, false)) {
      p.putShort(NVS_KEY, y);
      p.end();
    } else {
      Serial.println("[Servo] ❌ NVS 打不开,只改了这次开机");
    }
  }
  moveToOrigin();
  return y;
}

void ServoCustom::clearSavedOriginY() {
  Preferences p;
  if (p.begin(NVS_NS, false)) {
    p.remove(NVS_KEY);
    p.end();
  }
  if (_yaml_origin_y >= 0) _init_param.servo[AXIS_Y].start_degree = _yaml_origin_y;
  moveToOrigin();
}
