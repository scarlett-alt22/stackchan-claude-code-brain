#ifndef _TTS_BASE_H
#define _TTS_BASE_H

#include <Arduino.h>
#include "driver/PlayMP3.h"

struct tts_param_t
{
  String api_key;
  String model;
  String voice;
  String customEndpoint;   // 空 = 用后端自己的默认端点
  String customRootCA;     // https:// 的 customEndpoint 才需要
  String audioFormat;      // 服务端回的格式: "mp3"(默认) 或 "wav"
};


class TTSBase{
protected:
    tts_param_t param;

public:
    bool isOfflineService;
    
    TTSBase() {};
    TTSBase(tts_param_t param) : param(param), isOfflineService(false) {};
    virtual void stream(String text) = 0;
    virtual int getLevel(){ return abs(*out.getBuffer()); };

};



#endif //_TTS_BASE_H