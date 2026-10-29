#include <Arduino.h>
#include <deque>
#include <SD.h>
#include <SPIFFS.h>
#include "mod/ModManager.h"
#include "AiStackChanMod.h"
#include <Avatar.h>
#include "Robot.h"
#include "llm/ChatGPT/ChatGPT.h"
#include "llm/ChatGPT/FunctionCall.h"
#include "SideLED.h"   //OWNER 2026-07-18: 摸头时侧灯带呼吸
#include "share/Mutex.h"  //OWNER 2026-10-04: 拍照提示音用 enterMutexAudio/exitMutexAudio
#include "driver/PlayMP3.h"
#include "driver/WakeWord.h"
#include "driver/ModuleLLM.h"
#include <WiFiClientSecure.h>
#include "Scheduler.h"
#include "MySchedule.h"
#include "share/SDUtil.h"
#if defined( ENABLE_CAMERA )
#include "driver/Camera.h"
#endif
#include "driver/AudioWhisper.h"       //speechToText
#include "stt/Whisper.h"               //speechToText
#include "driver/Audio.h"              //speechToText
#include "stt/CloudSpeechClient.h"     //speechToText
#include "rootCA/rootCACertificate.h"  //speechToText
#include "rootCA/rootCAgoogle.h"       //speechToText
#include "driver/Audio.h"              //speechToText
#include "driver/HeadTouchSensor.h"
#include "driver/GifPlayer.h"          //OWNER: Clawd GIF小剧场
#include "driver/VoiceClip.h"          //OWNER 2026-07-26: 触摸反馈的预合成语音

using namespace m5avatar;

#if defined(ENABLE_WAKEWORD)
bool wakeword_is_enable = false;
#endif

/// 外部参照 ///
extern Avatar avatar;
extern bool servo_home;
//extern bool wakeword_is_enable;
extern void sw_tone();
extern void alarm_tone();
///////////////


static void report_batt_level(){
  char buff[100];
  int level = M5.Power.getBatteryLevel();
#if defined(ENABLE_WAKEWORD)
  mode = 0;
#endif
  if(M5.Power.isCharging())
    sprintf(buff,"充電中、バッテリーのレベルは%d％です。",level);
  else
    sprintf(buff,"バッテリーのレベルは%d％です。",level);
  avatar.setExpression(Expression::Happy);
#if defined(ENABLE_WAKEWORD)
  mode = 0; 
#endif
  robot->speech(String(buff));
  delay(1000);
  avatar.setExpression(Expression::Neutral);
}


static void STT_ChatGPT(const char *base64_buf = NULL) {
  if (gif_theater_on()) gif_scene_start(GIF_SCENE_LISTENING);   //OWNER: 剧场:切"听候"场景(目录缺素材=保持上一场景,提示音仍是开录信号)
  else gif_scene_stop();   //OWNER: 无剧场回退:交还屏幕给脸
  bool prev_servo_home = servo_home;
#ifdef USE_SERVO
  servo_home = true;
#endif

  avatar.setExpression(Expression::Happy);
  avatar.setSpeechText("御用でしょうか？");

  String ret = robot->listen();
  avatar.setSpeechText("");

#ifdef USE_SERVO
  //servo_home = prev_servo_home;
  servo_home = false;
#endif
  Serial.println("音声認識終了");
  Serial.println("音声認識結果");
  if(ret != "") {
    Serial.println(ret);
    robot->chat(ret, base64_buf);
    avatar.setSpeechText("");
    avatar.setExpression(Expression::Neutral);
    servo_home = true;
  } else {
    Serial.println("音声認識失敗");
    if (gif_theater_on()) {
      gif_scene_play_once(GIF_SCENE_ERROR);   //OWNER: 剧场:没听清→error小剧场(下面的气泡字在剧场里不可见,保留作无SD回退)
    }
    avatar.setExpression(Expression::Sad);
    avatar.setSpeechText("聞き取れませんでした");
    if (!gif_theater_on()) delay(2000);   //OWNER: 剧场里 error 已播够时长,不再定格干等
    avatar.setSpeechText("");
    avatar.setExpression(Expression::Neutral);
    // ★ 2026-10-28: 上面的处理对两种失败一视同仁(error GIF + 没听清气泡)。
    // 只有"连不上服务端"(Network)时再加一句内嵌离线语音说明原因;
    // "没听清"(DidntHear)与从前完全一致,不出声。限频在 playOfflineVoice 内。
    if (robot->stt != nullptr && robot->stt->lastErrorKind() == STTErrorKind::Network) {
      playOfflineVoice();
    }
    servo_home = true;
  }
}



