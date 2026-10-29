#ifndef _SERVO_CUSTOM_H
#define _SERVO_CUSTOM_H

#include <Stackchan_servo.h>

class ServoCustom : public StackchanSERVO {
public:
    ServoCustom(){};
    void moveToOrigin();
    void moveTo(int degX, int degY);
    void moveTo(int degX, int degY, uint32_t millis_for_move);

    //★ OWNER 2026-10-04: 待机俯仰角(Y 原点)可运行期改、存 NVS。
    // yaml center.y 仍是出厂值;NVS 里有存值时开机覆盖它(见 applySavedOrigin)。
    // SCS 角度域里 Y 越小越抬头,硬极限 70(抬)~170(低),这里夹在 80~160。
    static const int ORIGIN_Y_MIN = 80;
    static const int ORIGIN_Y_MAX = 160;
    int  originY() const { return _init_param.servo[AXIS_Y].start_degree; }
    int  yamlOriginY() const { return _yaml_origin_y; }
    bool hasSavedOriginY() const;
    int  setOriginY(int degY, bool persist);   // 返回夹过之后实际生效的值,立刻转过去
    void clearSavedOriginY();                  // 删存值并回到 yaml 的 center.y
    void applySavedOrigin();                   // begin() 之后调一次

private:
    int _yaml_origin_y = -1;

};

#endif  //_SERVO_CUSTOM_H
