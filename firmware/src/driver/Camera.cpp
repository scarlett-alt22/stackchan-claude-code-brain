#if defined(ENABLE_CAMERA)

#include <Arduino.h>
#include <M5Unified.h>
#include <Avatar.h>
#include "Camera.h"
#include "CameraWB.h"   //code-to-muse 20261028-174524: 存过的白平衡在每次 init 后应用
#include <SPIFFS.h>
#include <base64.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>    //OWNER 2026-10-04: camera_upload_photo 用
#include <WiFiClient.h>     //OWNER 2026-10-04: camera_upload_photo 用
#include "DebugTools.h"
#include <ArduinoJson.h>   //OWNER 2026-10-09 look: 读上传响应里的 reply
#include "share/Mutex.h"   //OWNER 2026-10-09: camera_tone 用(从 AiStackChanMod.cpp 挪过来)
using namespace m5avatar;
extern Avatar avatar;


bool isSubWindowON = false;   //OWNER 2026-10-04: 预览子窗已删除,恒 false。
                               //变量保留只为 idle() 里报警块的 set_isSubWindowEnable(isSubWindowON) 能编译。
bool isSilentMode = false;
bool isCameraReady = false;

static camera_config_t camera_config = {
    .pin_pwdn     = -1,
    .pin_reset    = -1,
    .pin_xclk     = 2,
    .pin_sscb_sda = 12,
    .pin_sscb_scl = 11,

    .pin_d7 = 47,
    .pin_d6 = 48,
    .pin_d5 = 16,
    .pin_d4 = 15,
    .pin_d3 = 42,
    .pin_d2 = 41,
    .pin_d1 = 40,
    .pin_d0 = 39,

    .pin_vsync = 46,
    .pin_href  = 38,
    .pin_pclk  = 45,

    .xclk_freq_hz = 20000000,
    .ledc_timer   = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,

    .pixel_format = PIXFORMAT_RGB565,
    //.pixel_format = PIXFORMAT_JPEG,
    //.frame_size   = FRAMESIZE_QVGA,   // QVGA(320x240)
    .frame_size   = FRAMESIZE_VGA,   // VGA(640x480)
    .jpeg_quality = 0,
    //.fb_count     = 2,
    .fb_count     = 1,
    .fb_location  = CAMERA_FB_IN_PSRAM,
    .grab_mode    = CAMERA_GRAB_WHEN_EMPTY,
};

esp_err_t camera_init(void){

    //OWNER 2026-07-24: センサ電源安定待ち。esp32-cameraはcamera_probe()失敗時、
    //cam_config()未実行(タスク/キュー未生成)のcam_objに対してcam_stop()を呼んでしまい
    //assert crashする既知の実装バグがある(ライブラリ内部、アプリ側では防げない)。
    //起動直後の探測を避けて初回成功率を上げるための待ち。
    delay(300);

    //initialize the camera
    M5.In_I2C.release();
    esp_err_t err = esp_camera_init(&camera_config);
    if (err != ESP_OK) {
        Serial.printf("Camera Init Failed: 0x%x\n", err);
        M5.Display.println("Camera Init Failed");
        isCameraReady = false;
        return err;
    }

    sensor_t *s = esp_camera_sensor_get();
    s->set_hmirror(s, 0);        // 左右反転

    CameraWB::applySavedAfterInit();   //code-to-muse 20261028-174524: 没存过白平衡 = 什么都不做

    isCameraReady = true;
    return ESP_OK;
}

