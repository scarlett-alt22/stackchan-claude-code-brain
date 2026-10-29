#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include "rootCA/rootCACertificate.h"
#include "tts/OpenAITTS.h"
#include "SpiRamJsonDocument.h"
#include "driver/PlayMP3.h"
#include "AudioFileSourceHttpsPostStream.h"
#include "llm/ChatGPT/ChatGPT.h"    // for API key

String OPENAITTS_MODEL = "tts-1";
String OPENAITTS_VOICE = "alloy";

static String dataTemplate = 
"{"
    "\"input\": \"dummy\","
    "\"model\": \"tts-1\","
    "\"voice\": \"alloy\","
    "\"speed\": 1,"
    "\"response_format\": \"mp3\""
"}";


void OpenAITTS::stream(String text){
  // ★ OWNER 2026-07-12: customEndpoint が設定されていれば、そちらへ投げる。
  // 用途は Mac 上の Claude 音色 server (LAN 内・平文 http・鍵なし・WAV を返す)。
  // 未設定なら従来どおり api.openai.com。llm/stt の customEndpoint と同じ流儀。
  const bool useCustom = (param.customEndpoint.length() > 0);
  String tts_url = useCustom ? param.customEndpoint
                             : String("https://api.openai.com/v1/audio/speech");

  // http:// のときは CA 不要(nullptr)。https:// のときだけ CA を渡す。
  const char* root_ca = nullptr;
  if (tts_url.startsWith("https://")) {
    root_ca = useCustom ? param.customRootCA.c_str() : root_ca_openai;
  }

  String format = param.audioFormat;
  if (format != "wav") {
    format = "mp3";   // 未指定 / 不明な値は従来どおり mp3
  }

  AudioFileSourceHttpsPostStream *stream = new AudioFileSourceHttpsPostStream(tts_url.c_str(), root_ca);
  stream->addHeader("content-type", "application/json");
  // ローカルの音声合成サーバは鍵を要求しない。空の Bearer を送るとエンドポイントに
  // よっては 401 になるため、鍵があるときだけヘッダを付ける。
  if (param.api_key.length() > 0) {
    stream->addHeader("Authorization", String("Bearer ") + param.api_key);
  }
  Serial.println(dataTemplate);
  DynamicJsonDocument doc(2000);
  DeserializationError error = deserializeJson(doc, dataTemplate.c_str());
  if (error) {
    Serial.println("DeserializationError");
    String jsonStr;
    serializeJsonPretty(doc, jsonStr);  // 文字列をシリアルポートに出力する
    Serial.println(jsonStr);
    delete stream;
    return;
  }
  doc["input"] = String(text);
  doc["response_format"] = format;
  if(param.model != 0){
    doc["model"] = param.model;
  }
  if(param.voice != 0){
    doc["voice"] = param.voice;
  }
  String jsonStr;
  serializeJson(doc, jsonStr);
  stream->addData(jsonStr);

  // open() の失敗を無視すると、死んだストリームを再生器に渡して黙り込む。
  // TTS サーバが落ちている/遅すぎるときに原因が分かるよう、ここで打ち切る。
  if (!stream->open(tts_url.c_str())) {
    Serial.printf("OpenAITTS: failed to open %s\n", tts_url.c_str());
    delete stream;
    return;
  }

  AudioFileSourceBuffer *buff = new AudioFileSourceBuffer(stream, preallocateBuffer, preallocateBufferSize);
  if (format == "wav") {
    playWAV(buff);
  } else {
    playMP3(buff, MP3_STREAM_PREFILL_BYTES);   // ★ 2026-10-04 网络流预填, 见 PlayMP3.cpp
  }
  delete stream;
  delete buff;
}
