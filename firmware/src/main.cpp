#include <Arduino.h>
//#include <FS.h>
#include <SD.h>
#include <SPIFFS.h>
#include "share/Version.h"
#include "share/Mutex.h"
#include "share/SDUtil.h"
#include "share/SpiffsMirror.h" //OWNER 07-28: SD->SPIFFS 配置/语音镜像, SD 退出开机关键路径
#include "share/DefaultParams.h"
#include <M5Unified.h>
#include <nvs.h>
#include <Preferences.h>
#include <Avatar.h>
#include <faces/CatFace.h>
#include "StackchanExConfig.h" 
#include "wifi_dual.h"  // ★ OWNER 2026-10-04: 双 Wi-Fi 选网纯逻辑
#include "Robot.h"
#include "mod/ModManager.h"
#include "mod/ModBase.h"
#include "mod/AiStackChan/AiStackChanMod.h"
#include "mod/AiStackChan/RealtimeAiMod.h"
#include "mod/Pomodoro/PomodoroMod.h"
#include "mod/PhotoFrame/PhotoFrameMod.h"
#include "mod/StatusMonitor/StatusMonitorMod.h"
#include "mod/VolumeSetting/VolumeSettingMod.h"
#include "mod/QRdisplay/QRdisplayMod.h"
#include "mod/EspNowRemote/EspNowRemoteMod.h"

#include "driver/PlayMP3.h"   //lipSync
#include "driver/TapDetect.h"
#include "driver/MotionReaction.h"
#include "driver/VoiceClip.h"
#include "driver/GifPlayer.h" //OWNER: 剧场模式(开机即小剧场)
#include "driver/SdSelfTest.h" //OWNER: SD 写入自检(串口 "sdtest", 不碰网络)

#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WiFiMulti.h>  // ★ OWNER 2026-10-04: 双 Wi-Fi（Arduino-ESP32 自带）
#include <ArduinoJson.h>
#include "SpiRamJsonDocument.h"
#include <ESP8266FtpServer.h>

#include "llm/ChatGPT/ChatGPT.h"
#include "llm/ChatGPT/FunctionCall.h"
#include "llm/ChatHistory.h"
#include "llm/Gemini/GeminiLive.h"

#include "WebAPI.h"

#if defined( ENABLE_CAMERA )
#include "driver/Camera.h"
#endif    //ENABLE_CAMERA

#include "driver/WatchDog.h"
#include "SDUpdater.h"
#include "DebugTools.h"

#if defined(USE_AUDIO_MODULE)
#include "driver/M5AudioModule.h"
#endif

StackchanExConfig system_config;
WiFiMulti wifiMulti;  // ★ OWNER 2026-10-04: 双 Wi-Fi，开机时填好两个 AP，loop() 断线重连也用它
Robot* robot;
bool isOffline = false;


// NTP接続情報　NTP connection information.
const char* NTPSRV      = "ntp.jst.mfeed.ad.jp";    // NTPサーバーアドレス NTP server address.
const long  GMT_OFFSET  = 9 * 3600;                 // GMT-TOKYO(時差９時間）9 hours time difference.
const int   DAYLIGHT_OFFSET = 0;                    // サマータイム設定なし No daylight saving time setting

//bool servo_home = false;
bool servo_home = true;
volatile bool espnow_remote_servo_override = false;

using namespace m5avatar;
Avatar avatar;
Face* customFace;
const Expression expressions_table[] = {
  Expression::Neutral,
  Expression::Happy,
  Expression::Sleepy,
  Expression::Doubt,
  Expression::Sad,
  Expression::Angry
};

FtpServer ftpSrv;   //set #define FTP_DEBUG in ESP8266FtpServer.h to see ftp verbose on serial


void lipSync(void *args)
{
  float gazeX, gazeY;
  int level = 0;
  DriveContext *ctx = (DriveContext *)args;
  Avatar *avatar = ctx->getAvatar();
  for (;;)
  {
#ifdef REALTIME_API
#ifdef REALTIME_API_WITH_TTS
    level = robot->tts->getLevel();
#else
    level = ((RealtimeLLMBase*)(robot->llm))->getAudioLevel();
#endif
#else
    level = robot->tts->getLevel();
#endif
    if(level<100) level = 0;
    if(level > 15000)
    {
      level = 15000;
    }
    float open = (float)level/15000.0;
    avatar->setMouthOpenRatio(open);
    avatar->getGaze(&gazeY, &gazeX);
    avatar->setRotation(gazeX * 5);
    delay(100);
  }
}


void servo(void *args)
{
  float gazeX, gazeY;
  DriveContext *ctx = (DriveContext *)args;
  Avatar *avatar = ctx->getAvatar();
  for (;;)
  {
#ifdef USE_SERVO
    if(espnow_remote_servo_override)
    {
      delay(100);
      continue;
    }

    if(!servo_home)
    {
      avatar->getGaze(&gazeY, &gazeX);
      robot->servo->moveTo((int)(15.0 * gazeX), (int)(10.0 * gazeY));
    } else {
      robot->servo->moveToOrigin();
    }
#endif
    delay(5000);
  }
}

