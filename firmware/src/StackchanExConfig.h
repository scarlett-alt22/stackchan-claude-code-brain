#ifndef __STACKCHAN_EX_CONFIG_H__
#define __STACKCHAN_EX_CONFIG_H__

#include <Stackchan_system_config.h>
#include "llm/ChatGPT/MCPClient.h"


#if defined(ARDUINO_M5STACK_Core2)
  // #define DEFAULT_SERVO_PIN_X 13  //Core2 PORT C
  // #define DEFAULT_SERVO_PIN_Y 14
  #define DEFAULT_SERVO_PIN_X 33  //Core2 PORT A
  #define DEFAULT_SERVO_PIN_Y 32
#elif defined( ARDUINO_M5STACK_FIRE )
  #define DEFAULT_SERVO_PIN_X 21
  #define DEFAULT_SERVO_PIN_Y 22
#elif defined( ARDUINO_M5Stack_Core_ESP32 )
  #define DEFAULT_SERVO_PIN_X 21
  #define DEFAULT_SERVO_PIN_Y 22
#elif defined( ARDUINO_M5STACK_CORES3 )
  #define DEFAULT_SERVO_PIN_X 18  //CoreS3 PORT C
  #define DEFAULT_SERVO_PIN_Y 17
#elif defined( ARDUINO_M5STACK_ATOMS3R )
  #define DEFAULT_SERVO_PIN_X 0   //非対応
  #define DEFAULT_SERVO_PIN_Y 0
#endif


//
// AI機能設定 
//
#define LLM_TYPE_CHATGPT                0
#define LLM_TYPE_MODULE_LLM             1
#define LLM_TYPE_MODULE_LLM_FNCL        2
#define LLM_TYPE_GEMINI                 3
#define LLM_TYPE_CUSTOM_OPENAI          4
#define LLM_N_MCP_SERVERS_MAX           10

#define TTS_TYPE_WEB_VOICEVOX           0
#define TTS_TYPE_ELEVENLABS             1
#define TTS_TYPE_OPENAI                 2
#define TTS_TYPE_AQUESTALK              3
#define TTS_TYPE_MODULE_LLM             4

#define STT_TYPE_GOOGLE                 0
#define STT_TYPE_OPENAI_WHISPER         1
#define STT_TYPE_MODULE_LLM_ASR         2
#define STT_TYPE_MODULE_LLM_WHISPER     3

#define WAKEWORD_TYPE_SIMPLEVOX         0
#define WAKEWORD_TYPE_MODULE_LLM_KWS    1


typedef struct LLMConf {
    int type;
    String model = "";           // Model name. ChatGPT (LLM_TYPE_CHATGPT): overrides the default gpt-4o when set, falls back to gpt-4o when blank. Custom OpenAI (LLM_TYPE_CUSTOM_OPENAI): required, requests are refused when blank. Inert for LLM types that do not use the ChatGPT class.
    int nMcpServers;
    mcp_server_s mcpServer[LLM_N_MCP_SERVERS_MAX];
    bool enableMemory;
    String customEndpoint = "";  // Optional URL for LLM_TYPE_CUSTOM_OPENAI. Empty falls back to api.openai.com. http:// works without a CA; https:// requires customRootCAFile and is refused at send time if the CA is missing.
    String customRootCA = "";    // PEM bundle of the trusted root CA(s), loaded from llm.customRootCAFile (single path on SD, e.g. "/customRootCA.pem") and/or llm.customRootCAFiles (a list of paths). Multiple files are concatenated into this one buffer; mbedTLS trusts every -----BEGIN CERTIFICATE----- ... -----END CERTIFICATE----- block as a root. Only needed for https:// customEndpoint.
} llm_s;

