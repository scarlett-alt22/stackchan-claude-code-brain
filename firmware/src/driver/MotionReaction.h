#ifndef _MOTION_REACTION_H
#define _MOTION_REACTION_H

namespace MotionReaction {

// Uses the CoreS3 internal IMU when present. Other boards are a no-op.
void begin(void);
bool update(void);
bool isAvailable(void);

//★ OWNER 2026-10-05: 摇晃反应开关。主人带着机器人走路时它老说"要被晃散架了"，
//  先关掉。存在 NVS "motion"/"shake"；没存过＝关（主人要的默认值）。
//  update() 照常采样（重力滤波保持热着），关着时只是不报 shake。
bool shakeEnabled(void);
void setShakeEnabled(bool on);

}  // namespace MotionReaction

#endif  // _MOTION_REACTION_H
