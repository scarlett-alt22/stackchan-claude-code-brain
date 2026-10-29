#include "Robot.h"
#include "driver/Look.h"   //OWNER 2026-10-09 look
#include "tts/WebVoiceVoxTTS.h"
#include "tts/ElevenLabsTTS.h"
#include "tts/OpenAITTS.h"
#include "tts/UAquesTalkTTS.h"
#include "tts/ModuleLLMTTS.h"
#include "stt/CloudSpeechClient.h"
#include "stt/Whisper.h"
#include "stt/ModuleLLMASR.h"
#include "stt/ModuleLLMWhisper.h"
#include "driver/ModuleLLM.h"
#include "llm/LLMBase.h"
#include "llm/ChatGPT/ChatGPT.h"
#include "llm/ChatGPT/RealtimeChatGPT.h"
#include "llm/Gemini/GeminiLive.h"
#include "llm/ModuleLLM/ChatModuleLLM.h"
#include "llm/ModuleLLMFncl/ChatModuleLLMFncl.h"
#include "Avatar.h"
#include "driver/GifPlayer.h"   //OWNER: Clawd GIF小剧场
#include "SideLED.h"            //OWNER 2026-07-18: 说话时侧灯带呼吸

using namespace m5avatar;

extern Avatar avatar;
extern bool servo_home;

//#if defined(REALTIME_API_WITH_TTS)
// TTS非同期実行用のタスク
//
void asyncTtsStreamTask(void *arg) {
  Serial.println("TTS stream task created");
  Robot* pRt = (Robot*)arg;

  while(1){
    if(pRt->llm->getOutputTextQueueSize() != 0){
      Serial.println("TTS stream task: get text form queue");
      pRt->asyncPlaying = true;
      String ttsText = pRt->llm->getOutputText();
      pRt->tts->stream(ttsText);
      pRt->asyncPlaying = false;
    }
    delay(1);
  }
}

//#endif

Robot::Robot(StackchanExConfig& config) : m_config(config)
{
  // Servo setting
  //
#ifdef USE_SERVO
  servo = new ServoCustom();
  servo->begin(config.getServoInfo(AXIS_X)->pin, config.getServoInfo(AXIS_X)->start_degree,
              config.getServoInfo(AXIS_X)->offset,
              config.getServoInfo(AXIS_Y)->pin, config.getServoInfo(AXIS_Y)->start_degree,
              config.getServoInfo(AXIS_Y)->offset,
              (ServoType)config.getServoType(),
              &M5.In_I2C);
  servo->applySavedOrigin();   //OWNER 2026-10-04: NVS 里存过待机俯仰角就覆盖 yaml center.y
#endif

  // TakaoBase setting 
  //
  // 設定ファイルのTakaoBaseがtrueの場合に、TakaoBaseのUSBからの給電でバッテリーを充電できるようにする
  // ただし、この設定ではバッテリーからの給電／横のUSBからの給電ではサーボが動かない
  M5.Power.setExtOutput(!config.getUseTakaoBase());

  // AI service setting
  //
#if defined(REALTIME_API)
  //LLM setting
  initRtLLM(config);

  #if defined(REALTIME_API_WITH_TTS)
    initTTS(config);
    invokeAsyncTtsStreamTask();
  #endif

#else //REALTIME_API

#if defined(USE_LLM_MODULE)
  module_llm_param = module_llm_param_t();
#endif
  
  initLLM(config);
  initTTS(config);
  initSTT(config);

  // ModuleLLM initialize
  //
#if defined(USE_LLM_MODULE)
  module_llm_param.rxPin = config.getExConfig().moduleLLM.rxPin;
  module_llm_param.txPin = config.getExConfig().moduleLLM.txPin;
  int wakeword_type = config.getExConfig().wakeword.type;
  if(wakeword_type == WAKEWORD_TYPE_MODULE_LLM_KWS){
    module_llm_param.enableKWS = true;
    module_llm_param.wake_up_keyword = config.getExConfig().wakeword.keyword;
  }
  module_llm_setup(module_llm_param);

  // TTSがModuleLLMではなくAquesTalkの場合はTTS非同期実行用タスクを起動する
  if(!module_llm_param.enableTTS){
    invokeAsyncTtsStreamTask();
  }
#endif

#endif  //REALTIME_API

}