AiStackChanMod::AiStackChanMod(bool _isOffline)
  : isOffline{_isOffline}
{
  box_servo.setupBox(80, 120, 80, 80);
  //OWNER 2026-10-04(code-to-muse 20261004-071145-stackchan-camera):
  //box_stt 恢复全宽(0,0,width,60);box_subWindow(预览开关)删除;新增右下角
  //拍照按钮 box_photo(240,170,80,70)。不重叠校验:
  //box_BtnC(280,100,40,60) y 100-160 vs box_photo y 170-240;
  //box_servo(80,120,80,80) x 80-160;box_stt y 0-60;
  //长按登录手势区(100-220,170-239) x 范围与 box_photo(240-320) 无交集。
  box_stt.setupBox(0, 0, M5.Display.width(), 60);
#if defined(ENABLE_CAMERA)
  box_photo.setupBox(240, 170, 80, 70);
#endif
  box_BtnA.setupBox(0, 100, 40, 60);
  box_BtnC.setupBox(280, 100, 40, 60);

  //SDカードのMP3ファイル（アラーム用）をSPIFFSにコピーする（SDカードだと音が途切れ途切れになるため）。
  //すでにSPIFFSにファイルがあればコピーはしない。強制的にコピー（上書き）したい場合は第2引数をtrueにする。
  //String fname = String(APP_DATA_PATH) + String(FNAME_ALARM_MP3);
  //copySDFileToSPIFFS(fname.c_str(), false);

  if(!isOffline){
    //スケジューラ設定
    init_schedule();
  }


  if(robot->m_config.getExConfig().wakeword.type == WAKEWORD_TYPE_MODULE_LLM_KWS){
#if defined(USE_LLM_MODULE)
    // Nothing to initialize here
#endif
  }
  else{
#if defined(ENABLE_WAKEWORD)
    wakeword_init();
    const int registeredWakewords = wakeword_registered_count();
    if (registeredWakewords > 0) {
      mode = 1;
      wakeword_is_enable = true;
      Serial.printf("[WakeWord] auto-enabled with %d sample(s)\n", registeredWakewords);
    } else {
      Serial.println("[WakeWord] no samples; listening remains disabled");
    }
#endif
  }

}


void AiStackChanMod::init(void)
{
  avatar.setSpeechText("AI Stack-chan");
#if defined(ENABLE_CAMERA)
  //OWNER 2026-10-04: 预览子窗已删除(isSubWindowON 恒 false),不再开 subwindow。
#endif
  //OWNER: 剧场模式重开。开机首次 init 在 avatar.init(16) 之前(main.cpp init_mod 顺序),
  //          太早不能起剧场——开机那次由 setup() 末尾的 gif_theater_begin() 负责,
  //          这里只管"从别的 mod 切回来"时把剧场找回来
  static bool s_bootInit = true;
  if (s_bootInit) s_bootInit = false;
  else gif_theater_begin();
}

void AiStackChanMod::pause(void)
{
  gif_theater_end();   //OWNER: 切走 mod 时关剧场,把屏幕还给下一个 mod 的脸
#if defined(ENABLE_CAMERA)
  //OWNER 2026-10-04: 预览子窗已删除,这里不再关 subwindow。
#endif
}


void AiStackChanMod::update(int page_no)
{

}

void AiStackChanMod::btnA_pressed(void)
{
#if defined(ARDUINO_M5STACK_ATOMS3R)
  sw_tone();
  STT_ChatGPT();
#else

#if defined(ENABLE_WAKEWORD)
  if(mode >= 0){
    sw_tone();
    if(mode == 0){
      avatar.setSpeechText("ウェイクワード有効");
      mode = 1;
      wakeword_is_enable = true;
    } else {
      avatar.setSpeechText("ウェイクワード無効");
      mode = 0;
      wakeword_is_enable = false;
    }
    delay(1000);
    avatar.setSpeechText("");
  }
#endif  //ENABLE_WAKEWORD

#endif  //ARDUINO_M5STACK_ATOMS3R
}


