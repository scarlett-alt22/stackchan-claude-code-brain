#include "MotionReaction.h"

#if defined(ARDUINO_M5STACK_CORES3)
#include <Arduino.h>
#include <M5Unified.h>
#include <math.h>
#endif
#include <Preferences.h>

namespace MotionReaction {

namespace {
bool shakeOn = false;
bool shakeLoaded = false;

void loadShakeSetting()
{
  if (shakeLoaded) {
    return;
  }
  shakeLoaded = true;
  Preferences p;
  if (p.begin("motion", true)) {
    shakeOn = p.getBool("shake", false);
    p.end();
  }
}
}  // namespace

bool shakeEnabled(void)
{
  loadShakeSetting();
  return shakeOn;
}

void setShakeEnabled(bool on)
{
  shakeLoaded = true;
  shakeOn = on;
}

#if defined(ARDUINO_M5STACK_CORES3)
namespace {

constexpr unsigned long SAMPLE_INTERVAL_MS = 40;
constexpr unsigned long SHAKE_WINDOW_MS = 600;
constexpr unsigned long SHAKE_COOLDOWN_MS = 4000;
constexpr unsigned long PULSE_GAP_MS = 80;
constexpr float GRAVITY_FILTER = 0.92f;
constexpr float SHAKE_DELTA_G = 0.38f;
constexpr uint8_t SHAKE_PULSE_COUNT = 3;

bool initialized = false;
bool initTried = false;
unsigned long lastSampleMs = 0;
unsigned long lastPulseMs = 0;
unsigned long windowStartedMs = 0;
unsigned long cooldownUntilMs = 0;
uint8_t pulseCount = 0;
float gravityMagnitude = 1.0f;

}  // namespace
#endif

void begin(void)
{
#if defined(ARDUINO_M5STACK_CORES3)
  if (initTried) {
    return;
  }
  initTried = true;
  initialized = M5.Imu.isEnabled();
  Serial.printf("[MotionReaction] CoreS3 IMU %s\n", initialized ? "ready" : "not available");
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

bool update(void)
{
#if defined(ARDUINO_M5STACK_CORES3)
  begin();
  if (!initialized) {
    return false;
  }

  const unsigned long now = millis();
  if (now - lastSampleMs < SAMPLE_INTERVAL_MS) {
    return false;
  }
  lastSampleMs = now;

  float ax = 0.0f;
  float ay = 0.0f;
  float az = 0.0f;
  if (!M5.Imu.getAccel(&ax, &ay, &az)) {
    return false;
  }

  const float magnitude = sqrtf(ax * ax + ay * ay + az * az);
  const float delta = fabsf(magnitude - gravityMagnitude);
  gravityMagnitude = GRAVITY_FILTER * gravityMagnitude + (1.0f - GRAVITY_FILTER) * magnitude;

  if (now < cooldownUntilMs) {
    return false;
  }
  if (now - windowStartedMs > SHAKE_WINDOW_MS) {
    pulseCount = 0;
    windowStartedMs = now;
  }
  if (delta < SHAKE_DELTA_G || now - lastPulseMs < PULSE_GAP_MS) {
    return false;
  }

  if (pulseCount == 0) {
    windowStartedMs = now;
  }
  lastPulseMs = now;
  ++pulseCount;
  if (pulseCount < SHAKE_PULSE_COUNT) {
    return false;
  }

  pulseCount = 0;
  cooldownUntilMs = now + SHAKE_COOLDOWN_MS;
  if (!shakeEnabled()) {
    Serial.printf("[MotionReaction] shake ignored, switched off (delta=%.2fg)\n", delta);
    return false;
  }
  Serial.printf("[MotionReaction] shake detected (delta=%.2fg)\n", delta);
  return true;
#else
  return false;
#endif
}

}  // namespace MotionReaction