bool Robot::isAllOfflineService()
{
  return llm->isOfflineService && stt->isOfflineService && tts->isOfflineService;
}

void Robot::initLLM(StackchanExConfig& config){
  int llm_type = config.getExConfig().llm.type;
  api_keys_s* api_key = config.getAPISetting();

  llm_param_t llm_param;
  llm_param.api_key = api_key->ai_service;
  llm_param.llm_conf = config.getExConfig().llm;

  switch(llm_type){
  case LLM_TYPE_CHATGPT:
  case LLM_TYPE_CUSTOM_OPENAI:
    llm = new ChatGPT(llm_param);
    break;
  case LLM_TYPE_MODULE_LLM:
#if defined(USE_LLM_MODULE)
    llm = new ChatModuleLLM(llm_param);
    module_llm_param.enableLLM = true;
    module_llm_param.m5llm_config = m5_module_llm::ApiLlmSetupConfig_t(); //default setting
    if((llm_param.llm_conf.model.c_str() != nullptr)
      && (llm_param.llm_conf.model != "null")
      && (llm_param.llm_conf.model != ""))
    {
      module_llm_param.m5llm_config.model = llm_param.llm_conf.model;
    }
    module_llm_param.m5llm_config.max_token_len = 511;
#else
    Serial.println("ModuleLLM is not enabled. Please setup in platformio.ini");
    llm = nullptr;
#endif
    break;
  case LLM_TYPE_MODULE_LLM_FNCL:
#if defined(USE_LLM_MODULE)
    llm = new ChatModuleLLMFncl(llm_param);
    module_llm_param.enableLLM = true;
    module_llm_param.m5llm_config.model = "SmolLM-360M-Instruct-fncl";
    module_llm_param.m5llm_config.max_token_len = 511;
#else
    Serial.println("ModuleLLM is not enabled. Please setup in platformio.ini");
    llm = nullptr;
#endif
    break;
  default:
    Serial.printf("Error: undefined LLM type %d\n", llm_type);
    llm = nullptr;
  }
}

void Robot::initRtLLM(StackchanExConfig& config){
#if defined(REALTIME_API)
  int llm_type = config.getExConfig().llm.type;
  api_keys_s* api_key = config.getAPISetting();

  llm_param_t llm_param;
  llm_param.api_key = api_key->ai_service;
  llm_param.llm_conf = config.getExConfig().llm;

  switch(llm_type){
  case LLM_TYPE_CHATGPT:
    llm = new RealtimeChatGPT(llm_param);
    break;
  case LLM_TYPE_GEMINI:
    llm = new GeminiLive(llm_param);
    break;
  default:
    Serial.printf("Error: undefined LLM type %d\n", llm_type);
    llm = nullptr;
  }
#endif
}


void Robot::initSTT(StackchanExConfig& config){
  int stt_type = config.getExConfig().stt.type;
  api_keys_s* api_key = config.getAPISetting();

  stt_param_t stt_param;
  stt_param.api_key = api_key->stt;
  stt_param.stt_conf = config.getExConfig().stt;
  
  switch(stt_type){
  case STT_TYPE_GOOGLE:
    stt = new CloudSpeechClient(stt_param);
    break;
  case STT_TYPE_OPENAI_WHISPER:
    stt = new Whisper(stt_param);
    break;
  case STT_TYPE_MODULE_LLM_ASR:
#if defined(USE_LLM_MODULE)
    stt = new ModuleLLMASR();
    module_llm_param.enableASR = true;
#else
    Serial.println("ModuleLLM is not enabled. Please setup in platformio.ini");
    stt = nullptr;
#endif
    break;
  case STT_TYPE_MODULE_LLM_WHISPER:
#if defined(USE_LLM_MODULE)
    stt = new ModuleLLMWhisper();
    module_llm_param.enableWhisper = true;
    if((stt_param.stt_conf.model.c_str() != nullptr)
      && (stt_param.stt_conf.model != "null")
      && (stt_param.stt_conf.model != ""))
    {
      module_llm_param.whisper_model = stt_param.stt_conf.model;
    }
#else
    Serial.println("ModuleLLM is not enabled. Please setup in platformio.ini");
    stt = nullptr;
#endif
    break;
  default:
    Serial.printf("Error: undefined STT type %d\n", stt_type);
    stt = nullptr;
  }

}

