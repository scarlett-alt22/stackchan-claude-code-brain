#include <ArduinoJson.h>
#include <WiFi.h>
#include "Whisper.h"
#include "rootCA/rootCACertificate.h"

namespace {
constexpr char* API_HOST = "api.openai.com";
constexpr int API_PORT = 443;
constexpr char* API_PATH = "/v1/audio/transcriptions";
constexpr uint32_t WIFI_RECOVERY_TIMEOUT_MS = 8000;
}  // namespace

Whisper::Whisper(stt_param_t param)
    : STTBase(param), plainClient(), secureClient(), client(nullptr), _useTLS(true) {
  // ★ OWNER 2026-07-05: 支持把 STT 指向任意 OpenAI 兼容转写端点(如 硅基流动 SenseVoice)。
  // 默认仍是 api.openai.com; 配了 stt.customEndpoint 就解析出 host/port/path,
  // 并优先用 stt.customRootCA(为空才回退内置 root_ca_openai)。
  _host = API_HOST;
  _port = API_PORT;
  _path = API_PATH;
  const String& ep = param.stt_conf.customEndpoint;
  if(ep.length() > 0){
    String rest = ep;
    if(rest.startsWith("https://")){ rest = rest.substring(8); _port = 443; _useTLS = true; }
    else if(rest.startsWith("http://")){ rest = rest.substring(7); _port = 80; _useTLS = false; }
    int slash = rest.indexOf('/');
    String hostport = (slash >= 0) ? rest.substring(0, slash) : rest;
    _path = (slash >= 0) ? rest.substring(slash) : String("/");
    int colon = hostport.indexOf(':');
    if(colon >= 0){
      _port = hostport.substring(colon + 1).toInt();
      _host = hostport.substring(0, colon);
    }
    else{
      _host = hostport;
    }
  }
  // ★ OWNER 2026-07-06: 修悬空指针 UB。setCACert 只存指针不拷内容;
  // 原来传的是按值局部形参 param 的 PEM 缓冲,构造函数一返回它就析构,
  // 指针悬空 → 等到 speech_to_text() 里 client.connect() 时 mbedTLS 读到垃圾 = -10112。
  // 改指 STTBase 存的持久成员 this->param(与 client 同为对象成员、同生命周期,connect 时必有效)。
  if(this->param.stt_conf.customRootCA.length() > 0){
    _rootCA = this->param.stt_conf.customRootCA.c_str();
  }
  else{
    _rootCA = root_ca_openai;
  }
  client = _useTLS ? static_cast<WiFiClient*>(&secureClient) : &plainClient;
  if(_useTLS){
    secureClient.setCACert(_rootCA);
  }
  client->setTimeout(10000);
  Serial.printf("Whisper endpoint: scheme=%s host=%s port=%d path=%s\n",
                _useTLS ? "https" : "http", _host.c_str(), _port, _path.c_str());
}

Whisper::~Whisper() {
  //client.stop();
}

bool Whisper::resolveEndpoint(IPAddress& address) {
  address = IPAddress((uint32_t)0);
  return WiFi.hostByName(_host.c_str(), address) && (uint32_t)address != 0;
}

bool Whisper::recoverWiFiForDNS() {
  Serial.println("Whisper DNS recovery: restarting Wi-Fi");
  client->stop();
  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_OFF);
  delay(250);
  WiFi.mode(WIFI_STA);
  WiFi.begin();

  const uint32_t startedAt = millis();
  while(WiFi.status() != WL_CONNECTED){
    if(millis() - startedAt >= WIFI_RECOVERY_TIMEOUT_MS){
      Serial.println("Whisper DNS recovery: Wi-Fi reconnect timeout");
      return false;
    }
    delay(250);
  }
  Serial.println("Whisper DNS recovery: Wi-Fi reconnected");
  return true;
}

bool Whisper::connectEndpoint() {
  IPAddress address;
  if(!resolveEndpoint(address)){
    Serial.println("Whisper DNS recovery: initial lookup failed");
    if(!recoverWiFiForDNS() || !resolveEndpoint(address)){
      Serial.println("Whisper DNS recovery: lookup still failed");
      return false;
    }
    Serial.println("Whisper DNS recovery: lookup recovered");
  }

  client->stop();
  // LAN HTTPは解決済みIPへ直接接続する。HTTPSはhost overloadを使い、
  // SNIと証明書検証を維持する。
  const bool connected = _useTLS
      ? secureClient.connect(_host.c_str(), _port)
      : plainClient.connect(address, _port);
  if(!connected){
    Serial.printf("Whisper: %s connection failed!\n", _useTLS ? "TLS" : "HTTP");
    return false;
  }
  return true;
}

