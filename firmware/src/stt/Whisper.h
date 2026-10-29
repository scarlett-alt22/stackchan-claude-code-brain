#ifndef _Whisper_H
#define _Whisper_H
#include "STTBase.h"
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include "driver/AudioWhisper.h"

class Whisper : public STTBase {
  WiFiClient plainClient;
  WiFiClientSecure secureClient;
  WiFiClient* client;
  bool _useTLS;
  // ★ OWNER 2026-07-05: 从 stt.customEndpoint 解析出的目标(默认 api.openai.com)
  String _host;
  int    _port;
  String _path;
  const char* _rootCA;

  bool resolveEndpoint(IPAddress& address);
  bool recoverWiFiForDNS();
  bool connectEndpoint();

public:
  Whisper(stt_param_t param);
  ~Whisper();
  String Transcribe(AudioWhisper* audio);
  virtual String speech_to_text();
};

#endif // _Whisper_H