void battery_check(void *args) {
  DriveContext *ctx = (DriveContext *)args;
  Avatar *avatar = ctx->getAvatar();
  for (;;)
  {
    int32_t batteryLevel = M5.Power.getBatteryLevel();
    if (batteryLevel < 0) batteryLevel = 0;
    if (batteryLevel > 100) batteryLevel = 100;

    // Always show the battery, including externally powered 0% readings and
    // a fully charged 100%.  The status only changes the charging glyph.
    avatar->setBatteryIcon(true);
    avatar->setBatteryStatus(M5.Power.isCharging(), batteryLevel);
    delay(60000);
  }
}

bool Wifi_connection_check() {
  unsigned long start_millis = millis();

  // 前回接続時情報で接続する
  while (WiFi.status() != WL_CONNECTED) {
    M5.Display.print(".");
    Serial.print(".");
    delay(1000);
    // 5秒以上接続できなかったら抜ける
    if ( 5000 < (millis() - start_millis) ) {
      //break;
      return false;
    }
  }
  return true;
}

// ★ OWNER 2026-10-04: 双 Wi-Fi 连接。把 yaml 里配好的 AP（wifi 必选、wifi2 可选）
// 交给 WiFiMulti，由它扫描后连信号最好的一个（比如家里 Wi-Fi 和手机热点同时在时
// 自动选强的）。总等待 5 秒：原来是 前回 5s + SD 设置 5s = 10s，现在是
// 前回 5s + 这里 5s = 10s，不超过原来的上限。
// 返回 true = 连上了（串口只打 SSID，不打密码）；false = 没配或都没连上。
//
// OWNER 2026-10-04 复核：登记 AP 拆成单独一步，开机读完配置就调，不管走哪条路连上。
// Muse 原稿只在"前回设定失败"时才 addAP——而开机最常见的是前回设定直接连上，
// 那样 wifiMulti 里一个 AP 都没有，loop() 的断线重连每 10 秒白扫一次、永远连不回去
// （WiFiMulti::run 只在 APlist 里找匹配）。
static int s_wifiApCount = 0;
static std::vector<std::string> s_wifiApNames;  // 只用于诊断打印"扫没扫到"，不存密码
void wifi_register_aps(wifi_s* wifi_info, wifi_s* wifi2_info) {
  WifiAp w1{ wifi_info->ssid.c_str(), wifi_info->password.c_str() };
  WifiAp w2{ wifi2_info->ssid.c_str(), wifi2_info->password.c_str() };
  std::vector<WifiAp> aps = wifi_build_ap_list(w1, w2);  // 去重、跳过空的
  for (auto& ap : aps) {
    if (wifiMulti.addAP(ap.ssid.c_str(), ap.password.c_str())) {  // addAP 内部 strdup
      s_wifiApCount++;
      s_wifiApNames.push_back(ap.ssid);
      Serial.printf("[WiFi] AP registered: %s\n", ap.ssid.c_str());
    }
  }
}

// OWNER 2026-10-04 晚: 连不上时报告原因——扫到几个网、每个配置的 AP 扫没扫到。
// 只打配置里那两个名字(不打邻居的网名)。10-04 晚到家连不上家里 Wi-Fi、串口全哑,
// 就是因为失败时什么都不打,只能猜。
void wifi_scan_report() {
  int n = WiFi.scanNetworks();
  Serial.printf("[WiFi] scan: %d networks visible\n", n);
  for (auto& name : s_wifiApNames) {
    bool seen = false;
    int32_t rssi = 0;
    for (int i = 0; i < n; i++) {
      if (WiFi.SSID(i) == String(name.c_str())) { seen = true; rssi = WiFi.RSSI(i); break; }
    }
    if (seen) Serial.printf("[WiFi]   %s: seen, RSSI=%d\n", name.c_str(), (int)rssi);
    else      Serial.printf("[WiFi]   %s: NOT seen\n", name.c_str());
  }
  WiFi.scanDelete();
}

// OWNER 2026-10-04 晚: 预算 5s -> 20s。Muse 稿的 5s 是它自定的"总等待不超过原来 10s",
// 没有实际依据; WiFiMulti::run() 一次就要 扫描(2~3s)+连接(最多 5s),
// 前回设定刚失败时 STA 还在后台重试旧网,第一次扫描常直接失败、改走异步扫描,5s 根本不够。
// 10-04 晚实测(家里路由器,前回设定刚失败后): 第 1 次 run() "AP not found"、第 2 次 "Connecting Failed",
// 第 21s 才连上 —— 20s 只是擦边过,放到 40s。没信号时多等 20s 再进离线,离线后 loop() 仍每 20s 重试。
static const uint32_t WIFI_MULTI_BOOT_BUDGET_MS = 40000;