static void draw_face_boxes(fb_data_t *fb, std::list<dl::detect::result_t> *results, int face_id)
{
    int x, y, w, h;
    uint32_t color = FACE_COLOR_YELLOW;
    if (face_id < 0)
    {
        color = FACE_COLOR_RED;
    }
    else if (face_id > 0)
    {
        color = FACE_COLOR_GREEN;
    }
    if(fb->bytes_per_pixel == 2){
        //color = ((color >> 8) & 0xF800) | ((color >> 3) & 0x07E0) | (color & 0x001F);
        color = ((color >> 16) & 0x001F) | ((color >> 3) & 0x07E0) | ((color << 8) & 0xF800);
    }
    int i = 0;
    for (std::list<dl::detect::result_t>::iterator prediction = results->begin(); prediction != results->end(); prediction++, i++)
    {
        // rectangle box
        x = (int)prediction->box[0];
        y = (int)prediction->box[1];

        // yが負の数のときにfb_gfx_drawFastHLine()でメモリ破壊してリセットする不具合の対策
        if(y < 0){
           y = 0;
        }

        w = (int)prediction->box[2] - x + 1;
        h = (int)prediction->box[3] - y + 1;

        //Serial.printf("x:%d y:%d w:%d h:%d\n", x, y, w, h);

        if((x + w) > fb->width){
            w = fb->width - x;
        }
        if((y + h) > fb->height){
            h = fb->height - y;
        }

        //Serial.printf("x:%d y:%d w:%d h:%d\n", x, y, w, h);

        //fb_gfx_fillRect(fb, x+10, y+10, w-20, h-20, FACE_COLOR_RED);  //モザイク
        fb_gfx_drawFastHLine(fb, x, y, w, color);
        fb_gfx_drawFastHLine(fb, x, y + h - 1, w, color);
        fb_gfx_drawFastVLine(fb, x, y, h, color);
        fb_gfx_drawFastVLine(fb, x + w - 1, y, h, color);

#if TWO_STAGE
        // landmarks (left eye, mouth left, nose, right eye, mouth right)
        int x0, y0, j;
        for (j = 0; j < 10; j+=2) {
            x0 = (int)prediction->keypoint[j];
            y0 = (int)prediction->keypoint[j+1];
            fb_gfx_fillRect(fb, x0, y0, 3, 3, color);
        }
#endif
    }
}

bool camera_capture_and_face_detect(void){
  bool isDetected = false;

  //acquire a frame
  M5.In_I2C.release();
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    //Serial.println("Camera Capture Failed");
    M5.Display.println("Camera Capture Failed");
    return ESP_FAIL;
  }

#if defined(ENABLE_FACE_DETECT)
  int face_id = 0;

#if TWO_STAGE
  HumanFaceDetectMSR01 s1(0.1F, 0.5F, 10, 0.2F);
  HumanFaceDetectMNP01 s2(0.5F, 0.3F, 5);
  std::list<dl::detect::result_t> &candidates = s1.infer((uint16_t *)fb->buf, {(int)fb->height, (int)fb->width, 3});
  std::list<dl::detect::result_t> &results = s2.infer((uint16_t *)fb->buf, {(int)fb->height, (int)fb->width, 3}, candidates);
#else
  HumanFaceDetectMSR01 s1(0.3F, 0.5F, 10, 0.2F);
  std::list<dl::detect::result_t> &results = s1.infer((uint16_t *)fb->buf, {(int)fb->height, (int)fb->width, 3});
#endif


  
  if (results.size() > 0) {
    //Serial.printf("Face detected : %d\n", results.size());

    isDetected = true;

    fb_data_t rfb;
    rfb.width = fb->width;
    rfb.height = fb->height;
    rfb.data = fb->buf;
    rfb.bytes_per_pixel = 2;
    rfb.format = FB_RGB565;

    draw_face_boxes(&rfb, &results, face_id);

  }
#endif  //ENABLE_FACE_DETECT

  if(isSubWindowON){
    avatar.updateSubWindowCam565(fb->buf);
  }

  //Serial.println("\n<<< heap size before fb return >>>");  
  //check_heap_free_size();

  //return the frame buffer back to the driver for reuse
  esp_camera_fb_return(fb);

  //Serial.println("<<< heap size after fb return >>>");  
  //check_heap_free_size();

  return isDetected;
}



bool camera_capture_base64(String& out)
{
  //acquire a frame
  M5.In_I2C.release();
  camera_fb_t *fb = esp_camera_fb_get();

  if (!fb) {
    Serial.println("Camera Capture Failed");
    return false;
  }

  size_t jpg_buf_len = 0;
  uint8_t *jpg_buf   = NULL;
  int ret;
  bool jpeg_converted = frame2jpg(fb, 80, &jpg_buf, &jpg_buf_len);
  esp_camera_fb_return(fb);
  fb = NULL;
  if (!jpeg_converted) {
    Serial.println("JPEG compression failed");
    return false;
  }

  //OWNER 2026-10-04: 删掉每次拍照往 SPIFFS 写 /capture.jpg 的 debug(那个文件
  //从来没 close,SPIFFS 已经用了约 71%)。照片走网关存 Mac,不再落设备。
  out = base64::encode(jpg_buf, jpg_buf_len);

#if 0 //debug
  fdst = SPIFFS.open("/capture_base64.txt", FILE_WRITE);
  if ((ret = fdst.write((const uint8_t*)out.c_str(), out.length())) < out.length()) {
    Serial.printf("write spiffs failed: %d - %d\n", ret, out.length());
    return false;
  }
#endif

  free(jpg_buf);
  jpg_buf = NULL;
  
  return true;
}