void AiStackChanMod::btnB_longPressed(void)
{
#if defined(ENABLE_WAKEWORD)
  M5.Mic.end();
  M5.Speaker.tone(1000, 100);
  delay(500);
  M5.Speaker.tone(600, 100);
  delay(1000);
  M5.Speaker.end();
  M5.Mic.begin();
  wakeword_is_enable = false; //wakeword 無効
  mode = -1;
#ifdef USE_SERVO
    servo_home = true;
    delay(500);
#endif
  avatar.setSpeechText("ウェイクワード登録開始");
#endif
}

void AiStackChanMod::btnC_pressed(void)
{
  static bool isQrDrawing = false;
  if(!isQrDrawing){
    avatar.setSpeechText("");
    String url = String("http://") + WiFi.localIP().toString();
    avatar.updateSubWindowQrcode(url);
    avatar.set_isSubWindowEnable(true);
    isQrDrawing = true;
  }else{
    avatar.set_isSubWindowEnable(false);
    isQrDrawing = false;
  }
}

#if defined(ENABLE_CAMERA)
//OWNER 2026-10-04(code-to-muse 20261004-071145-stackchan-camera): 右下角拍照按钮。
//快门/成功/失败提示音和 /v1/photos 地址推导 2026-10-09 挪进 driver/Camera.cpp(look 共用)。
static void photo_button_pressed()
{
  Serial.printf("[photo] t=%lu pressed\n", millis());   //诊断 2026-10-04
  camera_shutter_tone();          //先响快门,不等拍照结果
  //OWNER 2026-10-04 真机: 侧灯闪烁不能放在拍照前——侧灯任务呼吸时每 33ms 写一次内部 I2C,
  //和摄像头 SCCB 抢总线,实测 SCCB_Write 超时(ret 263)、探测失败。挪到拍完、总线还回来之后。

  //OWNER 2026-10-04: Muse 草稿在这里用 robot->asyncPlaying 判断"正在说话就跳过"。
  //真机发现它永远为 true——那个成员只在 RealtimeAiMod 的 invokeAsyncTtsStreamTask()
  //里初始化,本 mod 从不初始化(未初始化内存)。而本 mod 里说话是阻塞主循环的,
  //说话期间根本走不到 display_touched,所以不需要这个判断。

  uint8_t* jpg = nullptr;
  size_t jpg_len = 0;
  if (!camera_snap_jpeg(&jpg, &jpg_len)) {
    Serial.println("[photo] snap failed");
    camera_tone(330, 250);
    return;
  }
  sc_led_breathe_hold(600);      //拍到了:侧灯闪一下(此时摄像头已 deinit、I2C 已还给 M5)

  bool ok = false;
  String photoUrl = camera_photo_url(robot->m_config.getExConfig().llm.customEndpoint);
  if (photoUrl.length() == 0) {
    Serial.printf("[photo] cannot derive /v1/photos from customEndpoint: %s\n",
                  robot->m_config.getExConfig().llm.customEndpoint.c_str());
  } else {
    String apiKey = robot->m_config.getAPISetting()->ai_service;  //和 LLM 请求同一个 key
    //OWNER 2026-10-04(旅行): 端点走 https 时用和聊天同一份 CA。
    String rootCA = robot->m_config.getExConfig().llm.customRootCA;
    ok = camera_upload_photo(photoUrl.c_str(), apiKey.c_str(), rootCA.c_str(), jpg, jpg_len);
  }
  Serial.printf("[photo] bytes=%u uploaded=%d\n", (unsigned)jpg_len, (int)ok);
  free(jpg);
  if (ok) {
    camera_tone(1760, 150);   //成功:一声高音
  } else {
    camera_tone(330, 250);    //失败:一声低音,原因已打串口
  }
}
#endif  //ENABLE_CAMERA

