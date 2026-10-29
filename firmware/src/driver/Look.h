#ifndef _LOOK_H
#define _LOOK_H

#include <Arduino.h>

//★ OWNER 2026-10-09 "让 Claude 看"的固件半边。
//  Claude 在回复末尾写 [[look]] → Robot::speech 念前剥掉、念完调 lookAndAnswer():
//  回正 → 快门 → 拍一张 → 带 X-StackChan-Look: 1 传 /v1/photos → 念响应里的 reply
//  → reply 记进 chatHistory。网关的 STACKCHAN_LOOK 是总闸,这里的开关是设备侧的副闸。
namespace Look {

// 开关存 NVS "look"/"on";没存过＝开(总闸在网关,网关关着时设备根本收不到标记)。
bool enabled(void);
void setEnabled(bool on);

// 剥掉所有 [[look…]] 标记(大小写不敏感、可带参数,和网关 LOOK_TAG_RE 同一规则),
// 首尾空白一并去掉。有剥到返回 true。
bool stripTags(String& text);

// look 那一轮正在进行(念回答时不能再触发 look)
bool busy(void);

// 拍照、上传、念回答。没开 ENABLE_CAMERA 时只念一句"看不了"。
void lookAndAnswer(void);

}  // namespace Look

#endif  // _LOOK_H