//OWNER 2026-10-04(code-to-muse 20261004-071145-stackchan-camera): 按需拍照。
//平时摄像头完全 deinit,只有点拍照按钮时走: init → 扔掉头 2 帧(等自动曝光/白平衡)
//→ 抓一帧 → frame2jpg(质量 80) → esp_camera_deinit()。
//成功返回 true,JPEG 在 *buf(调用方负责 free),长度在 *len。
//任何一步失败:把已经 init 的摄像头 deinit 掉、释放已分配的内存,返回 false。
//注意 esp32-camera 的已知坑:init 失败时库内部 deinit 路径会 assert 崩溃
//(见上面 camera_init 的注释),所以 init 返回 !=ESP_OK 时**不要**再调 deinit。
//OWNER 2026-10-04 真机: M5.In_I2C.release() 把内部 I2C(触摸屏/头顶触摸/侧灯/电源 共用,
//GPIO12/11)让给摄像头自带的 SCCB(IDF 驱动)。旧代码开机 init 一次永不 deinit,所以从没
//还回去过;按需拍照后不还 = 头顶触摸连报 ESP_FAIL(实测 143 次)、触摸屏读到乱码误触发录音。
//M5.In_I2C 底层是 M5GFX 的寄存器级实现,不是 IDF 驱动,不能用 sccb_i2c_port 共享,只能一借一还。
static void give_i2c_back_to_m5(void)
{
  bool ok = M5.In_I2C.begin();
  Serial.printf("[snap] t=%lu In_I2C restored=%d\n", millis(), (int)ok);
}

bool camera_snap_jpeg(uint8_t** buf, size_t* len)
{
  if (buf == NULL || len == NULL) {
    return false;
  }
  *buf = NULL;
  *len = 0;

  Serial.printf("[snap] t=%lu init begin\n", millis());   //诊断 2026-10-04: 主循环卡死,逐步打点
  M5.In_I2C.release();
  esp_err_t err = esp_camera_init(&camera_config);
  Serial.printf("[snap] t=%lu init done err=0x%x\n", millis(), err);
  if (err != ESP_OK) {
    //init 失败:库内部已经走过(可能崩溃的)deinit,应用层不再调 deinit。
    Serial.printf("[snap] esp_camera_init failed: 0x%x\n", err);
    give_i2c_back_to_m5();
    return false;
  }

  //OWNER 2026-10-09: 照片是左右镜像的(10-09 look 拍的药瓶 NATROL 字是反的)。
  //每次 init 后驱动从寄存器 0x14 读回默认值,这里取反——不依赖默认是 0 还是 1。
  //放在扔帧之前,改完的头两帧正好扔掉。
  sensor_t* sensor = esp_camera_sensor_get();
  if (sensor != NULL && sensor->set_hmirror != NULL) {
    int was = sensor->status.hmirror;
    sensor->set_hmirror(sensor, !was);
    Serial.printf("[snap] hmirror %d -> %d\n", was, (int)sensor->status.hmirror);
  }

  //code-to-muse 20261028-174524: 存过的白平衡在扔帧之前应用 —— AWB 开的模式
  //正好借头两帧收敛; 没存过时 applySavedAfterInit 什么都不做, 行为和从前一致。
  CameraWB::applySavedAfterInit();

  //扔掉头 2 帧,等 GC0308 的自动曝光/白平衡稳定
  for (int i = 0; i < 2; i++) {
    camera_fb_t* warm = esp_camera_fb_get();
    Serial.printf("[snap] t=%lu warm%d %s\n", millis(), i, warm ? "ok" : "NULL");
    if (warm == NULL) {
      Serial.println("[snap] warmup frame get failed");
      esp_camera_deinit();
      give_i2c_back_to_m5();
      return false;
    }
    esp_camera_fb_return(warm);
  }

  camera_fb_t* fb = esp_camera_fb_get();
  Serial.printf("[snap] t=%lu frame %s\n", millis(), fb ? "ok" : "NULL");
  if (fb == NULL) {
    Serial.println("[snap] frame get failed");
    esp_camera_deinit();
    give_i2c_back_to_m5();
    return false;
  }

  uint8_t* jpg_buf = NULL;
  size_t jpg_len = 0;
  bool converted = frame2jpg(fb, 80, &jpg_buf, &jpg_len);
  esp_camera_fb_return(fb);
  fb = NULL;
  Serial.printf("[snap] t=%lu jpg converted=%d len=%u\n", millis(), (int)converted, (unsigned)jpg_len);
  //拍完立刻 deinit:摄像头平时不占 CPU/总线/内存(07-25 关摄像头的三个理由
  //——cam_task 抢资源、EV-EOF-OVF 刷屏——都是"常驻"才有的问题)。
  esp_camera_deinit();
  Serial.printf("[snap] t=%lu deinit done\n", millis());
  give_i2c_back_to_m5();

  if (!converted || jpg_buf == NULL || jpg_len == 0) {
    Serial.println("[snap] frame2jpg failed");
    if (jpg_buf != NULL) {
      free(jpg_buf);
    }
    return false;
  }

  *buf = jpg_buf;
  *len = jpg_len;
  return true;
}