void AiStackChanMod::display_touched(int16_t x, int16_t y)
{
  if (box_stt.contain(x, y))
  {
    sw_tone();
    //OWNER 2026-10-04: 顶部触摸只做语音提问,不再拍照、不再往对话里塞图
    //("带图提问"那一路已删除;拍照走右下角 box_photo,照片只存 Mac 不进对话)。
    STT_ChatGPT();
  }
#ifdef USE_SERVO
  if (box_servo.contain(x, y))
  {
    //servo_home = !servo_home;
    //sw_tone();
  }
#endif
  if (box_BtnA.contain(x, y))
  {
    //OWNER 2026-10-04: 原来 ENABLE_CAMERA 下这里切 isSilentMode(日文提示+阻塞 2 秒)。
    //isSilentMode 只给 ENABLE_FACE_DETECT 用,而人脸检测是关的——切了没有任何效果,
    //只会在摄像头为拍照按钮重新打开后,让左侧这块误触时卡 2 秒。保持摄像头关闭时的空操作。
    //sw_tone();
  }
  if (box_BtnC.contain(x, y))
  {
    btnC_pressed();
  }
#if defined(ENABLE_CAMERA)
  if (box_photo.contain(x, y))
  {
    //OWNER 2026-10-04: 右下角拍照按钮。只是一个触摸区,不画图标
    //(屏幕被 GIF 剧场接管,画了也会被盖掉)。
    photo_button_pressed();
  }
#endif //ENABLE_CAMERA

}

void AiStackChanMod::doubleTapped(float ax, float ay, float az)
{
  Serial.printf("Mod double tapped. ax=%.3f ay=%.3f az=%.3f\n", ax, ay, az);
#if defined(ARDUINO_M5STACK_ATOMS3R)
  sw_tone();
  STT_ChatGPT();
#endif
}

void AiStackChanMod::motionDetected(void)
{
  Serial.println("[MotionReaction] dizzy reaction");
  sc_led_breathe_hold(2200);
  //★ OWNER 2026-07-26: 被摇晃时也说一句。放在 GIF 前面——GIF 是阻塞播完才返回的,
  //  排在它后面就变成"摇完两秒才出声"。
  VoiceClip::play(VOICE_CLIP_SHAKE);
  if (gif_theater_on()) {
    if (gif_scene_play_once(GIF_SCENE_DIZZY)) {
      return;
    }
    // Avatar drawing is paused by theatre mode, so a missing scene needs a
    // GIF fallback rather than an expression-only fallback.
    Serial.println("[MotionReaction] dizzy GIF missing; using error scene");
    if (gif_scene_play_once(GIF_SCENE_ERROR)) {
      return;
    }
    gif_theater_end();
  }
  motionDizzyUntilMs = millis() + 2200;
  motionDizzyActive = true;
  avatar.setExpression(Expression::Doubt);
}