typedef struct TTSConf {
    int type;
    String model;
    String voice;
    // ★ OWNER 2026-07-12: 让 TTS(OpenAI 分支)也能像 LLM/STT 一样指向任意 OpenAI 兼容
    // 合成端点(例: Mac 上的 Claude 克隆音色 server, LAN 内明文 http)。镜像 llm_s / stt_s。
    String customEndpoint = "";  // 空 = 默认 api.openai.com。https:// 需配 customRootCAFile
    String customRootCA = "";    // 从 tts.customRootCAFile 加载的 PEM bundle。http:// 不需要
    String audioFormat = "mp3";  // 服务端回的音频格式。设备解码器: "mp3" 或 "wav"
} tts_s;

typedef struct STTConf {
    int type;
    String model;
    // ★ OWNER 2026-07-05: 让 STT(Whisper 分支)也能像 LLM 一样指向任意 OpenAI 兼容
    // 转写端点(例: 硅基流动 SenseVoice, 国内直连可达)。镜像上面 llm_s 的那套。
    String customEndpoint = "";  // 空 = 默认 api.openai.com。https:// 需配 customRootCAFile
    String customRootCA = "";    // 从 stt.customRootCAFile / customRootCAFiles 加载的 PEM bundle
    String language = "";        // 空 = 省略 language 字段(让 SenseVoice 自判语言); 否则如 "zh"/"ja"
} stt_s;

typedef struct WakeWordConf {
    int type;
    String keyword;
} wakeword_s;

typedef struct AudioConf {
    uint8_t speaker_volume;
} audio_s;

typedef struct ModuleLLMConf {
    int8_t rxPin;
    int8_t txPin;
} moduleLLM_s;

typedef struct ExConfig {
    llm_s llm;
    tts_s tts;
    stt_s stt;
    wakeword_s wakeword;
    audio_s audio;
    moduleLLM_s moduleLLM;
} ex_config_s;


// StackchanSystemConfigを継承します。
class StackchanExConfig : public StackchanSystemConfig
{
    protected:
        bool USE_SERVO_ST;      //servo.txtの1行目のパラメータの格納先（このソフトでは未使用）。
        ex_config_s _ex_parameters;
        fs::FS* _extend_fs = nullptr;  // filesystem in use during loadExtendConfig (SD on most boards, SPIFFS on AtomS3R)
        // ★ OWNER 2026-10-04: 第二个 Wi-Fi（手机热点）。复用第三方头里的 wifi_s
        // 结构体定义，不改第三方库。ssid 为空 = 没配 wifi2，和原来行为完全一样。
        wifi_s _wifi2;


    public:
        StackchanExConfig();
        ~StackchanExConfig();

        void loadExtendConfig(fs::FS& fs, const char *yaml_filename, uint32_t yaml_size) override;
        void setExtendSettings(DynamicJsonDocument doc) override;
        void printExtParameters(void) override;

        ex_config_s getExConfig() { return _ex_parameters; }

        // ★ OWNER 2026-10-04: 在 loadConfig() 之后调用，只解析 secret yaml 里
        // 可选的 wifi2: { ssid, password }。第三方库的 setSecretConfig 不是虚函数，
        // 这里把 secret yaml 再读一遍是最不侵入的办法（小文件，开机多一次读）。
        void loadWifi2Setting(fs::FS& fs, const char* secret_yaml_filename);
        wifi_s* getWiFi2Setting() { return &_wifi2; }
        void setExConfig(ex_config_s config) { _ex_parameters = config; } 

        void basicConfigNotFoundCallback(void) override;
        void secretConfigNotFoundCallback(void) override;
        void extendConfigNotFoundCallback(void);

    private:
        // Read a PEM file from _extend_fs and append it to `target` (e.g. llm.customRootCA
        // or stt.customRootCA), building one bundle that may hold several concatenated roots.
        // ★ OWNER 2026-07-05: 加了 target 参数, 从只服务 LLM 泛化成 LLM/STT 共用。
        void appendRootCAFromFile(const String& ca_path, String& target);

};


#endif