String Whisper::Transcribe(AudioWhisper* audio) {
  // ★ OWNER 2026-07-05: model/language 改为可配(原写死 whisper-1 / ja)。
  //   model 空 -> 回退 whisper-1; language 空或 "null" -> 整段不发该字段(SenseVoice 自判)。
  String model = param.stt_conf.model;
  if(model.length() == 0 || model == "null"){ model = "whisper-1"; }
  const String& language = param.stt_conf.language;

  char boundary[64] = "------------------------";
  for (auto i = 0; i < 2; ++i) {
    ltoa(random(0x7fffffff), boundary + strlen(boundary), 16);
  }
  String header = "--" + String(boundary) + "\r\n"
    "Content-Disposition: form-data; name=\"model\"\r\n\r\n" + model + "\r\n";
  if(language.length() > 0 && language != "null"){
    header += "--" + String(boundary) + "\r\n"
      "Content-Disposition: form-data; name=\"language\"\r\n\r\n" + language + "\r\n";
  }
  header += "--" + String(boundary) + "\r\n"
    "Content-Disposition: form-data; name=\"file\"; filename=\"speak.wav\"\r\n"
    "Content-Type: application/octet-stream\r\n\r\n";
  const String footer = "\r\n--" + String(boundary) + "--\r\n";

  // header
  client->printf("POST %s HTTP/1.1\n", _path.c_str());
  client->printf("Host: %s\n", _host.c_str());
  client->println("Accept: */*");
  client->printf("Authorization: Bearer %s\n", param.api_key.c_str());
  client->printf("Content-Length: %d\n", header.length() + audio->GetSize() + footer.length());
  client->printf("Content-Type: multipart/form-data; boundary=%s\n", boundary);
  client->println();
  client->print(header.c_str());
  client->flush();

  auto ptr = audio->GetBuffer();
  auto remainings = audio->GetSize();
  while (remainings > 0) {
    auto sz = (remainings > 512) ? 512 : remainings;
    client->write(ptr, sz);
    client->flush();
    remainings -= sz;
    ptr += sz;
  }
  client->flush();

  // footer
  client->print(footer.c_str());
  client->flush();

  // wait response
  const auto now = ::millis();
  while (client->available() == 0) {
    if (::millis() - now > 10000) {
      Serial.println(">>> Client Timeout !");
      _lastErrorKind = STTErrorKind::Network;   // ★ 响应等待 10 秒超时 = Network
      return "";
    }
  }

  bool isBody = false;
  String body = "";
  int httpStatus = 0;
  bool firstLine = true;
  while(client->available()){
    const auto line = client->readStringUntil('\r');
    if (firstLine) {
      firstLine = false;
      // ★ 首行是状态行 "HTTP/1.1 200 OK"(原代码从不解析它)。substring(9,12)
      // 取状态码 —— Network / DidntHear 的判定就靠这一行。
      if (line.startsWith("HTTP/")) {
        httpStatus = line.substring(9, 12).toInt();
      }
    }
    if (isBody) {
      body += line;
    } else if (line.equals("\n")) {
      isBody = true;
    }
  }

  // ★ OWNER 2026-07-05: 200 字节池对中文转写太小(1汉字=3字节UTF-8),长句会 NoMemory
  // 导致 text 读空。放大到 1024, 给几百汉字余量。
  StaticJsonDocument<1024> doc;
  ::deserializeJson(doc, body);
  String text = doc["text"].as<String>();
  // ★ 判定确定点:200 且 text 为空 = DidntHear(服务通、没听清,保持无声);
  // 非 200 或根本没读到状态行 = Network。
  if (httpStatus == 200) {
    _lastErrorKind = (text.length() == 0) ? STTErrorKind::DidntHear : STTErrorKind::None;
  } else {
    _lastErrorKind = STTErrorKind::Network;
  }
  return text;
}



String Whisper::speech_to_text(){
  String ret;
  _lastErrorKind = STTErrorKind::None;
  AudioWhisper* audio = new AudioWhisper();
  Serial.println("\r\nRecord start!\r\n");
  audio->Record();  
  Serial.println("Record end\r\n");
  Serial.println("音声認識開始");
  //avatar.setSpeechText("わかりました");
  if (!connectEndpoint()) {
    Serial.println("Whisper: Connection failed!");
    _lastErrorKind = STTErrorKind::Network;   // ★ DNS/连接失败 = Network
    ret = String("");
  }
  else{
    ret = Transcribe(audio);
    client->stop();
  }
  delete audio;
  return ret;
}