void AiStackChanMod::idle(void)
{
  //OWNER: 剧场模式:任何一次性场景结束(happy/error播完)或交互流程走完,都自动回 idle
  //          小剧场——屏幕永远有动画。启动失败(SD被拔了之类)则退出剧场,交还橙脸保底
  if (gif_theater_on() && !gif_scene_is_playing()) {
    if (!gif_scene_start(GIF_SCENE_IDLE)) gif_theater_end();
  }
  //★ OWNER 2026-07-28: 白脸自愈。以前开机 gif_theater_begin() 失败(坏模式开机
  // SD 扫不动)= 整轮开机白脸到底, 一次机会都不再给。改成低频重试: 素材上了 SPIFFS
  // pak 之后基本走不到这里, 留着兜"只有 SD 素材"的回退路线的底。真没素材(没插卡
  // 也没推 pak)也只是每 15s 一次短扫, 不打扰交互。
  else if (!gif_theater_on()) {
    static uint32_t s_lastRetryMs = 0;
    const uint32_t nowRetryMs = millis();
    if (nowRetryMs - s_lastRetryMs >= 15000) {
      s_lastRetryMs = nowRetryMs;
      gif_theater_begin();
    }
  }

  /// Face detect ///
  //OWNER 2026-07-24: ENABLE_FACE_DETECT無効時、この区画の唯一の効果は
  //idle()毎150msでカメラを空撮りしてすぐ捨てるだけ(isFaceDetectedは誰にも
  //使われない)。撮影自体がI2C/DMA/CPUを食い、音声出力が詰まる原因になった
  //(実機で確認)。プレビューは今はbox_subWindowタップ時の単発撮影に一本化した
  //ので、ここの定期撮影はFACE_DETECT専用にしてFACE_DETECT無効時は完全に
  //コンパイルアウトする。
#if defined(ENABLE_CAMERA) && defined(ENABLE_FACE_DETECT)
  //顔が検出されれば音声認識を開始。
  //OWNER 2026-07-24: loop()に節流が無く毎ティック呼ばれるとカメラの実フレームレートを
  //大幅に超えてesp_camera_fb_get()を叩き続け、cam_hal DMAのVSYNC/EOFイベントキューが溢れて
  //cam_taskのスタックカナリアが発火→Double exception→再起動ループになる（真機で確認済み）。
  //150ms間隔(約6-7fps)に節流してドライバに処理の余裕を与える。
  static uint32_t lastCameraCaptureMs = 0;
  bool isFaceDetected = false;
  uint32_t nowMs = millis();
  if (isCameraReady && nowMs - lastCameraCaptureMs >= 150) {
    lastCameraCaptureMs = nowMs;
    isFaceDetected = camera_capture_and_face_detect();
  }
  if(!isSilentMode){

#if defined(ENABLE_FACE_DETECT)
    if(isFaceDetected){
      avatar.set_isSubWindowEnable(false);
      sw_tone();
      STT_ChatGPT();                              //音声認識

      // フレームバッファを読み捨てる（ｽﾀｯｸﾁｬﾝが応答した後に、過去のフレームで顔検出してしまうのを防ぐため）
      M5.In_I2C.release();
      camera_fb_t *fb = esp_camera_fb_get();
      esp_camera_fb_return(fb);
      avatar.set_isSubWindowEnable(isSubWindowON);
    }
#endif
  }
  else{
#if defined(ENABLE_FACE_DETECT)
    if(isFaceDetected){
      avatar.setExpression(Expression::Happy);
      //delay(2000);
      //avatar.setExpression(Expression::Neutral);
    }
    else{
      avatar.setExpression(Expression::Neutral);
    }
#endif
  }
#endif  //ENABLE_CAMERA

  //Wakeword
  if(robot->m_config.getExConfig().wakeword.type == WAKEWORD_TYPE_MODULE_LLM_KWS){
#if defined(USE_LLM_MODULE)
    if( check_kws_wakeup() ){
      sw_tone();
      STT_ChatGPT();
    }
#else
    Serial.println("ModuleLLM is not enabled. Please define USE_LLM_MODULE.");
    delay(1000);
#endif
  }
  else{
#if defined(ENABLE_WAKEWORD)
    if (mode == 0) { /* return; */ }
    else if (mode < 0) {
      int idx = wakeword_regist();
      if(idx >= 0){
        String text = String("ウェイクワード#") + String(idx) + String("登録終了");
        avatar.setSpeechText(text.c_str());
        delay(1000);
        avatar.setSpeechText("");
        //mode = 0;
        //wakeword_is_enable = false;
        mode = 1;
        wakeword_is_enable = true;
        Serial.printf("[WakeWord] enabled with %d sample(s)\n", wakeword_registered_count());

      }
    }
    else if (mode > 0 && wakeword_is_enable) {
      int idx = wakeword_compare();
      if( idx >= 0){
        Serial.println("wakeword_compare OK!");
        String text = String("ウェイクワード#") + String(idx);
        avatar.setSpeechText(text.c_str());
        sw_tone();
        STT_ChatGPT();
      }
    }

#if defined(ARDUINO_M5STACK_CORES3)
    // Function Callからの要求でウェイクワード有効化
    if (wakeword_enable_required)
    {
      wakeword_enable_required = false;
      btnA_pressed();
    }

    // Function Callからの要求でウェイクワード登録
    if(register_wakeword_required)
    {
      register_wakeword_required = false;
      btnB_longPressed();
    }
#endif  //defined(ARDUINO_M5STACK_CORES3)
#endif  //ENABLE_WAKEWORD
  }

  /// Alarm ///
  if(xAlarmTimer != NULL){
    TickType_t xRemainingTime;

    /* Query the period of the timer that expires. */
    xRemainingTime = xTimerGetExpiryTime( xAlarmTimer ) - xTaskGetTickCount();
    avatarText = "残り" + String(xRemainingTime / 1000) + "秒";
    avatar.setSpeechText(avatarText.c_str());
  }

  if (alarmTimerCallbacked) {
    alarmTimerCallbacked = false;
    avatar.setSpeechText("");
#if defined(ENABLE_CAMERA)
    avatar.set_isSubWindowEnable(false);
#endif    
    if(!sd_begin_cores3(10000000)) {
    //if(!SPIFFS.begin(true)){
      Serial.println("Failed to mount SD card. Use alarm tone.");
      alarm_tone();
    }
    else{
      String fname = String(APP_DATA_PATH) + String(FNAME_ALARM_MP3);
      bool result = playMP3SD(fname.c_str());
      if(!result){
        alarm_tone();
      }
    }
#if defined(ENABLE_CAMERA)
    avatar.set_isSubWindowEnable(isSubWindowON);
#endif  
  }

  //スケジューラ処理
  if(!isOffline){
    run_schedule();
  }

  updateHeadTouchExpression();
  updateMotionDizzyExpression();

}