//OWNER 2026-10-04: 把 JPEG 原始字节(不要 base64)POST 到网关 /v1/photos。
//Content-Type: image/jpeg, Authorization: Bearer <api_key>(和 LLM 请求同一个 key)。
//setTimeout 15 秒。2xx 算成功,其他一律 false(原因打串口)。
//OWNER 2026-10-04(旅行): 支持 https —— root_ca 传 llm.customRootCA(和聊天同一份 CA,
//出门时三个端点都改走 VPS 的 https)。https 但 CA 为空仍然拒绝,不 setInsecure。
//OWNER 2026-10-09 look: 拆成 upload_jpeg(),look 版多一个请求头、更长的超时、要读响应体。
//code-to-muse 20261028-174524: 加 out_code(可为 NULL),把失败原因带给调用方做重试判断:
//   >0 = HTTP 状态码; <0 = HTTPClient 传输错误码(连不上/写一半/读超时一类);
//   0 = http.begin 失败; UPLOAD_REFUSED = 还没碰网络就被拒(参数空/https 没 CA),
//   这类重试多少次都一样,调用方不要重试。
static const int UPLOAD_REFUSED = -1000;
static bool upload_jpeg(const char* url, const char* api_key, const char* root_ca,
                        const uint8_t* buf, size_t len,
                        bool look, uint16_t timeout_ms, String* response_body,
                        int* out_code)
{
  if (out_code != NULL) {
    *out_code = UPLOAD_REFUSED;
  }
  if (url == NULL || url[0] == '\0' || buf == NULL || len == 0) {
    Serial.println("[photo] upload refused: bad args");
    return false;
  }
  const bool isHttps = strncmp(url, "https://", 8) == 0;
  if (isHttps && (root_ca == NULL || root_ca[0] == '\0')) {
    Serial.println("[photo] upload refused: https without CA");
    return false;
  }

  WiFiClient plainClient;
  WiFiClientSecure secureClient;
  if (isHttps) {
    secureClient.setCACert(root_ca);
  }
  HTTPClient http;
  http.setTimeout(timeout_ms);
  if (!http.begin(isHttps ? (WiFiClient&)secureClient : plainClient, url)) {
    Serial.printf("[photo] http.begin failed: %s\n", url);
    if (out_code != NULL) {
      *out_code = 0;
    }
    return false;
  }
  http.addHeader("Content-Type", "image/jpeg");
  http.addHeader("Authorization", String("Bearer ") + (api_key ? api_key : ""));
  if (look) {
    http.addHeader("X-StackChan-Look", "1");
  }
  Serial.printf("[photo] t=%lu POST %s look=%d\n", millis(), url, (int)look);
  int code = http.POST((uint8_t*)buf, len);
  if (out_code != NULL) {
    *out_code = code;
  }
  if (response_body != NULL && code > 0) {
    *response_body = http.getString();
  }
  http.end();
  Serial.printf("[photo] t=%lu upload code=%d bytes=%u\n", millis(), code, (unsigned)len);
  return (code >= 200 && code < 300);
}

