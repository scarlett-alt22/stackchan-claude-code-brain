#ifndef _AI_STACKCHAN_MOD_H
#define _AI_STACKCHAN_MOD_H

#include <Arduino.h>
#include "mod/ModBase.h"

class AiStackChanMod: public ModBase{
private:
    box_t box_servo;
    box_t box_stt;
    box_t box_BtnA;
    box_t box_BtnC;
    #if defined(ENABLE_CAMERA)
    box_t box_photo;   //OWNER 2026-10-04: 右下角拍照按钮(240,170,80,70)。
                       //box_subWindow(左上角预览开关)已删除。
    #endif
    String avatarText;
    bool isOffline;
    unsigned long headTouchHappyUntilMs = 0;
    bool headTouchHappyActive = false;
    unsigned long motionDizzyUntilMs = 0;
    bool motionDizzyActive = false;
    //★ OWNER 2026-07-26: 拍头。0 = 没有待决的单拍;非 0 = 第一拍的时刻,
    //  在窗口内等第二拍(拍两下 = 开始对话)。
    unsigned long patPendingSinceMs = 0;
    bool headTouchGrumpy = false;   //逆毛:表情回退路径上用 Doubt 而不是 Happy
    void updateHeadTouchExpression(void);
    void updateMotionDizzyExpression(void);
    void reactHeadPat(void);
    void reactHeadPet(bool backward);
public:
    AiStackChanMod(bool _isOffline);

    void init(void);
    void pause(void);
    void update(int page_no);
    void btnA_pressed(void);
    void btnB_longPressed(void);
    void btnC_pressed(void);
    void display_touched(int16_t x, int16_t y);
    void doubleTapped(float ax, float ay, float az);   // 加速度センサによるダブルタップ検出のコールバック。platformio.iniで-DENABLE_TAP_DETECTを有効にしてください
    void motionDetected(void);
    void idle(void);
};


#endif  //_AI_STACKCHAN_MOD_H