void AiStackChanMod::updateMotionDizzyExpression(void)
{
  if (motionDizzyActive && millis() < motionDizzyUntilMs) {
    avatar.setExpression(Expression::Doubt);
    return;
  }
  if (motionDizzyActive) {
    motionDizzyActive = false;
    avatar.setExpression(Expression::Neutral);
  }
}

//★ OWNER 2026-07-26: 被撸头时的头部动作。
//
// 故意不用舵机库现成的 motion(refuse):
//   ① 语义反了——refuse 是"摇头说不",被摸头回它一个拒绝,看着像"别碰我";
//   ② 它整套阻塞约 3 秒(±20° 两个来回 + delay(1000) + 回原点),塞进每 tick 都调的
//      idle() 里会把主循环连同 GIF、触摸采样一起卡死三秒。
// 这里自己走小幅快动:顺毛 ±8°/180ms 两个来回(像蹭手),逆毛 ±12°/110ms 三下(抖一激灵)。
// moveTo() 是 ServoCustom 的相对坐标(相对 start_degree),不是舵机库 motion() 里那套
// 90/75 绝对角。整套 <1 秒,结束回中——servo 任务每 5 秒才轮询一次 moveToOrigin(),
// 不会跟它抢。
static void headNuzzle(bool backward)
{
#ifdef USE_SERVO
  if (robot == nullptr || robot->servo == nullptr) return;

  const int amp = backward ? 12 : 8;
  const uint32_t step = backward ? 110 : 180;
  const int reps = backward ? 3 : 2;

  for (int i = 0; i < reps; i++) {
    robot->servo->moveTo(+amp, 0, step);
    robot->servo->moveTo(-amp, 0, step);
  }
  robot->servo->moveTo(0, 0, step);
#endif
}

// 拍头(单拍)。声音 → GIF,不动舵机:第一版有意只让"撸头"这一路碰舵机,
// 免得舵机/灯带/喇叭三路一起上、出问题分不清是谁的(舵机和灯带共用 PY32 0x6F)。
void AiStackChanMod::reactHeadPat(void)
{
  Serial.println("[HeadTouch] pat");
  sc_led_breathe_hold(1500);
  VoiceClip::play(VOICE_CLIP_PAT);
  if (gif_theater_on()) {
    gif_scene_play_once(GIF_SCENE_HAPPY);
  }
  headTouchGrumpy = false;
  headTouchHappyUntilMs = millis() + 2000;
  headTouchHappyActive = true;
  avatar.setExpression(Expression::Happy);
}