bool wifi_connect_multi() {
  if (s_wifiApCount == 0) {
    return false;
  }
  Serial.printf("[WiFi] trying %d configured AP(s)...\n", s_wifiApCount);
  unsigned long start_millis = millis();
  while (millis() - start_millis < WIFI_MULTI_BOOT_BUDGET_MS) {
    if (wifiMulti.run() == WL_CONNECTED) {
      Serial.printf("[WiFi] connected SSID: %s\n", WiFi.SSID().c_str());
      return true;
    }
    delay(500);
  }
  return WiFi.status() == WL_CONNECTED;
}

bool WifiSmartConfig() {
#if defined(USE_LLM_MODULE)
  // LLMモジュール使用時は普通はオフラインが前提のため、Smart Config待ちはしない
  return false;
#else
  unsigned long start_millis = millis();
  WiFi.mode(WIFI_STA);
  WiFi.beginSmartConfig();
  M5.Display.println("Waiting for SmartConfig");
  Serial.println("Waiting for SmartConfig");
  while (!WiFi.smartConfigDone()) {
    delay(1000);
    M5.Display.print("#");
    Serial.print("#");
    // 30秒以上接続できなかったら抜ける
    if ( 30000 < millis() - start_millis) {
      Serial.println("");
      //Serial.println("Reset");
      //ESP.restart();
      return false;
    }
  }
  return true;
#endif
}

void time_sync(const char* ntpsrv, long gmt_offset, int daylight_offset) {
  struct tm timeInfo; 
  char buf[60];

  configTime(gmt_offset, daylight_offset, ntpsrv);          // NTPサーバと同期

  if (getLocalTime(&timeInfo)) {                            // timeinfoに現在時刻を格納
    Serial.print("NTP : ");                                 // シリアルモニターに表示
    Serial.println(ntpsrv);                                 // シリアルモニターに表示

    sprintf(buf, "%04d-%02d-%02d %02d:%02d:%02d\n",     // 表示内容の編集
    timeInfo.tm_year + 1900, timeInfo.tm_mon + 1, timeInfo.tm_mday,
    timeInfo.tm_hour, timeInfo.tm_min, timeInfo.tm_sec);

    Serial.println(buf);                                    // シリアルモニターに表示
  }
  else {
    Serial.print("NTP Sync Error ");                        // シリアルモニターに表示
  }
}



ModBase* init_mod(void)
{
  ModBase* mod;
  if(!isOffline || robot->isAllOfflineService()){
#if defined(REALTIME_API)
    add_mod(new RealtimeAiMod(isOffline));
#else
    add_mod(new AiStackChanMod(isOffline));
#endif
  }
  add_mod(new StatusMonitorMod());
  add_mod(new VolumeSettingMod());
  //add_mod(new EspNowRemoteMod());
  //add_mod(new PomodoroMod(isOffline));
  //add_mod(new PhotoFrameMod(isOffline));
  //add_mod(new QRdisplayMod());
  mod = get_current_mod();
  mod->init();
  return mod;
}


void sw_tone()
{
  enterMutexAudio();
  M5.Mic.end();
  M5.Speaker.begin();
  delay(300);     // AtomS3Rはこのdelayがないと鳴らないときがある
  M5.Speaker.tone(1000, 100);
  delay(500);

  M5.Speaker.end();
  M5.Mic.begin();
  exitMutexAudio();
}
  
void alarm_tone()
{
  enterMutexAudio();
  M5.Mic.end();
  M5.Speaker.begin();

  for(int i=0; i<5; i++){
    M5.Speaker.tone(1200, 50);
    delay(100);
    M5.Speaker.tone(1200, 50);
    delay(100);
    M5.Speaker.tone(1200, 50);
    delay(1000);  
  }

  M5.Speaker.end();
  M5.Mic.begin();
  exitMutexAudio();
}

void init_mic_spk()
{
#if defined(USE_AUDIO_MODULE)
  initAudioModule();
#endif

  {
    auto micConfig = M5.Mic.config();
    //micConfig.stereo = false;
    micConfig.sample_rate = 16000;
#if defined(USE_AUDIO_MODULE)
    micConfig.pin_data_in = SYS_I2S_DIN_PIN;
    micConfig.pin_bck = SYS_I2S_SCLK_PIN;
    micConfig.pin_mck = SYS_I2S_MCLK_PIN;
    micConfig.pin_ws = SYS_I2S_LRCK_PIN;
#endif
    M5.Mic.config(micConfig);
  }
  M5.Mic.begin();

  { /// custom setting
    auto spk_cfg = M5.Speaker.config();
    /// Increasing the sample_rate will improve the sound quality instead of increasing the CPU load.
    spk_cfg.sample_rate = 64000; // default:64000 (64kHz)  e.g. 48000 , 50000 , 80000 , 96000 , 100000 , 128000 , 144000 , 192000 , 200000
    spk_cfg.task_pinned_core = APP_CPU_NUM;

#if defined(USE_AUDIO_MODULE)
    spk_cfg.pin_data_out = SYS_I2S_DOUT_PIN;
    spk_cfg.pin_bck = SYS_I2S_SCLK_PIN;
    spk_cfg.pin_mck = SYS_I2S_MCLK_PIN;
    spk_cfg.pin_ws = SYS_I2S_LRCK_PIN;
#endif
    M5.Speaker.config(spk_cfg);
  }
  //M5.Speaker.begin();
}

