#ifndef _CAMERA_H_
#define _CAMERA_H_

#include <esp_camera.h>
#include <fb_gfx.h>
#include <vector>
#include "human_face_detect_msr01.hpp"
#include "human_face_detect_mnp01.hpp"
#define TWO_STAGE 1 /*<! 1: detect by two-stage which is more accurate but slower(with keypoints). */
                    /*<! 0: detect by one-stage which is less accurate but faster(without keypoints). */

#define FACE_COLOR_WHITE  0x00FFFFFF
#define FACE_COLOR_BLACK  0x00000000
#define FACE_COLOR_RED    0x000000FF
#define FACE_COLOR_GREEN  0x0000FF00
#define FACE_COLOR_BLUE   0x00FF0000
#define FACE_COLOR_YELLOW (FACE_COLOR_RED | FACE_COLOR_GREEN)
#define FACE_COLOR_CYAN   (FACE_COLOR_BLUE | FACE_COLOR_GREEN)
#define FACE_COLOR_PURPLE (FACE_COLOR_BLUE | FACE_COLOR_RED)

extern bool isSubWindowON;
extern bool isSilentMode;
extern bool isCameraReady;   //OWNER 2026-07-24: camera_init()失敗時にidle()から呼ばせないためのガード

extern esp_err_t camera_init(void);
extern bool camera_capture_and_face_detect(void);
extern bool camera_capture_base64(String& out);
//OWNER 2026-10-04(code-to-muse 20261004-071145-stackchan-camera): 按需拍照。
//init→扔2帧→抓一帧→frame2jpg(质量80)→deinit,返回 JPEG(调用方 free)。
//任何一步失败都把已 init 的摄像头 deinit 掉、释放内存再返回 false。
extern bool camera_snap_jpeg(uint8_t** buf, size_t* len);
//把 JPEG 原始字节 POST 到网关 /v1/photos(Content-Type: image/jpeg,
//Authorization: Bearer <api_key>)。setTimeout 15 秒。
//root_ca: https 时用的 PEM(传 llm.customRootCA);http 时忽略,可传 NULL。
//code-to-muse 20261028-174524: 失败重试 —— 同一 JPEG 缓冲最多 3 次(退避 1s/3s),
//408 以外的 4xx 和参数类拒绝不重试;最坏阻塞约 49s。详见 Camera.cpp 的注释。
extern bool camera_upload_photo(const char* url, const char* api_key, const char* root_ca,
                                const uint8_t* buf, size_t len);
//OWNER 2026-10-09 look: 同上,但带 X-StackChan-Look: 1,网关会拿这张图再问一轮窗口,
//回答放在响应 JSON 的 "reply" 里(出错时另有 "look_error")。两轮分叉要 25-30 秒,超时放到 60 秒。
//上传本身失败(连不上/非 2xx)返回 false;reply/look_error 只在返回 true 时有意义。
extern bool camera_upload_photo_look(const char* url, const char* api_key, const char* root_ca,
                                     const uint8_t* buf, size_t len,
                                     String& reply, String& look_error);
//OWNER 2026-10-09: 原来是 AiStackChanMod.cpp 里拍照按钮的 static 函数,look 也要用,挪到这里共用。
extern void camera_tone(uint16_t freq, uint16_t ms);   //抢音频锁,临时把 Mic 换成 Speaker 响一声
extern void camera_shutter_tone(void);                 //快门:两声短 2500Hz
//从 llm.customEndpoint 推 /v1/photos 地址,推不出来返回空串
extern String camera_photo_url(const String& chatEndpoint);

#endif //_CAMERA_H_