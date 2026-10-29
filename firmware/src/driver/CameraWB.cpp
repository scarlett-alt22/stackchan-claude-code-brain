#include "CameraWB.h"

#include <Preferences.h>
#include <esp_camera.h>

namespace {

// 寄存器地址: PR #814 的 gc0308_regs.h, 全部在 page 0
constexpr int REG_PAGE_SELECT = 0xFE;   // bit[0]: 0 = page0, 1 = page1
constexpr int REG_AAAA_EN = 0x22;       // bit0 AEC / bit1 AWB / bit2 AGC
constexpr int AAAA_AWB_MASK = 0x02;
constexpr int REG_AWB_R_GAIN = 0x5A;
constexpr int REG_AWB_G_GAIN = 0x5B;
constexpr int REG_AWB_B_GAIN = 0x5C;

// 预设增益 {R, G, B}, 下标 = mode, PR #814 set_wb_mode 原值
const uint8_t PRESET_GAINS[5][3] = {
    {0x56, 0x40, 0x4A},  // 0 auto
    {0x74, 0x52, 0x40},  // 1 sunny
    {0x8C, 0x50, 0x40},  // 2 cloudy
    {0x48, 0x40, 0x5C},  // 3 office
    {0x40, 0x42, 0x50},  // 4 home
};

const char* NVS_NS = "camerawb";

bool s_loaded = false;
bool s_has = false;
CameraWB::Setting s_setting = {0, false, 0x56, 0x40, 0x4A};

void ensureLoaded()
{
  if (s_loaded) return;
  s_loaded = true;
  Preferences p;
  if (!p.begin(NVS_NS, true)) return;   // 只读: 命名空间不存在就当没存过
  if (p.isKey("mode")) {
    uint8_t mode = p.getUChar("mode", CameraWB::MODE_AUTO);
    if (mode > CameraWB::MODE_MAX) mode = CameraWB::MODE_AUTO;
    s_setting.mode = mode;
    s_setting.custom = p.getBool("custom", false);
    s_setting.r = p.getUChar("r", PRESET_GAINS[mode][0]);
    s_setting.g = p.getUChar("g", PRESET_GAINS[mode][1]);
    s_setting.b = p.getUChar("b", PRESET_GAINS[mode][2]);
    s_has = true;
  }
  p.end();
}

}  // namespace

namespace CameraWB {

bool current(Setting& out)
{
  ensureLoaded();
  if (!s_has) return false;
  out = s_setting;
  return true;
}

bool savedInNVS()
{
  Preferences p;
  if (!p.begin(NVS_NS, true)) return false;
  bool saved = p.isKey("mode");
  p.end();
  return saved;
}

void setPreset(uint8_t mode)
{
  if (mode > MODE_MAX) mode = MODE_AUTO;
  ensureLoaded();
  s_setting.mode = mode;
  s_setting.custom = false;
  s_setting.r = PRESET_GAINS[mode][0];
  s_setting.g = PRESET_GAINS[mode][1];
  s_setting.b = PRESET_GAINS[mode][2];
  s_has = true;
}

void setCustom(uint8_t r, uint8_t g, uint8_t b)
{
  ensureLoaded();
  s_setting.custom = true;
  s_setting.r = r;
  s_setting.g = g;
  s_setting.b = b;
  s_has = true;
}

void saveToNVS()
{
  ensureLoaded();
  if (!s_has) return;
  Preferences p;
  if (!p.begin(NVS_NS, false)) return;
  p.putUChar("mode", s_setting.mode);
  p.putBool("custom", s_setting.custom);
  p.putUChar("r", s_setting.r);
  p.putUChar("g", s_setting.g);
  p.putUChar("b", s_setting.b);
  p.end();
}

void clearSaved()
{
  Preferences p;
  if (p.begin(NVS_NS, false)) {
    p.remove("mode");
    p.remove("custom");
    p.remove("r");
    p.remove("g");
    p.remove("b");
    p.end();
  }
  s_loaded = true;
  s_has = false;
}

bool cameraOn()
{
  return esp_camera_sensor_get() != NULL;
}

bool applyNow()
{
  Setting cur;
  if (!current(cur)) return false;
  sensor_t* s = esp_camera_sensor_get();
  if (s == NULL || s->set_reg == NULL) return false;
  const bool awb = !cur.custom && cur.mode == MODE_AUTO;
  // 照 PR #814 set_wb_mode 的顺序: 先选 page 0, 再 AWB 使能位, 再三个增益。
  // set_reg 是读-改-写: AWB 位用 mask=0x02 只动 bit1, 不碰 AEC/AGC。
  int rc = 0;
  rc |= s->set_reg(s, REG_PAGE_SELECT, 0xFF, 0x00);
  rc |= s->set_reg(s, REG_AAAA_EN, AAAA_AWB_MASK, awb ? AAAA_AWB_MASK : 0x00);
  rc |= s->set_reg(s, REG_AWB_R_GAIN, 0xFF, cur.r);
  rc |= s->set_reg(s, REG_AWB_G_GAIN, 0xFF, cur.g);
  rc |= s->set_reg(s, REG_AWB_B_GAIN, 0xFF, cur.b);
  return rc == 0;
}

void applySavedAfterInit()
{
  Setting cur;
  if (!current(cur)) return;   // 没存过: 不碰寄存器, 行为和从前一致
  const bool awb = !cur.custom && cur.mode == MODE_AUTO;
  if (applyNow()) {
    Serial.printf("[WB] applied after camera init: mode=%u custom=%d gains=%u/%u/%u awb=%s\n",
                  cur.mode, (int)cur.custom, cur.r, cur.g, cur.b, awb ? "on" : "off");
  } else {
    Serial.println("[WB] saved setting exists but register write failed after camera init");
  }
}

}  // namespace CameraWB