void Robot::initTTS(StackchanExConfig& config){
  int tts_type = config.getExConfig().tts.type;
  api_keys_s* api_key = config.getAPISetting();

  tts_param_t tts_param;
  tts_param.api_key = api_key->tts;
  tts_param.model = config.getExConfig().tts.model;
  tts_param.voice = config.getExConfig().tts.voice;
  tts_param.customEndpoint = config.getExConfig().tts.customEndpoint;
  tts_param.customRootCA = config.getExConfig().tts.customRootCA;
  tts_param.audioFormat = config.getExConfig().tts.audioFormat;
  switch(tts_type){
  case TTS_TYPE_WEB_VOICEVOX:
    tts = new WebVoiceVoxTTS(tts_param);
    break;
  case TTS_TYPE_ELEVENLABS:
    tts = new ElevenLabsTTS(tts_param);
    break;
  case TTS_TYPE_OPENAI:
    // ★ OWNER 2026-07-12: 本家 OpenAI に投げるときだけ ChatGPT と鍵を共用する。
    // customEndpoint(= LAN 内の Mac の音声合成サーバ)を使う場合は apikey.tts を使う。
    // そちらは鍵を要求しないので通常は空 = Authorization ヘッダ自体を送らない。
    // ChatGPT/shim の鍵を平文 HTTP で LAN に流さないための分岐。
    if(config.getExConfig().tts.customEndpoint.length() == 0){
      tts_param.api_key = api_key->ai_service;    //API KeyはChatGPTと共通
    }
    tts = new OpenAITTS(tts_param);
    break;
  case TTS_TYPE_AQUESTALK:
#if defined(USE_AQUESTALK)
    tts = new UAquesTalkTTS();
#else
    Serial.println("AquesTalk is not enabled. Please define USE_AQUESTALK.");
    tts = nullptr;
#endif
    break;
  case TTS_TYPE_MODULE_LLM:
#if defined(USE_LLM_MODULE)
    tts = new ModuleLLMTTS();
    module_llm_param.enableTTS = true;
    module_llm_param.model = tts_param.model;
#else
    Serial.println("ModuleLLM is not enabled. Please setup in platformio.ini");
    tts = nullptr;
#endif
    break;
  default:
    Serial.printf("Error: undefined TTS type %d\n", tts_type);
    tts = nullptr;
  }

}

//OWNER 2026-07-13: 首字 emoji 契约。Claude 在回复开头吐一个 emoji 表情绪,这里把它剥掉
// (绝不能送进 TTS: 克隆音色服务器会把它念成怪音)并映射成答完要播的小剧场。
// 为什么不用 set_avatar_expression 那个 tool: ChatGPT.cpp 对 CUSTOM_OPENAI 端点(=VPS shim)
// 整段跳过 functions 注入,那个 tool 永远到不了 Claude 面前 —— 情绪只能走文本带内信号。
// 只吃 0xF0 打头的 4 字节 UTF-8(U+10000+ emoji 平面)及紧随的修饰符;日语假名汉字是 0xE3/0xE4
// 打头,不会被误伤 —— speech() 也被日语提醒(Scheduler)和番茄钟调用,它们的文本必须一字不动。
static const uint8_t EMOJI_HAPPY[] = {0xF0, 0x9F, 0x98, 0x8A};   // U+1F60A 😊