// ★ OWNER 2026-07-18: 头顶两侧灯带(12×WS2812C，挂机身 PY32L020 IO 扩展器 0x6F，非 GPIO)——
//   空闲熄灭 + 说话/思考/摸头时橙色呼吸。实现与硬件说明见 SideLED.h / SideLED.cpp。
#include "SideLED.h"

void setup()
{
  /// シリアル出力のログレベルを VERBOSEに設定
  //M5.Log.setLogLevel(m5::log_target_serial, ESP_LOG_VERBOSE);

  auto cfg = M5.config();

#if defined(ARDUINO_M5STACK_CORES3)
  cfg.internal_imu = true;
#endif

#if defined(ARDUINO_M5STACK_ATOMS3R)
  cfg.internal_spk = false;
  cfg.internal_mic = false;
  cfg.external_speaker.atomic_echo = true;
#endif
  cfg.serial_baudrate = 115200;   //M5Unified 0.1.17からデフォルトが0になったため設定
  M5.begin(cfg);

  ///// Debug /////
#if 0
  check_board();
  Wire.begin(); 
  i2c_scan(Wire);
  Wire1.begin(); 
  i2c_scan(Wire1);
#endif
  /////////////////

#if defined(ARDUINO_M5STACK_ATOMS3R)
  M5.Lcd.setTextSize(2);
  M5.Lcd.printf("Ver.%s\n", FW_VERSION);
#else
  M5.Lcd.setFont(&fonts::lgfxJapanGothic_20);
  M5.Lcd.setTextSize(1);
  M5.Lcd.println("AI Stack-chan Ex [・＿・]");
  M5.Lcd.printf("Firmware Version: %s\n", FW_VERSION);
#endif

  initMutex();

#if defined(ENABLE_SD_UPDATER)
  // ***** for SD-Updater *********************
  SDU_lobby("AiStackChanEx");
  // ******************************************
#endif

  //auto brightness = M5.Display.getBrightness();
  //Serial.printf("Brightness: %d\n", brightness);

  init_mic_spk();

  /// settings
#if defined(ARDUINO_M5STACK_ATOMS3R)
  const bool config_ready = SPIFFS.begin();
  if (config_ready) {
    // この関数ですべてのYAMLファイル(Basic, Secret, Extend)を読み込む
    system_config.loadConfig(SPIFFS, "/SC_ExConfig.yaml", 2048,
                                     "/SC_SecConfig.yaml", 2048,
                                     "/SC_BasicConfig.yaml", 2048);
    // ★ OWNER 2026-10-04: 读可选的 wifi2 段（secret yaml 与 loadConfig 同路径）。
    system_config.loadWifi2Setting(SPIFFS, "/SC_SecConfig.yaml");
  }
#else
  //★ OWNER 2026-07-26: SD SPI 时钟 25MHz -> 10MHz。
  // 起因: 串口反复刷 `sd_diskio.cpp:621 ff_sd_status(): Check status failed`,
  // 表现是目录偶尔扫成空/少读文件(实测 pet_back 4 个只读到 3 个)。
  // **在刚格式化的新卡上照样复现**, 所以不是文件系统坏, 是这条读路本身不稳 ——
  // CoreS3 的 SD 走 SPI 且跟屏幕共总线, 25MHz 偏激进。
  // 降速够不够用(算过): 10MHz 实测 600~800KB/s; 最重的需求是加载一个 90KB 的 GIF
  // (约 150ms, 每 20 秒才一次), 语音播放只吃 48KB/s。余量很大, 不影响任何体验。
  //★ OWNER 2026-07-27: 改走 sd_begin_cores3()(share/SDUtil.h)。
  // 以前这里直接 SD.begin(GPIO_NUM_4, SPI, ...) 而**全仓库没有一次 SPI.begin()** ——
  // CoreS3 的 TFT 拿 MISO 当 DC 用, 不先按 SD 引脚初始化 SPI 就等于把总线状态
  // 交给屏幕驱动决定。详见 SDUtil.h 里的注释和修复前基线数据。
  //★ OWNER 2026-07-28: SD 不再是开机的人质。
  // 以前: SD 挂载失败 -> "System reset after 5 seconds" -> ESP.restart() ->
  // 还是失败 -> **无限重启循环**, WiFi(凭据在卡上)/SPIFFS 素材/触摸全部陪葬。
  // 07-28 那张卡第三次 FAT 报废, 整机就是这么变砖的(串口 1.8s 一轮:
  // f_mount failed: There is no valid FAT volume + RTC_SW_CPU_RST)。现在:
  //   SD 好 -> 照常从 SD 读配置, 顺手把配置+CA+触摸语音镜像进 SPIFFS(SpiffsMirror)
  //   SD 坏 -> 用上次的镜像从 SPIFFS 起机, 机器人照活(SD 沦为可选外设)
  // ExConfig 的镜像放 SPIFFS 根("/SC_ExConfig.yaml", 31 字符上限装不下
  // /app/... 全路径); Basic/Sec 用 loadConfig 默认参数的 /yaml/... 路径,
  // SPIFFS 下原样放得下, 跟 SD 同名同路。
  const bool sd_ok = sd_begin_cores3(10000000);
  const bool config_ready = true;   // CoreS3 从此永远开机, 死循环那条路废除
  if (sd_ok) {
    // この関数ですべてのYAMLファイル(Basic, Secret, Extend)を読み込む
    system_config.loadConfig(SD, "/app/AiStackChanEx/SC_ExConfig.yaml");
    // ★ OWNER 2026-10-04: 读可选的 wifi2 段（secret yaml 与 loadConfig 同路径）。
    system_config.loadWifi2Setting(SD, "/yaml/SC_SecConfig.yaml");
    spiffs_mirror_from_sd();
  } else {
    SPIFFS.begin(true);
    Serial.println("[Mirror] SD 挂不上, 从 SPIFFS 镜像起机");
    M5.Lcd.print("SD unavailable - boot from SPIFFS mirror\n");
    // 镜像也缺文件时 loadConfig 内部自己会落默认值, 不会崩——最坏也是
    // 离线模式活着, 比无限重启强
    system_config.loadConfig(SPIFFS, "/SC_ExConfig.yaml");
    // ★ OWNER 2026-10-04: 读可选的 wifi2 段（SPIFFS 镜像里的 secret yaml）。
    system_config.loadWifi2Setting(SPIFFS, "/yaml/SC_SecConfig.yaml");
  }
#endif

  if (config_ready) {
    // Wifi設定読み込み
    wifi_s* wifi_info = system_config.getWiFiSetting();
    Serial.printf("\nSSID: %s\n",wifi_info->ssid.c_str());
    // OWNER 2026-10-04: 删掉原来的 Serial.printf("Key: %s") ——它把 Wi-Fi 密码打到串口，
    // 而串口日志会被读进对话记录。
    // ★ OWNER 2026-10-04: 双 Wi-Fi —— 可选的第二个 AP（手机热点）。
    // 没配 wifi2 时，下面走 WiFiMulti 也只会试 wifi，和原来行为一样。
    wifi_s* wifi2_info = system_config.getWiFi2Setting();
    wifi_register_aps(wifi_info, wifi2_info);  // 先登记，loop() 断线重连要用

    // 前回設定で接続
    Serial.println("Connecting to WiFi");
    WiFi.disconnect();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    WiFi.begin();
    if(Wifi_connection_check()){
      Serial.println("Successfully connected to Wi-Fi using the previous settings.");
      // 只打 SSID，不打密码。
      Serial.printf("[WiFi] connected SSID: %s\n", WiFi.SSID().c_str());
    }else{
      // 前回設定での接続に失敗。SDカード設定（wifi / wifi2）による接続にトライ。
      Serial.println("The previous WiFi connection failed. Attempting to connect using the SD card settings.");
      if(!wifi_connect_multi()){
        if (s_wifiApCount > 0) {
          // OWNER 2026-10-04 晚: 配了 AP 却都没连上(比如在没信号的地方开机)。
          // 不进 SmartConfig(那是给完全没配置的机器用手机 App 配网的,干等 30s 没意义),
          // 直接离线,由 loop() 每 20s 重试,连上就重启进在线模式。
          Serial.println("All configured WiFi APs failed. Offline; loop() will keep retrying.");
          wifi_scan_report();
          isOffline = true;
        } else if(!WifiSmartConfig()){
          // Smart Config失敗。オフラインモード。
          Serial.println("Smart Config failed. Running in offline mode.");
          isOffline = true;
        }
      }
    }

    if(!isOffline){
      Serial.println(WiFi.localIP());
      M5.Lcd.println(WiFi.localIP());
      // ★ OWNER 2026-07-24: 打印 Wi-Fi 链路质量。起因 = 设备侧 ping 20~40% 丢包、
      // RTT 均值 511ms/峰值 1320ms(同时段 Mac→路由器 0% 丢包 / 3-16ms，所以坏的是
      // 设备这一跳)，导致 TTS 音频流欠载、设备 http.end() 断开 —— Mac 侧表现为
      // BrokenPipe，听感 = 只说了一个字后面没了。RSSI/信道 是判断「信号弱」还是
      // 「信道拥挤」的第一手数据，WebAPI 没有这个接口、CORE_DEBUG_LEVEL=0 也不打，
      // 只能从这里打出来。
      // ★ OWNER 2026-07-24: 关掉 Wi-Fi 省电(modem sleep)。
      // 上面那行打印逼出来的结论: RSSI -41 dBm(极强，贴着路由器才 -30) 却 20~40%
      // 丢包、RTT 均值 678ms / 峰值 2986ms。信号这么强而延迟抖成这样，不是信号问题，
      // 是 ESP32 默认的 WIFI_PS_MIN_MODEM 在 DTIM 间隔之间关射频省电的典型表现。
      // TTS 是 48KB/s 的实时流，这种抖动直接打穿设备的音频缓冲 -> 阻塞读 500ms 超时
      // -> http.end() 永久断开 -> 听感就是"只说了一个字，后面没了"。
      // 代价: 功耗上升约 100~150mA（这机器常插电，可接受）。回退: 删掉这一行。
      WiFi.setSleep(false);
      Serial.printf("[WiFi] RSSI=%d dBm  ch=%d  BSSID=%s  sleep=%d\n",
                    WiFi.RSSI(), WiFi.channel(), WiFi.BSSIDstr().c_str(), (int)WiFi.getSleep());
      delay(1000);

      //Webサーバ設定
      init_web_server();
      //FTPサーバ設定（SPIFFS用）
      //★ OWNER 2026-10-05: 上游自带的 FTP 服务器关掉了。它用固定账号读写 SPIFFS，而 SPIFFS 镜像里有
      //  含密钥的 SC_SecConfig.yaml；同一 Wi-Fi 下谁都能拿。素材上传走 HTTP 接口。
      // Serial.println("FTP server started");
      // M5.Lcd.println("FTP server started");

      //時刻同期
      time_sync(NTPSRV, GMT_OFFSET, DAYLIGHT_OFFSET);
    }else{
      M5.Lcd.print("Can't connect to WiFi. Start offline mode.\n");
    }

    robot = new Robot(system_config);

    //SD.end();
  } else {
    // 只有 ATOMS3R 的 SPIFFS 挂不上才会走到这; CoreS3 的 config_ready 恒 true,
    // SD 失败改走 SPIFFS 镜像(见上), "失败就重启"的死循环已废除。
    M5.Lcd.print("Failed to load settings. System reset after 5 seconds.");
    delay(5000);
    ESP.restart();
    //WiFi.begin();
  }
  
  mp3_init();

  //mod設定
  init_mod();

#if defined(ARDUINO_M5STACK_ATOMS3R)
#if defined(CAT_FACE)
  customFace = new CatFace();
  avatar.setFace(customFace);
#endif
  avatar.setScale(0.5);
  avatar.setPosition(-56, -96);
  avatar.init();
#else
  //avatar.init();
  avatar.init(16);

  // ★ 橙色系定制 (OWNER 2026-07-02): 橙底 + 大黑眼睛 + 无嘴 + 奶白气泡
  //   需要 16-bit 色深(上面的 init(16))才能让气泡用第三种颜色(奶白)
  {
    ColorPalette cp;
    cp.set(COLOR_PRIMARY,            M5.Display.color565(  0,   0,   0));  // 眼/眉: 黑
    cp.set(COLOR_SECONDARY,          M5.Display.color565(  0,   0,   0));  // 次要: 黑
    cp.set(COLOR_BACKGROUND,         M5.Display.color565(250, 243, 224));  // 脸底: 奶白(OWNER 2026-07-24, 原橙色改配gif剧场同色系,摄像头模式退回classic avatar时不再突兀)
    cp.set(COLOR_BALLOON_BACKGROUND, M5.Display.color565(250, 243, 224));  // 气泡底: 奶白
    cp.set(COLOR_BALLOON_FOREGROUND, M5.Display.color565(  0,   0,   0));  // 气泡字: 黑
    avatar.setColorPalette(cp);
    avatar.getFace()->setMouth(new Mouth(0, 0, 0, 0));   // 去掉嘴
    avatar.getFace()->setRightEye(new Eye(20, false));   // 眼睛放大(默认8)
    avatar.getFace()->setLeftEye(new Eye(20, true));
  }
#endif

  avatar.addTask(lipSync, "lipSync", 2048, 2);
  avatar.addTask(servo, "servo", 2048);
  avatar.addTask(battery_check, "battery_check", 2048);
  avatar.setSpeechFont(&fonts::efontJA_16);

  Serial.printf("Speaker volume (yaml): %d\n", system_config.getExConfig().audio.speaker_volume);
  if(0 != system_config.getExConfig().audio.speaker_volume){
    robot->spk_volume = system_config.getExConfig().audio.speaker_volume;
  }else{
    robot->spk_volume = DEFAULT_SPEAKER_VOLUME;
  }
  {
    //OWNER 2026-10-04: GET /volume?v=N&save=1 存过音量就覆盖 yaml/默认值(GIF 剧场占屏,VolumeSettingMod 点不到)
    Preferences p;
    if (p.begin("audio", true)) {
      if (p.isKey("spkvol")) {
        robot->spk_volume = p.getUChar("spkvol", robot->spk_volume);
        Serial.printf("Speaker volume (NVS): %d\n", robot->spk_volume);
      }
      p.end();
    }
  }
  Serial.printf("Speaker volume (set): %d\n", robot->spk_volume);
  M5.Speaker.setVolume(robot->spk_volume);

#if defined(ENABLE_CAMERA)
  //OWNER 2026-10-04(code-to-muse 20261004-071145-stackchan-camera): 按需拍照——
  //开机不再 camera_init(),也不开预览子窗。摄像头只在点右下角拍照按钮时
  //init→抓帧→deinit,平时完全关闭(07-25 关摄像头的三个理由不再成立)。
#endif

#if defined(ENABLE_TAP_DETECT)
  invokeDoubleTapDetectTask();
#endif

  MotionReaction::begin();

  //init_watchdog();

  //OWNER: 剧场模式:开机即小剧场(橙脸退役为无SD回退)。必须在 setup() 末尾——
  //          mod->init() 那次太早(avatar.init(16) 还没跑);SD 探不到 idle 素材=不开,整机回退原版行为
  gif_theater_begin();

  //★ OWNER 2026-07-26: 触摸反馈的语音目录。放在这里是因为要等 SD 挂好
  //  (上一行 gif_theater_begin() 已经在读 SD 了)。设备侧建目录不是多余的:
  //  esp8266FTPServer 的 MKD 硬编码失败,从 Mac 推语音之前目录必须已经存在。
  VoiceClip::begin();

  //ヒープメモリ残量確認(デバッグ用)
  check_heap_free_size();
  check_heap_largest_free_block();

  // ★ OWNER 2026-07-18: 启动侧灯带(空闲熄灭 + 说话/思考/摸头呼吸,详见 SideLED.h/.cpp)
  sc_led_begin();

}