bool camera_upload_photo(const char* url, const char* api_key, const char* root_ca,
                         const uint8_t* buf, size_t len)
{
  //code-to-muse 20261028-174524: 失败重试。起因是旅行时照片出现过整段丢失:
  //网关那头收一半断掉, 设备只试一次、失败就把唯一的 JPEG 扔了。现在用同一个
  //JPEG 缓冲重发(不重拍、不换帧), 最多共 3 次尝试, 退避 1s、3s。
  //重试: begin 失败/传输错误(连不上、写一半)/408/5xx/其它非 2xx。
  //不重试: UPLOAD_REFUSED(参数/CA 问题), 和 408 以外的 4xx —— 网关已经明确
  //回了"不收", 重发同一张图不会变结果。
  //代价(主循环阻塞): 本函数在 AiStackChanMod 里同步调用, 单次尝试超时 15s,
  //最坏 3 次 + 退避 = 3*15 + 1 + 3 = 49s(再加 TLS 握手和写出时间), 比现在
  //单次最坏多堵约 34s; 成功、或第一次就 4xx 时和现在一样快。每次尝试都打
  //一行 attempt/code/bytes, 失败原因在串口里能对上网关日志的 got/expected。
  static const uint32_t backoff_ms[2] = {1000, 3000};
  for (int attempt = 0; attempt < 3; attempt++) {
    if (attempt > 0) {
      delay(backoff_ms[attempt - 1]);
    }
    int code = 0;
    bool ok = upload_jpeg(url, api_key, root_ca, buf, len, false, 15000, NULL, &code);
    Serial.printf("[photo] attempt %d/3 code=%d bytes=%u\n", attempt + 1, code, (unsigned)len);
    if (ok) {
      return true;
    }
    if (code == UPLOAD_REFUSED) {
      return false;
    }
    if (code >= 400 && code < 500 && code != 408) {
      return false;
    }
  }
  return false;
}

bool camera_upload_photo_look(const char* url, const char* api_key, const char* root_ca,
                              const uint8_t* buf, size_t len,
                              String& reply, String& look_error)
{
  reply = "";
  look_error = "";
  String body;
  if (!upload_jpeg(url, api_key, root_ca, buf, len, true, 60000, &body, NULL)) {
    return false;
  }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.printf("[look] response is not JSON: %s\n", err.c_str());
    look_error = "bad_response";
    return true;
  }
  reply = doc["reply"] | "";
  look_error = doc["look_error"] | "";
  Serial.printf("[look] reply_len=%u look_error=%s\n", (unsigned)reply.length(), look_error.c_str());
  return true;
}


//OWNER 2026-10-04(code-to-muse 20261004-071145-stackchan-camera): 拍照按钮的提示音。
//快门提示=两声短 tone(和 sw_tone() 的单声 1000Hz/100ms 区分开);成功=一声高音,失败=一声低音。
//OWNER 2026-10-09: 从 AiStackChanMod.cpp 挪过来,look 也用。
void camera_tone(uint16_t freq, uint16_t ms)
{
  enterMutexAudio();
  M5.Mic.end();
  M5.Speaker.begin();
  M5.Speaker.tone(freq, ms);
  delay(ms + 40);
  M5.Speaker.end();
  M5.Mic.begin();
  exitMutexAudio();
}

void camera_shutter_tone(void)
{
  camera_tone(2500, 70);
  delay(60);
  camera_tone(2500, 70);
}

//从 llm.customEndpoint 推 /v1/photos 地址:
//"http://YOUR_MAC_LAN_IP:8124/v1/chat/completions" → "http://YOUR_MAC_LAN_IP:8124/v1/photos"。
//IP 不写死,跟着配置走;推不出来(比如没配 customEndpoint)就返回空串,调用方按失败处理。
String camera_photo_url(const String& chatEndpoint)
{
  const String suffix = "/v1/chat/completions";
  if (chatEndpoint.length() > suffix.length() && chatEndpoint.endsWith(suffix)) {
    String url = chatEndpoint;
    url.remove(chatEndpoint.length() - suffix.length());
    url += "/v1/photos";
    return url;
  }
  return String();
}

#endif