static bool byte_match(const String& s, int at, const uint8_t* pat, int len)
{
  if (at + len > (int)s.length()) return false;
  for (int k = 0; k < len; k++) {
    if ((uint8_t)s[at + k] != pat[k]) return false;
  }
  return true;
}

// 返回答完该播的场景(nullptr=没有可映射的情绪);text 被原地剥掉前导 emoji。
static const char* strip_leading_emoji(String& text)
{
  const int n = text.length();
  int i = 0;
  while (i < n && (uint8_t)text[i] <= ' ') i++;
  if (i >= n || (uint8_t)text[i] != 0xF0) return nullptr;   // 没有前导 emoji: 原文一字不动

  const char* scene = byte_match(text, i, EMOJI_HAPPY, 4) ? GIF_SCENE_HAPPY : nullptr;

  while (i < n) {   // 吃干净整段前导 emoji(复合 emoji 的 ZWJ/肤色/变体选择符),别把碎片留给 TTS 念
    uint8_t b = (uint8_t)text[i];
    if      (b == 0xF0 && i + 4 <= n) i += 4;   // emoji 本体
    else if (b == 0xEF && i + 3 <= n) i += 3;   // U+FE0F 变体选择符
    else if (b == 0xE2 && i + 3 <= n) i += 3;   // U+200D ZWJ
    else break;
  }
  while (i < n && (uint8_t)text[i] <= ' ') i++;
  text = text.substring(i);
  return scene;   // 吐了个白名单外的 emoji: 照样剥干净,只是不播剧场
}

void Robot::speech(String text)
{
  if(text != ""){
    //OWNER 2026-10-09 look: 回复末尾的 [[look]] 不念出来;念完再去拍照、问、念回答。
    //look 那一轮自己念回答时 Look::busy() 为真,不会再触发(防递归)。设备侧开关关着时只剥不看。
    const bool wantLook = Look::stripTags(text) && Look::enabled() && !Look::busy();
    const char* moodScene = strip_leading_emoji(text);   //OWNER: 剥掉首字 emoji,拿到 Claude 自报的情绪
    if(text == "" && wantLook) text = "我看看。";          //OWNER 2026-10-09: 只给了标记没给话,也得先出个声
    if(text == "") return;                               //OWNER: 整条回复只有一个 emoji,没东西可念

    if (gif_theater_on()) gif_scene_start(GIF_SCENE_TALKING);   //OWNER: 剧场:念回复场景(回答本来就只走语音,气泡从不显示回答文本,损失的只有嘴型)
    else gif_scene_stop();   //OWNER: 无剧场回退:交还屏幕看嘴动
    servo_home = false;
    avatar.setExpression(Expression::Happy);

    sc_led_breathe(true);    //OWNER: 说话中→侧灯呼吸(stream 阻塞整段播放)
    tts->stream(text);
    sc_led_breathe(false);   //OWNER: 说完→侧灯熄灭

    avatar.setExpression(Expression::Neutral);
    servo_home = true;
    if (wantLook) {               //OWNER 2026-10-09: 要看就直接去看,情绪剧场留给看完的回答
      Look::lookAndAnswer();
    } else if (moodScene != nullptr) {   //OWNER: 答完按 Claude 自报的情绪播小剧场(旧版是 1/5 骰子,与他说了什么无关)
      gif_scene_play_once(moodScene);
    }
  }
}

void Robot::invokeAsyncTtsStreamTask(void)
{
  asyncPlaying = false;

  xTaskCreate(asyncTtsStreamTask, /* Function to implement the task */
            "asyncTtsStreamTask", /* Name of the task */
            5*1024,               /* Stack size in words */
            this,                 /* Task input parameter */
            2,                    /* Priority of the task */
            NULL);                /* Task handle. */
}

String Robot::listen()
{
  String ret = stt->speech_to_text();
  return ret;
}

void Robot::chat(String text, const char *base64_buf)
{
  llm->chat(text, base64_buf);
}