void loop()
{
  //get_elapsed_time_micro("loop() start");
  // ★ OWNER 2026-07-24: 每 30 秒打一次 RSSI。开机那一次只是个快照 ——
  // 断流是**间歇**发生的，要看的是链路抖不抖、什么时候掉，所以必须持续采样。
  static uint32_t s_lastRssiLog = 0;
  if (!isOffline && (millis() - s_lastRssiLog >= 30000)) {
    s_lastRssiLog = millis();
    Serial.printf("[WiFi] RSSI=%d dBm  ch=%d  sleep=%d\n",
                  WiFi.RSSI(), WiFi.channel(), (int)WiFi.getSleep());
  }
  // ★ OWNER 2026-10-04: 运行中断线自动重连。原来 loop() 里没有任何重连逻辑，
  // 掉线就一直离线到下次重启。现在每 10 秒检查一次：掉线就让 WiFiMulti 把配好的
  // 两个 AP 都试一遍（扫描后连信号好的，哪个在连哪个）；连上只打 SSID 不打密码。
  static uint32_t s_lastWifiReconnect = 0;
  if (!isOffline && s_wifiApCount > 0 && (millis() - s_lastWifiReconnect >= 10000)) {
    s_lastWifiReconnect = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WiFi] disconnected, reconnecting...");
      if (wifiMulti.run(5000) == WL_CONNECTED) {
        Serial.printf("[WiFi] reconnected SSID: %s\n", WiFi.SSID().c_str());
      } else {
        Serial.println("[WiFi] reconnect failed, will retry");
      }
    }
  }
  // ★ OWNER 2026-10-04 晚: 开机没连上网 -> 原来是永久离线(mods 按离线初始化,
  // 上面的掉线重连也跳过 isOffline)。10-04 晚主人带它回家、热点已关,开机找不到网就一直哑着。
  // 现在: 配了 AP 就每 20s 再试一次,连上立刻 ESP.restart() —— 重启后"前回设定"就是刚连上的网,
  // 秒连,mods 按在线模式干净地初始化。不在这里热切换在线状态(mods 已按离线建好,半路改不干净)。
  static uint32_t s_lastOfflineRetry = 0;
  if (isOffline && s_wifiApCount > 0 && (millis() - s_lastOfflineRetry >= 20000)) {
    s_lastOfflineRetry = millis();
    Serial.println("[WiFi] offline, retrying configured APs...");
    if (wifiMulti.run(8000) == WL_CONNECTED) {
      Serial.printf("[WiFi] connected SSID: %s -> restarting to come up online\n", WiFi.SSID().c_str());
      delay(300);
      ESP.restart();
    } else {
      wifi_scan_report();
    }
  }
  //★ OWNER 2026-07-27: 串口诊断命令入口。目前只有 "sdtest"(见 SdSelfTest.h)。
  // 刻意走串口而不是 HTTP —— 要测的就是"不碰网络时 SD 还坏不坏"。
  if (Serial.available()) {
    //🔴 readStringUntil 默认等 1000ms: 一个杂散字节就能把主循环连同 GIF、
    // 触摸采样一起卡住一秒。收紧到 50ms —— 整行命令早就在缓冲里了。
    Serial.setTimeout(50);
    const String cmd = Serial.readStringUntil('\n');
    if (cmd.length() > 0 && !sd_self_test_handle_command(cmd) &&
        !spiffs_mirror_handle_command(cmd)) {
      Serial.printf("[cmd] 不认得: %s\n", cmd.c_str());
    }
  }

  M5.update();
  //get_elapsed_time_micro("M5.update time");
  ModBase* mod = get_current_mod();
  mod->idle();
  //get_elapsed_time_micro("Mod idle time");

  if (M5.BtnA.wasPressed())
  {
    mod->btnA_pressed();
  }

  if (M5.BtnA.pressedFor(2000))
  {
    mod->btnA_longPressed();
  }

  if (M5.BtnB.wasPressed())
  {
    mod->btnB_pressed();
  }

  if (M5.BtnB.pressedFor(2000))
  {
    mod->btnB_longPressed();
  }

  if (M5.BtnC.wasPressed())
  {
    mod->btnC_pressed();
  }