// 撸头。顺毛 = 舒服,逆毛 = 抗议。
// 顺序是 声音 → GIF → 舵机,三件都阻塞所以只能排队:
//   - 声音排第一,因为手势是在主人手滑到一半时触发的,这时出声才跟得上手;
//   - 舵机排最后,因为这时主人的手多半已经离开头顶了——舵机先动会跟主人的手对着顶。
// ⚠️ SwipeForward/SwipeBackward 哪个是"顺毛"取决于触摸带的物理朝向,代码里看不出来。
//    真机上要是反了,把这里的 backward 取反即可(只此一处)。
void AiStackChanMod::reactHeadPet(bool backward)
{
  Serial.printf("[HeadTouch] pet %s\n", backward ? "backward(逆毛)" : "forward(顺毛)");
  sc_led_breathe_hold(3000);   //OWNER 2026-07-18: 摸头→侧灯呼吸,每次手势刷新 3 秒窗口,松手后自动熄灭

  VoiceClip::play(backward ? VOICE_CLIP_PET_BACK : VOICE_CLIP_PET);

  //OWNER: 剧场:顺毛播 happy,逆毛播 annoyed(生气)。剧场模式下 avatar 是
  //          pauseDraw 的,下面那句 setExpression 画不出来(留作无SD回退)。
  //★ 2026-07-26: 逆毛原来借的是 dizzy,但 dizzy 是"被摇晃"的反应 —— 两个动作共用
  //  一个画面等于没分开,所以另开了 annoyed 场景。annoyed 素材还没拷进卡时
  //  gif_scene_play_once 会返回 false,这里退回 dizzy,不至于逆毛一点反应都没有。
  if (gif_theater_on()) {
    if (backward) {
      if (!gif_scene_play_once(GIF_SCENE_ANNOYED)) gif_scene_play_once(GIF_SCENE_DIZZY);
    } else {
      gif_scene_play_once(GIF_SCENE_HAPPY);   //阻塞到播完(期间不采样)
    }
  }

  headNuzzle(backward);

  headTouchGrumpy = backward;
  headTouchHappyUntilMs = millis() + 3000;
  headTouchHappyActive = true;
  avatar.setExpression(backward ? Expression::Doubt : Expression::Happy);
}

void AiStackChanMod::updateHeadTouchExpression(void)
{
  HeadTouchSensor::Gesture gesture = HeadTouchSensor::update();

  //★ OWNER 2026-07-26: 拍头。拍一下 = 反应一句,拍两下 = 开始对话
  //  (替掉"摸屏幕顶部 box_stt"这个别扭的入口,box_stt 那条路保留没动)。
  //
  //  为什么第一拍要压一个 PAT_DOUBLE_WINDOW_MS 才反应,而不是立刻出声:
  //  出声是阻塞的(播完才返回),第一拍要是立刻开播,主人那第二拍正好落在播放期间——
  //  这期间根本不采样触摸,第二拍就整个丢了,"拍两下"永远触发不了。压 400ms 再反应,
  //  人感觉不出来(人的反应本来就 ~250ms),但把双拍窗口完整让了出来。
  constexpr unsigned long PAT_DOUBLE_WINDOW_MS = 400;
  const unsigned long nowMs = millis();

  if (gesture == HeadTouchSensor::Gesture::Click) {
    if (patPendingSinceMs != 0 && (nowMs - patPendingSinceMs) <= PAT_DOUBLE_WINDOW_MS) {
      patPendingSinceMs = 0;
      Serial.println("[HeadTouch] double pat -> STT");
      sw_tone();
      STT_ChatGPT();
      return;
    }
    patPendingSinceMs = nowMs;
  } else if (patPendingSinceMs != 0 && (nowMs - patPendingSinceMs) > PAT_DOUBLE_WINDOW_MS) {
    patPendingSinceMs = 0;
    reactHeadPat();
  }

  //★ OWNER 2026-10-04: 顺毛/逆毛「整个去掉」(主人嫌触摸出声太频繁)——不出声、不播 GIF、
  //  不动舵机、侧灯也不亮,撸头手势直接忽略。拍一下头/拍两下对话/摇晃不受影响。
  //  reactHeadPet() 和 pet / pet_back 两组语音文件都留着,想恢复把下面这段解开即可:
  //  if (HeadTouchSensor::isPetGesture(gesture)) {
  //    if (!headTouchHappyActive) {
  //      reactHeadPet(gesture == HeadTouchSensor::Gesture::SwipeBackward);
  //    } else {
  //      sc_led_breathe_hold(3000);
  //      headTouchHappyUntilMs = millis() + 3000;
  //    }
  //  }

  if (headTouchHappyActive && millis() < headTouchHappyUntilMs) {
    //OWNER 2026-07-26: 逆毛保持 Doubt 而不是 Happy。这条只在无 SD/退出剧场的
    //回退路径上看得见(剧场模式下 avatar 是 pauseDraw 的)。
    avatar.setExpression(headTouchGrumpy ? Expression::Doubt : Expression::Happy);
    return;
  }

  if (headTouchHappyActive) {
    headTouchHappyActive = false;
    avatar.setExpression(Expression::Neutral);
  }
}
