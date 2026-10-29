#ifndef _STT_BASE_H
#define _STT_BASE_H

#include <Arduino.h>
#include "StackchanExConfig.h"

struct stt_param_t
{
  String api_key;
  stt_s stt_conf;
};

// ★ 2026-10-28(code-to-muse 20261028-125756): speech_to_text() 只返回文本,
// 失败原因原来全丢了 —— "" 既可能是"没连上服务端"(Network),也可能是
// "服务端应答 200 但没听清"(DidntHear)。离线语音只该在 Network 时出声,
// 所以把最近一次调用的失败类型留在基类:具体客户端在返回前写,调用方读。
// 未实现的客户端(如 ModuleLLM 系)恒为 None,行为与从前一致。
enum class STTErrorKind {
  None,      // 最近一次调用成功,或还没调用过
  Network,   // 连接失败 / 响应超时 / HTTP 状态不是 200
  DidntHear, // HTTP 200 但转写文本为空:服务是通的,只是没听清
};

class STTBase{
protected:
    stt_param_t param;
    STTErrorKind _lastErrorKind = STTErrorKind::None;

public:
    bool isOfflineService;
    STTErrorKind lastErrorKind() const { return _lastErrorKind; }
    
    STTBase() {};
    STTBase(stt_param_t param) : param{param}, isOfflineService{false} {};
    virtual String speech_to_text() = 0;

};



#endif //_STT_BASE_H