#if defined(ARDUINO_M5STACK_Core2) || defined( ARDUINO_M5STACK_CORES3 )
  static bool coreS3WakeEnrollHoldHandled = false;
  auto count = M5.Touch.getCount();
  if (count)
  {
    auto t = M5.Touch.getDetail();
    if (t.wasPressed())
    {
      mod->display_touched(t.x, t.y);
    }

#if defined(ARDUINO_M5STACK_CORES3) && defined(ENABLE_WAKEWORD)
    // ★ OWNER 2026-07-25: 唤醒词取消后这段登录手势就是空动作(btnB_longPressed
    // 整个函数体都在 ENABLE_WAKEWORD 里), 一并关掉, 免得长按左下角还打印日志。
    // StackChan CoreS3 has no A/B/C hardware keys. Reuse the existing B-hold
    // enrollment action only after a deliberate two-second hold in the otherwise
    // unused lower-center area; normal touch-to-talk and swipe areas stay unchanged.
    constexpr int16_t kWakeEnrollXMin = 100;
    constexpr int16_t kWakeEnrollXMax = 220;
    constexpr int16_t kWakeEnrollYMin = 170;
    constexpr int16_t kWakeEnrollYMax = 239;
    const bool wakeEnrollStartInRegion =
        t.base_x >= kWakeEnrollXMin && t.base_x <= kWakeEnrollXMax &&
        t.base_y >= kWakeEnrollYMin && t.base_y <= kWakeEnrollYMax;
    if (!coreS3WakeEnrollHoldHandled && t.isPressed() && wakeEnrollStartInRegion &&
        millis() - t.base_msec >= 2000) {
      coreS3WakeEnrollHoldHandled = true;
      Serial.println("[WakeWord] lower-center hold; starting enrollment");
      mod->btnB_longPressed();
    }
#endif

    if (t.wasFlicked())
    {
      int16_t dx = t.distanceX();
      int16_t dy = t.distanceY();

      // detect flick right/left
      if(abs(dx) >= abs(dy))
      {
        if(dx > 0){
          //Serial.println("Right flicked");
          change_mod(true);
        }
        else{
          //Serial.println("Left flicked");
          change_mod();
        }
      }
    }
  }
  else
  {
    coreS3WakeEnrollHoldHandled = false;
  }
#endif

#if defined(ENABLE_TAP_DETECT)
  if(doubleTapDetected){
    Serial.println("loop(): Double tap detected");
    mod->doubleTapped(detectedAcc[0], detectedAcc[1], detectedAcc[2]);
    doubleTapDetected = false;
  }

  // Modで重い処理をしている場合はダブルタップ検出を停止する
  if(mod->isBusy()){
    stopDoubleTapDetectTask();
  }else{
    resumeDoubleTapDetectTask();
  }
#endif

  if (!mod->isBusy() && MotionReaction::update()) {
    mod->motionDetected();
  }
  //get_elapsed_time_micro("Callback process time");

  if(!isOffline){
    web_server_handle_client();
    // ftpSrv.handleFTP();   //★ OWNER 2026-10-05: FTP 已关，见 setup()
  }

  //get_elapsed_time_micro("Web event process time");
  
  //reset_watchdog();
}
