#include <ESP32WebServer.h>
#include <SD.h>
#include <SPIFFS.h>
#include <nvs.h>
#include <Preferences.h>
#include "WebAPI.h"
#include "Avatar.h"
#include "llm/ChatGPT/ChatGPT.h"
#include "llm/ChatGPT/FunctionCall.h"
#include "Robot.h"
#include "driver/GifPlayer.h"
#include "share/SpiffsMirror.h"
#include "driver/MotionReaction.h"
#include "driver/Look.h"   //OWNER 2026-10-09 look
#include "driver/HeadTouchSensor.h"   //OWNER 2026-10-11 头顶触摸门槛
#include "driver/CameraWB.h"   //code-to-muse 20261028-174524 GC0308 白平衡

using namespace m5avatar;
extern Avatar avatar;
extern uint8_t m5spk_virtual_channel;
extern String STT_API_KEY;

ESP32WebServer server(80);

namespace {

constexpr const char* DIZZY_GIF_PATH = GIF_SCENE_DIZZY "/stackchan-dizzy.gif";
constexpr const char* DIZZY_GIF_TEMP_PATH = GIF_SCENE_DIZZY "/stackchan-dizzy.tmp";
constexpr size_t DIZZY_GIF_MAX_BYTES = 200 * 1024;

File dizzyGifUpload;
bool dizzyGifUploadOk = false;
size_t dizzyGifUploadBytes = 0;

bool ensureDizzyGifDirectory()
{
  const char* dirs[] = {"/app", "/app/AiStackChanEx", GIF_SCENE_ROOT, GIF_SCENE_DIZZY};
  for (const char* dir : dirs) {
    if (!SD.exists(dir) && !SD.mkdir(dir)) {
      Serial.printf("[DizzyUpload] cannot create %s\n", dir);
      return false;
    }
  }
  return true;
}

// ★ OWNER 2026-07-26: 触摸语音的上传口。
//
// 为什么要新开一个而不用现成的 FTP:机上那个 esp8266FTPServer 的 STOR 写的是
// **SPIFFS 不是 SD**(ESP8266FtpServer.cpp:596),而且它的 MKD 硬编码返回失败
// (同文件:619)。也就是说 FTP 根本够不到 SD 卡——这条实测过,18 个文件全 451。
// 这里照 handle_dizzy_gif_upload 的路子走 HTTP 上传,那条是验证过能写 SD 的。
//
// 约定:目标由**上传的文件名**编码成 "<分类>-<文件名>",例如 pat-01.wav。
// 不用 query 参数,是因为 multipart 上传回调触发时 server.arg() 的时序不保证。
constexpr size_t ASSET_MAX_BYTES = 256 * 1024;
const char* const VOICE_CATEGORIES[] = {"pat", "pet", "pet_back", "shake"};
//★ OWNER 2026-07-26: GIF 场景也走同一套上传。起因是手拷 SD 那次把
//  voice/pet_back 的目录项写坏了(macOS 读得出、ESP32 遍历成空),折腾了好几轮。
//  设备自己写 SD 反而更可靠,而且换素材再不用拔卡。
const char* const GIF_SCENES[] = {"idle", "listening", "talking", "thinking",
                                  "error", "happy", "dizzy", "annoyed"};

File assetUpload;
bool assetUploadOk = false;
size_t assetUploadBytes = 0;
String assetUploadPath;
String assetUploadTemp;

//★ OWNER 2026-07-27: 写完读回校验用的滚动校验和。
// 起因:换新卡做对照实验时发现 **write() 返回了完整长度、接口返回 200、
// 文件大小也对,但内容是坏的** —— pet_back/02.wav 从第 26624 字节
// (= 13 x 2048, 正好一个上传缓冲块的边界) 起有 2033 字节跟源文件不一样。
// 也就是说 SD 在这条路径上会**静默写错一整块**而不报错。
// 16 次"成功"里出了 1 次,约 6%。在此之前"重试能过"这个结论是靠 200 判的,
// 而 200 根本不保证内容对 —— 所以那结论一直是假的。
// FNV-1a 32 位:三行、不用表、对"整块内容不同"这种错误足够灵敏。
constexpr uint32_t FNV1A_INIT = 2166136261u;
uint32_t assetUploadHash = FNV1A_INIT;

inline uint32_t fnv1aUpdate(uint32_t h, const uint8_t* p, size_t n)
{
  while (n--) { h ^= *p++; h *= 16777619u; }
  return h;
}

// "pat-01.wav" + root=voice   -> "/app/AiStackChanEx/voice/pat/01.wav"
// "idle-a.gif" + root=gif     -> "/app/AiStackChanEx/gif/idle/a.gif"
// 子目录必须命中白名单——顺带挡掉 ".." 之类的路径穿越。
bool resolveAssetPath(const char* root, const char* const* allowed, size_t allowedCount,
                      const String& uploadName, String& out)
{
  const int dash = uploadName.indexOf('-');
  if (dash <= 0) return false;
  const String sub = uploadName.substring(0, dash);
  const String file = uploadName.substring(dash + 1);
  if (file.length() == 0 || file.indexOf('/') >= 0 || file.indexOf('\\') >= 0) return false;

  bool known = false;
  for (size_t i = 0; i < allowedCount; i++) {
    if (sub == allowed[i]) { known = true; break; }
  }
  if (!known) return false;

  const String dir = String("/app/AiStackChanEx/") + root + "/" + sub;
  if (!SD.exists(dir) && !SD.mkdir(dir)) {
    Serial.printf("[AssetUpload] cannot create %s\n", dir.c_str());
    return false;
  }
  out = dir + "/" + file;
  return true;
}

// 两个路由共用同一台状态机(ESP32WebServer 一次只处理一个上传,不会并发)。
void handleAssetUpload(const char* root, const char* const* allowed, size_t allowedCount)
{
  HTTPUpload& upload = server.upload();
  switch (upload.status) {
    case UPLOAD_FILE_START: {
      //🔴 OWNER 2026-07-26: 这里**曾经**加过"上传期间 gif_theater_end() 让路",
      // 已撤销 —— 那条防线是反的。实测(二分,单变量):加了它之后每一次写入都失败
      // (ff_sd_status + 写到 2048/4096 字节就断),关掉立刻 2/2 成功。
      // 原因:CoreS3 的**屏幕和 SD 共用一条 SPI 总线**。停掉剧场会 avatar.resumeDraw(),
      // 把持续不停重绘的 avatar 唤回来;而 GIF 剧场每 20 秒才读一次文件。
      // 也就是说剧场是屏幕**最安静**的模式,停掉它反而把总线占满了。
      // 教训:动手"减少争用"之前先量谁在占总线,别照直觉猜。
      assetUploadBytes = 0;
      assetUploadHash = FNV1A_INIT;
      assetUploadOk = resolveAssetPath(root, allowed, allowedCount,
                                       upload.filename, assetUploadPath);
      if (!assetUploadOk) {
        Serial.printf("[AssetUpload] bad name: %s\n", upload.filename.c_str());
        break;
      }
      //先写 .tmp 再 rename:上传中途断了不会留下半截文件被设备抽中播出来
      assetUploadTemp = assetUploadPath + ".tmp";
      SD.remove(assetUploadTemp);
      assetUpload = SD.open(assetUploadTemp, FILE_WRITE);
      assetUploadOk = static_cast<bool>(assetUpload);
      break;
    }

    case UPLOAD_FILE_WRITE:
      if (assetUploadOk && assetUploadBytes + upload.currentSize <= ASSET_MAX_BYTES) {
        const size_t written = assetUpload.write(upload.buf, upload.currentSize);
        assetUploadOk = written == upload.currentSize;
        //算的是**收到的**字节, 不是写回去的 —— 待会读回来跟它比才有意义
        assetUploadHash = fnv1aUpdate(assetUploadHash, upload.buf, written);
        assetUploadBytes += written;
      } else {
        assetUploadOk = false;
      }
      break;

    case UPLOAD_FILE_END: {
      if (assetUpload) assetUpload.close();

      //★ OWNER 2026-07-27: 写完读回校验(见 assetUploadHash 上面那段注释)。
      // write() 全部返回成功也不代表卡上的内容是对的, 只有读回来比一遍才知道。
      // 代价: 50KB 文件在 10MHz SPI 下约 70ms, 相对一次上传可以忽略。
      if (assetUploadOk && assetUploadBytes > 0) {
        File back = SD.open(assetUploadTemp, FILE_READ);
        if (!back) {
          assetUploadOk = false;
          Serial.println("[AssetUpload] verify: 读不开 .tmp");
        } else if (back.size() != assetUploadBytes) {
          assetUploadOk = false;
          Serial.printf("[AssetUpload] verify: 大小不符 %u != %u\n",
                        (unsigned)back.size(), (unsigned)assetUploadBytes);
          back.close();
        } else {
          uint32_t h = FNV1A_INIT;
          uint8_t buf[512];
          while (int n = back.read(buf, sizeof(buf))) {
            if (n < 0) { h = 0; break; }
            h = fnv1aUpdate(h, buf, (size_t)n);
          }
          back.close();
          assetUploadOk = (h == assetUploadHash);
          if (!assetUploadOk) {
            Serial.printf("[AssetUpload] verify: 内容不符 %08x != %08x —— "
                          "卡静默写错了, 重传\n",
                          (unsigned)h, (unsigned)assetUploadHash);
          }
        }
      } else {
        assetUploadOk = false;
      }

      //★ OWNER 2026-07-27: 🔴 这里**曾经**是 SD.remove(目标) 然后 rename(.tmp)。
      // 那个顺序会吃文件: rename 失败时目标已经被删掉、下面那行又把 .tmp 也删了,
      // 新旧两份同时消失。2026-07-27 换卡实验实测 20 次失败里有 4 次是
      // "写满了完整字节数才失败"(= 失败发生在 rename 这一步), 当场丢了 3 个文件,
      // 而且丢完之后那个目录会进入持续写不进去的状态、重启也不解。
      // 07-26 那个"pet_back 目录项被写坏"的谜大概率就是这么来的。
      // 现在改成:原文件先改名避让 -> rename 新的 -> 成功才删避让副本、失败换回来。
      // 最坏情况(连避让都 rename 不动)才退回老做法, 至少不比以前差。
      if (assetUploadOk) {
        const String backup = assetUploadPath + ".bak";
        bool moved = false;
        if (SD.exists(assetUploadPath)) {
          SD.remove(backup);
          moved = SD.rename(assetUploadPath, backup);
          if (!moved) SD.remove(assetUploadPath);  //避让失败, 只能腾位置
        }
        assetUploadOk = SD.rename(assetUploadTemp, assetUploadPath);
        if (assetUploadOk) {
          if (moved) SD.remove(backup);
        } else if (moved) {
          SD.rename(backup, assetUploadPath);      //新的没成, 把旧的换回来
          Serial.println("[AssetUpload] rename 失败, 已回滚到旧文件");
        }
      }

      if (!assetUploadOk) SD.remove(assetUploadTemp);
      Serial.printf("[AssetUpload] %s %s (%u bytes)\n",
                    assetUploadOk ? "complete" : "failed",
                    assetUploadPath.c_str(), assetUploadBytes);

      //★ OWNER 2026-07-28: 语音上传成功后当场同步 SPIFFS 镜像(SpiffsMirror)。
      // 没有这一步, 镜像只在下次开机按"大小变了才拷"刷新——同字节数换内容会漏。
      // 镜像失败不影响返回码: SD 正身已经写好并读回校验过, 开机镜像还会兜底。
      if (assetUploadOk && strcmp(root, "voice") == 0) {
        spiffs_mirror_voice_file(assetUploadPath);
      }
      break;
    }

    case UPLOAD_FILE_ABORTED:
      if (assetUpload) assetUpload.close();
      SD.remove(assetUploadTemp);
      assetUploadOk = false;
      break;

    default:
      break;
  }
}

}  // namespace

// C++11 multiline string constants are neato...
static const char HEAD[] PROGMEM = R"KEWL(
<!DOCTYPE html>
<html lang="ja">
<head>
  <meta charset="UTF-8">
  <title>AIｽﾀｯｸﾁｬﾝ</title>
</head>)KEWL";

static const char APIKEY_HTML[] PROGMEM = R"KEWL(
<!DOCTYPE html>
<html>
  <head>
    <meta charset="UTF-8">
    <title>APIキー設定</title>
  </head>
  <body>
    <h1>APIキー設定</h1>
    <form>
      <label for="role1">OpenAI API Key</label>
      <input type="text" id="openai" name="openai" oninput="adjustSize(this)"><br>
      <label for="role2">VoiceVox API Key</label>
      <input type="text" id="voicevox" name="voicevox" oninput="adjustSize(this)"><br>
      <label for="role3">Speech to Text API Key</label>
      <input type="text" id="sttapikey" name="sttapikey" oninput="adjustSize(this)"><br>
      <button type="button" onclick="sendData()">送信する</button>
    </form>
    <script>
      function adjustSize(input) {
        input.style.width = ((input.value.length + 1) * 8) + 'px';
      }
      function sendData() {
        // FormDataオブジェクトを作成
        const formData = new FormData();

        // 各ロールの値をFormDataオブジェクトに追加
        const openaiValue = document.getElementById("openai").value;
        if (openaiValue !== "") formData.append("openai", openaiValue);

        const voicevoxValue = document.getElementById("voicevox").value;
        if (voicevoxValue !== "") formData.append("voicevox", voicevoxValue);

        const sttapikeyValue = document.getElementById("sttapikey").value;
        if (sttapikeyValue !== "") formData.append("sttapikey", sttapikeyValue);

	    // POSTリクエストを送信
	    const xhr = new XMLHttpRequest();
	    xhr.open("POST", "/apikey_set");
	    xhr.onload = function() {
	      if (xhr.status === 200) {
	        alert("データを送信しました！");
	      } else {
	        alert("送信に失敗しました。");
	      }
	    };
	    xhr.send(formData);
	  }
	</script>
  </body>
</html>)KEWL";

#if 0
static const char ROLE_HTML[] PROGMEM = R"KEWL(
<!DOCTYPE html>
<html>
<head>
	<title>ロール設定</title>
	<meta charset="UTF-8">
	<meta name="viewport" content="width=device-width, initial-scale=1.0">
	<style>
		textarea {
			width: 80%;
			height: 200px;
			resize: both;
		}
	</style>
</head>
<body>
	<h1>ロール設定</h1>
	<form onsubmit="postData(event)">
		<label for="textarea">ここにロールを記述してください。:</label><br>
		<textarea id="textarea" name="textarea"></textarea><br><br>
		<input type="submit" value="Submit">
	</form>
	<script>
		function postData(event) {
			event.preventDefault();
			const textAreaContent = document.getElementById("textarea").value.trim();
//			if (textAreaContent.length > 0) {
				const xhr = new XMLHttpRequest();
				xhr.open("POST", "/role_set", true);
				xhr.setRequestHeader("Content-Type", "text/plain;charset=UTF-8");
			// xhr.onload = () => {
			// 	location.reload(); // 送信後にページをリロード
			// };
			xhr.onload = () => {
				document.open();
				document.write(xhr.responseText);
				document.close();
			};
				xhr.send(textAreaContent);
//        document.getElementById("textarea").value = "";
				alert("Data sent successfully!");
//			} else {
//				alert("Please enter some text before submitting.");
//			}
		}
	</script>
</body>
</html>)KEWL";
#endif

#define IMPORT_FILE(section, filename, symbol) \
static constexpr const char* filename_##symbol = filename; \
extern const uint8_t symbol[], sizeof_##symbol[]; \
asm(\
  ".section " #section "\n"\
  ".balign 4\n"\
  ".global " #symbol "\n"\
  #symbol ":\n"\
  ".incbin \"incbin/" filename "\"\n"\
  ".global sizeof_" #symbol "\n"\
  ".set sizeof_" #symbol ", . - " #symbol "\n"\
  ".balign 4\n"\
  ".section \".text\"\n")

//IMPORT_FILE(.rodata, "index.html", index_html);
IMPORT_FILE(.rodata, "personalize.html", personalize_html);
IMPORT_FILE(.rodata, "personalize.js", personalize_js);


void handleRoot() {
  //Serial.println("handleRoot");
  //server.send(200, "text/plain", "hello from m5stack!");
  server.send_P(200, "text/html", (const char*)personalize_html, (size_t)sizeof_personalize_html);
}

void handle_personalize_html() {
  server.send_P(200, "text/html", (const char*)personalize_html, (size_t)sizeof_personalize_html);
}

void handle_personalize_js() {
  server.send_P(200, "application/javascript", (const char*)personalize_js, (size_t)sizeof_personalize_js);
}

void handleNotFound(){
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET)?"GET":"POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (uint8_t i=0; i<server.args(); i++){
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }
//  server.send(404, "text/plain", message);
  server.send(404, "text/html", String(HEAD) + String("<body>") + message + String("</body>"));
}

void handle_speech() {
  String message = server.arg("say");
  String speaker = server.arg("voice");
  //if(speaker != "") {
  //  TTS_PARMS = TTS_SPEAKER + speaker;
  //}
  Serial.println(message);
  ////////////////////////////////////////
  // 音声の発声
  ////////////////////////////////////////
  //avatar.setExpression(Expression::Happy);
  robot->speech(message);
  server.send(200, "text/plain", String("OK"));
}

void handle_chat() {
  static String response = "";
  // tts_parms_no = 1;
  String text = server.arg("text");
  String speaker = server.arg("voice");
  //if(speaker != "") {
  //  TTS_PARMS = TTS_SPEAKER + speaker;
  //}

  robot->chat(text);

  server.send(200, "text/html", String(HEAD)+String("<body>")+response+String("</body>"));
}

void handle_apikey() {
  // ファイルを読み込み、クライアントに送信する
  server.send(200, "text/html", APIKEY_HTML);
}

#if 0
void handle_apikey_set() {
  // POST以外は拒否
  if (server.method() != HTTP_POST) {
    return;
  }
  // openai
  String openai = server.arg("openai");
  // voicetxt
  String voicevox = server.arg("voicevox");
  // voicetxt
  String sttapikey = server.arg("sttapikey");
 
  OPENAI_API_KEY = openai;
  VOICEVOX_API_KEY = voicevox;
  STT_API_KEY = sttapikey;
  Serial.println(openai);
  Serial.println(voicevox);
  Serial.println(sttapikey);

  uint32_t nvs_handle;
  if (ESP_OK == nvs_open("apikey", NVS_READWRITE, &nvs_handle)) {
    nvs_set_str(nvs_handle, "openai", openai.c_str());
    nvs_set_str(nvs_handle, "voicevox", voicevox.c_str());
    nvs_set_str(nvs_handle, "sttapikey", sttapikey.c_str());
    nvs_close(nvs_handle);
  }
  server.send(200, "text/plain", String("OK"));
}
#endif

void handle_role_set() {
  String html = "";

  // POST以外は拒否
  if (server.method() != HTTP_POST) {
    return;
  }
  String role = server.arg("plain");

  // JSONデータをSPIFFSに保存
  if(robot->llm->save_userRole(role)){
#if 0
    // 整形したJSONデータを出力するHTMLデータを作成する
    serializeJsonPretty(robot->llm->get_chat_doc(), html);
    html = "<html><body><pre>" + html + "</pre></body></html>";
    //Serial.println(html);
#endif
    server.send(200, "text/plain", String("Role set successful"));
  }
  else{
    //html = "Failed to save role to SPIFFS.";
    server.send(500, "text/plain", String("Role set failed"));
  }

  // HTMLデータをシリアルに出力する
  //server.send(200, "text/html", html);
};

void handle_role_get() {
#if 0
  String html = "";
  serializeJsonPretty(robot->llm->get_chat_doc(), html);
  html = "<html><body><pre>" + html + "</pre></body></html>";

  // HTMLデータをシリアルに出力する
  //Serial.println(html);
  server.send(200, "text/html", String(HEAD) + html);
#endif
  Serial.println("http request: handle_role_get");
  Serial.println(robot->llm->get_userRole());
  server.send(200, "text/plain", robot->llm->get_userRole());
};

//★ OWNER 2026-10-04: 喇叭音量。屏幕被 GIF 剧场占着,VolumeSettingMod 点不到,改从 LAN 调。
//   GET /volume              → 只读
//   GET /volume?v=160        → 立刻生效,只改这次开机
//   GET /volume?v=160&save=1 → 生效并存 NVS(ns "audio" key "spkvol"),开机覆盖 yaml/默认 120
//   GET /volume?reset=1      → 删存值(下次开机回 yaml/默认;本次不变)
//   &say=1                   → 改完念一句测试
void handle_volume() {
  bool say = server.hasArg("say") && server.arg("say") == "1";
  if (server.hasArg("reset") && server.arg("reset") == "1") {
    Preferences p;
    if (p.begin("audio", false)) { p.remove("spkvol"); p.end(); }
  } else if (server.hasArg("v")) {
    String vs = server.arg("v");
    char* end = nullptr;
    long v = strtol(vs.c_str(), &end, 10);
    if (vs.length() == 0 || end == nullptr || *end != '\0' || v < 0 || v > 255) {
      server.send(400, "text/plain", "v must be an integer 0~255\n");
      return;
    }
    robot->spk_volume = (uint8_t)v;
    M5.Speaker.setVolume(robot->spk_volume);
    if (server.hasArg("save") && server.arg("save") == "1") {
      Preferences p;
      if (p.begin("audio", false)) { p.putUChar("spkvol", robot->spk_volume); p.end(); }
    }
  }
  bool saved = false;
  {
    Preferences p;
    if (p.begin("audio", true)) { saved = p.isKey("spkvol"); p.end(); }
  }
  String body = "{\"volume\":" + String(robot->spk_volume) +
                ",\"saved\":" + String(saved ? "true" : "false") + "}\n";
  Serial.print("[Volume] /volume "); Serial.print(body);
  server.send(200, "application/json", body);
  if (say) robot->speech("现在是这个音量，你听听看。");
}

//★ OWNER 2026-10-05: 摇晃反应开关（主人带着走路时它老说要被晃散架）。LAN 无鉴权，同其它路由。
//   GET /motion                → 只读当前状态
//   GET /motion?shake=1        → 打开，只管这次开机
//   GET /motion?shake=0&save=1 → 关掉并存 NVS，以后开机都关
//   GET /motion?reset=1        → 删存值，回默认（关）
void handle_motion() {
  if (server.hasArg("reset") && server.arg("reset") == "1") {
    Preferences p;
    if (p.begin("motion", false)) { p.remove("shake"); p.end(); }
    MotionReaction::setShakeEnabled(false);
  } else if (server.hasArg("shake")) {
    String v = server.arg("shake");
    if (v != "0" && v != "1") {
      server.send(400, "text/plain", "shake must be 0 or 1\n");
      return;
    }
    bool on = (v == "1");
    MotionReaction::setShakeEnabled(on);
    if (server.hasArg("save") && server.arg("save") == "1") {
      Preferences p;
      if (p.begin("motion", false)) { p.putBool("shake", on); p.end(); }
    }
  }
  bool saved = false;
  {
    Preferences p;
    if (p.begin("motion", true)) { saved = p.isKey("shake"); p.end(); }
  }
  String body = String("{\"shake\":") + (MotionReaction::shakeEnabled() ? "true" : "false") +
                ",\"saved\":" + (saved ? "true" : "false") + "}\n";
  Serial.print("[Motion] /motion "); Serial.print(body);
  server.send(200, "application/json", body);
}

//★ OWNER 2026-10-09: "让 Claude 看"的设备侧开关(总闸是网关的 STACKCHAN_LOOK)。LAN 无鉴权,同其它路由。
//   GET /look                → 只读当前状态
//   GET /look?on=0           → 关掉,只管这次开机(关着时 [[look]] 照样剥掉不念,只是不拍)
//   GET /look?on=1&save=1    → 打开并存 NVS
//   GET /look?reset=1        → 删存值,回默认(开)
void handle_look() {
  if (server.hasArg("reset") && server.arg("reset") == "1") {
    Preferences p;
    if (p.begin("look", false)) { p.remove("on"); p.end(); }
    Look::setEnabled(true);
  } else if (server.hasArg("on")) {
    String v = server.arg("on");
    if (v != "0" && v != "1") {
      server.send(400, "text/plain", "on must be 0 or 1\n");
      return;
    }
    bool on = (v == "1");
    Look::setEnabled(on);
    if (server.hasArg("save") && server.arg("save") == "1") {
      Preferences p;
      if (p.begin("look", false)) { p.putBool("on", on); p.end(); }
    }
  }
  bool saved = false;
  {
    Preferences p;
    if (p.begin("look", true)) { saved = p.isKey("on"); p.end(); }
  }
  String body = String("{\"look\":") + (Look::enabled() ? "true" : "false") +
                ",\"saved\":" + (saved ? "true" : "false") + "}\n";
  Serial.print("[Look] /look "); Serial.print(body);
  server.send(200, "application/json", body);
}

//★ OWNER 2026-10-11: 头顶触摸门槛(见 HeadTouchSensor.h)。LAN 无鉴权,同其它路由。
//   GET /headtouch               → 只读当前值
//   GET /headtouch?min=3         → 只管这次开机
//   GET /headtouch?min=3&save=1  → 存 NVS
//   GET /headtouch?min=4&save=1  → 头顶触摸整个关掉
//   GET /headtouch?reset=1       → 删存值,回默认(2)
void handle_headtouch() {
  if (server.hasArg("reset") && server.arg("reset") == "1") {
    Preferences p;
    if (p.begin("htouch", false)) { p.remove("min"); p.end(); }
    HeadTouchSensor::setMinLevel(HeadTouchSensor::TOUCH_MIN_LEVEL_DEFAULT);
  } else if (server.hasArg("min")) {
    String v = server.arg("min");
    if (v != "1" && v != "2" && v != "3" && v != "4") {
      server.send(400, "text/plain", "min must be 1, 2, 3 or 4 (4 = off)\n");
      return;
    }
    uint8_t level = (uint8_t)v.toInt();
    HeadTouchSensor::setMinLevel(level);
    if (server.hasArg("save") && server.arg("save") == "1") {
      Preferences p;
      if (p.begin("htouch", false)) { p.putUChar("min", level); p.end(); }
    }
  }
  bool saved = false;
  {
    Preferences p;
    if (p.begin("htouch", true)) { saved = p.isKey("min"); p.end(); }
  }
  String body = String("{\"min\":") + HeadTouchSensor::minLevel() +
                ",\"saved\":" + (saved ? "true" : "false") + "}\n";
  Serial.print("[HeadTouch] /headtouch "); Serial.print(body);
  server.send(200, "application/json", body);
}

//★ code-to-muse 20261028-174524: GC0308 白平衡(寄存器图与预设见 driver/CameraWB.h)。
// LAN 无鉴权,同其它路由。
//   GET /wb                        → 只读当前设定
//   GET /wb?mode=0..4              → 试预设(0=auto 开 AWB;1 sunny 2 cloudy 3 office 4 home),只管这次开机
//   GET /wb?mode=2&save=1          → 存 NVS,以后每次相机 init 后自动应用
//   GET /wb?r=128&g=64&b=74        → 手动增益(三个必须齐,0~255,AWB 关);&save=1 同样存
//   GET /wb?reset=1                → 删 NVS 存值并清内存,回到"没存过"的行为
// 相机是按需 init 的、平时不在:相机没开时设定只记内存/NVS,下次拍照 init 后、
// 扔帧之前应用(响应里的 camera_on 说现在在不在);相机正开着时当场写寄存器,
// 写失败回 500。reset 只清存值——已经写进传感器的增益活到下次 init 才回驱动默认。
void handle_wb() {
  bool writeFailed = false;
  if (server.hasArg("reset") && server.arg("reset") == "1") {
    CameraWB::clearSaved();
  } else if (server.hasArg("mode")) {
    String ms = server.arg("mode");
    char* end = nullptr;
    long m = strtol(ms.c_str(), &end, 10);
    if (ms.length() == 0 || end == nullptr || *end != '\0' || m < 0 || m > CameraWB::MODE_MAX) {
      server.send(400, "text/plain", "mode must be an integer 0~4\n");
      return;
    }
    CameraWB::setPreset((uint8_t)m);
    if (server.hasArg("save") && server.arg("save") == "1") {
      CameraWB::saveToNVS();
    }
    if (CameraWB::cameraOn()) {
      writeFailed = !CameraWB::applyNow();
    }
  } else if (server.hasArg("r") || server.hasArg("g") || server.hasArg("b")) {
    if (!server.hasArg("r") || !server.hasArg("g") || !server.hasArg("b")) {
      server.send(400, "text/plain", "r, g and b must be given together\n");
      return;
    }
    long v[3];
    const char* names[3] = {"r", "g", "b"};
    for (int i = 0; i < 3; i++) {
      String s = server.arg(names[i]);
      char* end = nullptr;
      v[i] = strtol(s.c_str(), &end, 10);
      if (s.length() == 0 || end == nullptr || *end != '\0' || v[i] < 0 || v[i] > 255) {
        server.send(400, "text/plain", "r/g/b must be integers 0~255\n");
        return;
      }
    }
    CameraWB::setCustom((uint8_t)v[0], (uint8_t)v[1], (uint8_t)v[2]);
    if (server.hasArg("save") && server.arg("save") == "1") {
      CameraWB::saveToNVS();
    }
    if (CameraWB::cameraOn()) {
      writeFailed = !CameraWB::applyNow();
    }
  }
  if (writeFailed) {
    server.send(500, "text/plain", "wb register write failed\n");
    return;
  }
  CameraWB::Setting cur;
  String body;
  if (CameraWB::current(cur)) {
    bool awb = !cur.custom && cur.mode == CameraWB::MODE_AUTO;
    body = String("{\"set\":true") +
           ",\"mode\":" + String(cur.mode) +
           ",\"custom\":" + String(cur.custom ? "true" : "false") +
           ",\"r\":" + String(cur.r) +
           ",\"g\":" + String(cur.g) +
           ",\"b\":" + String(cur.b) +
           ",\"awb\":" + String(awb ? "true" : "false") +
           ",\"saved\":" + String(CameraWB::savedInNVS() ? "true" : "false") +
           ",\"camera_on\":" + String(CameraWB::cameraOn() ? "true" : "false") + "}\n";
  } else {
    body = String("{\"set\":false") +
           ",\"saved\":" + String(CameraWB::savedInNVS() ? "true" : "false") +
           ",\"camera_on\":" + String(CameraWB::cameraOn() ? "true" : "false") + "}\n";
  }
  Serial.print("[WB] /wb "); Serial.print(body);
  server.send(200, "application/json", body);
}

//★ OWNER 2026-10-04: 待机俯仰角调校。LAN 无鉴权,与其它路由同一模型。
//   GET /servo_origin              → 只读当前值
//   GET /servo_origin?y=125        → 立刻转过去,只改这次开机(试角度用)
//   GET /servo_origin?y=125&save=1 → 转过去并存 NVS,以后开机都用它
//   GET /servo_origin?reset=1      → 删 NVS 存值,回到 yaml center.y
// Y 越小越抬头;夹在 80~160。
void handle_servo_origin() {
#ifdef USE_SERVO
  if (server.hasArg("reset") && server.arg("reset") == "1") {
    robot->servo->clearSavedOriginY();
  } else if (server.hasArg("y")) {
    String ys = server.arg("y");
    char* end = nullptr;
    long y = strtol(ys.c_str(), &end, 10);
    if (ys.length() == 0 || end == nullptr || *end != '\0') {
      server.send(400, "text/plain", "y must be an integer\n");
      return;
    }
    bool persist = server.hasArg("save") && server.arg("save") == "1";
    robot->servo->setOriginY((int)y, persist);
  }
  String body = "{\"y\":" + String(robot->servo->originY()) +
                ",\"yaml_y\":" + String(robot->servo->yamlOriginY()) +
                ",\"saved\":" + String(robot->servo->hasSavedOriginY() ? "true" : "false") +
                ",\"min\":" + String(ServoCustom::ORIGIN_Y_MIN) +
                ",\"max\":" + String(ServoCustom::ORIGIN_Y_MAX) + "}\n";
  Serial.print("[Servo] /servo_origin "); Serial.print(body);
  server.send(200, "application/json", body);
#else
  server.send(404, "text/plain", "servo disabled\n");
#endif
}

void handle_memory_get() {
  Serial.println("http request: handle_memory_get");
  Serial.println(robot->llm->get_userInfo());
  server.send(200, "text/plain", robot->llm->get_userInfo());
};

void handle_memory_clear() {
  Serial.println("http request: handle_memory_clear");
  bool result = robot->llm->clear_userInfo();
  if(result){
    server.send(200, "text/plain", String("Memory clear successful"));
  }else{
    server.send(500, "text/plain", String("Memory clear failed"));
  }
};

void handle_dizzy_gif_upload()
{
  HTTPUpload& upload = server.upload();
  switch (upload.status) {
    case UPLOAD_FILE_START:
      dizzyGifUploadOk = ensureDizzyGifDirectory();
      dizzyGifUploadBytes = 0;
      if (dizzyGifUploadOk) {
        SD.remove(DIZZY_GIF_TEMP_PATH);
        dizzyGifUpload = SD.open(DIZZY_GIF_TEMP_PATH, FILE_WRITE);
        dizzyGifUploadOk = static_cast<bool>(dizzyGifUpload);
      }
      break;

    case UPLOAD_FILE_WRITE:
      if (dizzyGifUploadOk && dizzyGifUploadBytes + upload.currentSize <= DIZZY_GIF_MAX_BYTES) {
        const size_t written = dizzyGifUpload.write(upload.buf, upload.currentSize);
        dizzyGifUploadOk = written == upload.currentSize;
        dizzyGifUploadBytes += written;
      } else {
        dizzyGifUploadOk = false;
      }
      break;

    case UPLOAD_FILE_END:
      if (dizzyGifUpload) {
        dizzyGifUpload.close();
      }
      if (dizzyGifUploadOk && dizzyGifUploadBytes > 0) {
        SD.remove(DIZZY_GIF_PATH);
        dizzyGifUploadOk = SD.rename(DIZZY_GIF_TEMP_PATH, DIZZY_GIF_PATH);
      }
      if (!dizzyGifUploadOk) {
        SD.remove(DIZZY_GIF_TEMP_PATH);
      }
      Serial.printf("[DizzyUpload] %s (%u bytes)\n", dizzyGifUploadOk ? "complete" : "failed", dizzyGifUploadBytes);
      break;

    case UPLOAD_FILE_ABORTED:
      if (dizzyGifUpload) {
        dizzyGifUpload.close();
      }
      SD.remove(DIZZY_GIF_TEMP_PATH);
      dizzyGifUploadOk = false;
      break;

    default:
      break;
  }
}

void handle_dizzy_gif_upload_complete()
{
  server.send(dizzyGifUploadOk ? 200 : 500, "text/plain", dizzyGifUploadOk ? "Dizzy GIF uploaded" : "Dizzy GIF upload failed");
}

void handle_voice_upload()
{
  handleAssetUpload("voice", VOICE_CATEGORIES,
                    sizeof(VOICE_CATEGORIES)/sizeof(VOICE_CATEGORIES[0]));
}

void handle_voice_upload_complete()
{
  server.send(assetUploadOk ? 200 : 500, "text/plain",
              assetUploadOk ? "voice clip uploaded" : "voice clip upload failed");
}

void handle_gif_upload()
{
  handleAssetUpload("gif", GIF_SCENES, sizeof(GIF_SCENES)/sizeof(GIF_SCENES[0]));
}

void handle_gif_upload_complete()
{
  server.send(assetUploadOk ? 200 : 500, "text/plain",
              assetUploadOk ? "gif uploaded" : "gif upload failed");
}

//★ OWNER 2026-07-28: GIF pak 上传口(白脸治本, 见 GifPlayer.cpp 顶部注释)。
// 落盘目标是 **SPIFFS 不是 SD**: /g/<scene>.pak。上传文件名即 "<scene>.pak",
// 场景白名单同 GIF_SCENES。写 .t 临时文件 -> FNV 读回校验 -> 换正身;
// 内部 flash 不玩 .bak 避让(SD 那套是对着会静默写坏的介质设计的)——
// 最坏情况 pak 缺失, GifPlayer 自动回退 SD, 不白脸。
// 🔴 SPIFFS 写偏慢(~50-200KB/s), 580KB 的 idle.pak 要 3~12s, 期间 loop 被
// ESP32WebServer 占着、触摸没响应——跟现有 SD 上传同级, 素材更新是低频手动操作, 可接受。
constexpr size_t GIFPAK_MAX_BYTES = 1536 * 1024;   // idle 目前 ~580KB, 留够余量
File gifPakUpload;
bool gifPakUploadOk = false;
size_t gifPakUploadBytes = 0;
uint32_t gifPakUploadHash = FNV1A_INIT;
String gifPakPath, gifPakTemp;

void handle_gifpak_upload()
{
  HTTPUpload& upload = server.upload();
  switch (upload.status) {
    case UPLOAD_FILE_START: {
      gifPakUploadOk = false;
      gifPakUploadBytes = 0;
      gifPakUploadHash = FNV1A_INIT;
      const String& nm = upload.filename;
      if (!nm.endsWith(".pak") || nm.length() <= 4) {
        Serial.printf("[GifPak] bad name: %s\n", nm.c_str());
        break;
      }
      const String scene = nm.substring(0, nm.length() - 4);
      bool known = false;
      for (size_t i = 0; i < sizeof(GIF_SCENES)/sizeof(GIF_SCENES[0]); i++) {
        if (scene == GIF_SCENES[i]) { known = true; break; }
      }
      if (!known) { Serial.printf("[GifPak] bad scene: %s\n", scene.c_str()); break; }
      if (!SPIFFS.begin(true)) { Serial.println("[GifPak] SPIFFS mount failed"); break; }
      gifPakPath = String("/g/") + scene + ".pak";
      gifPakTemp = gifPakPath + ".t";   // "/g/listening.pak.t"=19 字符, 在 SPIFFS 31 上限内
      SPIFFS.remove(gifPakTemp);
      gifPakUpload = SPIFFS.open(gifPakTemp, FILE_WRITE);
      gifPakUploadOk = (bool)gifPakUpload;
      break;
    }

    case UPLOAD_FILE_WRITE:
      if (gifPakUploadOk && gifPakUploadBytes + upload.currentSize <= GIFPAK_MAX_BYTES) {
        const size_t written = gifPakUpload.write(upload.buf, upload.currentSize);
        gifPakUploadHash = fnv1aUpdate(gifPakUploadHash, upload.buf, written);
        gifPakUploadBytes += written;
        gifPakUploadOk = written == upload.currentSize;
      } else {
        gifPakUploadOk = false;
      }
      break;

    case UPLOAD_FILE_END: {
      if (gifPakUpload) gifPakUpload.close();
      // 读回校验: 内部 flash 没观察到 SD 那种静默写坏, 但校验成本几十 ms,
      // "200 必须保证内容对"这条纪律不因介质换了就丢。
      if (gifPakUploadOk && gifPakUploadBytes > 0) {
        File back = SPIFFS.open(gifPakTemp, FILE_READ);
        uint32_t h = FNV1A_INIT;
        bool ok = (bool)back && back.size() == gifPakUploadBytes;
        if (ok) {
          uint8_t buf[512];
          while (int n = back.read(buf, sizeof(buf))) {
            if (n < 0) { ok = false; break; }
            h = fnv1aUpdate(h, buf, (size_t)n);
          }
        }
        if (back) back.close();
        gifPakUploadOk = ok && h == gifPakUploadHash;
        if (!gifPakUploadOk) Serial.println("[GifPak] verify failed");
      } else {
        gifPakUploadOk = false;
      }
      if (gifPakUploadOk) {
        SPIFFS.remove(gifPakPath);   // SPIFFS rename 不覆盖已存在目标
        gifPakUploadOk = SPIFFS.rename(gifPakTemp, gifPakPath);
      }
      if (!gifPakUploadOk) SPIFFS.remove(gifPakTemp);
      Serial.printf("[GifPak] %s %s (%u bytes)\n",
                    gifPakUploadOk ? "complete" : "failed",
                    gifPakPath.c_str(), (unsigned)gifPakUploadBytes);
      break;
    }

    case UPLOAD_FILE_ABORTED:
      if (gifPakUpload) gifPakUpload.close();
      SPIFFS.remove(gifPakTemp);
      gifPakUploadOk = false;
      break;
  }
}

void handle_gifpak_upload_complete()
{
  server.send(gifPakUploadOk ? 200 : 500, "text/plain",
              gifPakUploadOk ? "gif pak uploaded" : "gif pak upload failed");
}

//★ OWNER 2026-07-26: 素材删除口。
// 补这个是因为:上传口有了、删除口没有,所以"不喜欢哪个动画删掉就行"这句话
// 在设备上其实做不到,只能拔卡。
// 🔴 参数必须放 **POST body**(form-urlencoded),不能放 URL query ——
//    ESP32WebServer 对 POST 是拿 body 去填 args 的,URL 上的 query 会被丢掉
//    (实测: ?root=gif 拿到空字符串、当场 400 "bad root")。
//   curl -X POST http://<ip>/asset_delete -d root=gif -d sub=dizzy -d file=xxx.gif
void handle_asset_delete()
{
  const String root = server.arg("root");
  const String sub  = server.arg("sub");
  const String file = server.arg("file");

  const char* const* allowed = nullptr;
  size_t count = 0;
  if (root == "voice") {
    allowed = VOICE_CATEGORIES; count = sizeof(VOICE_CATEGORIES)/sizeof(VOICE_CATEGORIES[0]);
  } else if (root == "gif") {
    allowed = GIF_SCENES; count = sizeof(GIF_SCENES)/sizeof(GIF_SCENES[0]);
  } else {
    server.send(400, "text/plain", "bad root"); return;
  }

  String path;
  //复用上传那套解析(同一份白名单 + 同样挡路径穿越), 拼成 "<sub>-<file>" 喂进去
  if (!resolveAssetPath(root.c_str(), allowed, count, sub + "-" + file, path)) {
    server.send(400, "text/plain", "bad target"); return;
  }
  if (!SD.exists(path)) { server.send(404, "text/plain", "not found"); return; }
  const bool ok = SD.remove(path);   //别在这里停剧场,理由同 handleAssetUpload
  //★ OWNER 2026-07-28: 语音的 SPIFFS 镜像副本一起删, 不然下次开机还会被抽中
  if (ok && root == "voice") spiffs_mirror_voice_remove(path);
  Serial.printf("[AssetDelete] %s %s\n", ok ? "removed" : "FAILED", path.c_str());
  server.send(ok ? 200 : 500, "text/plain", ok ? "removed" : "remove failed");
}

void handle_face() {
  String expression = server.arg("expression");
  expression = expression + "\n";
  Serial.println(expression);
  switch (expression.toInt())
  {
    case 0: avatar.setExpression(Expression::Neutral); break;
    case 1: avatar.setExpression(Expression::Happy); break;
    case 2: avatar.setExpression(Expression::Sleepy); break;
    case 3: avatar.setExpression(Expression::Doubt); break;
    case 4: avatar.setExpression(Expression::Sad); break;
    case 5: avatar.setExpression(Expression::Angry); break;  
  } 
  server.send(200, "text/plain", String("OK"));
}

#if 0
void handle_setting() {
  String value = server.arg("volume");
  String led = server.arg("led");
  String speaker = server.arg("speaker");
//  volume = volume + "\n";
  Serial.println(speaker);
  Serial.println(value);
  size_t speaker_no;

  if(speaker != ""){
    speaker_no = speaker.toInt();
    if(speaker_no > 60) {
      speaker_no = 60;
    }
    TTS_SPEAKER_NO = String(speaker_no);
    TTS_PARMS = TTS_SPEAKER + TTS_SPEAKER_NO;
  }

  if(value == "") value = "180";
  size_t volume = value.toInt();
  uint8_t led_onoff = 0;
  uint32_t nvs_handle;
  if (ESP_OK == nvs_open("setting", NVS_READWRITE, &nvs_handle)) {
    if(volume > 255) volume = 255;
    nvs_set_u32(nvs_handle, "volume", volume);
    if(led != "") {
      if(led == "on") led_onoff = 1;
      else  led_onoff = 0;
      nvs_set_u8(nvs_handle, "led", led_onoff);
    }
    nvs_set_u8(nvs_handle, "speaker", speaker_no);

    nvs_close(nvs_handle);
  }
  M5.Speaker.setVolume(volume);
  M5.Speaker.setChannelVolume(m5spk_virtual_channel, volume);
  server.send(200, "text/plain", String("OK"));
}
#endif


void init_web_server(void)
{
  // Files
  //
  server.on("/", handleRoot);
  server.on("/personalize.html", handle_personalize_html);
  server.on("/personalize.js", handle_personalize_js);


  // APIs
  //
  server.on("/speech", handle_speech);
  server.on("/face", handle_face);
  server.on("/chat", handle_chat);
  server.on("/apikey", handle_apikey);
  //server.on("/setting", handle_setting);
  //server.on("/apikey_set", HTTP_POST, handle_apikey_set);
  server.on("/role_set", HTTP_POST, handle_role_set);
  server.on("/role_get", handle_role_get);
  server.on("/servo_origin", handle_servo_origin);   //OWNER 2026-10-04
  server.on("/volume", handle_volume);               //OWNER 2026-10-04
  server.on("/motion", handle_motion);               //OWNER 2026-10-05
  server.on("/look", handle_look);                   //OWNER 2026-10-09
  server.on("/headtouch", handle_headtouch);         //OWNER 2026-10-11
  server.on("/wb", handle_wb);                       //code-to-muse 20261028-174524
  server.on("/memory_get", handle_memory_get);
  server.on("/memory_clear", handle_memory_clear);
  server.on("/dizzy_gif_upload", HTTP_POST, handle_dizzy_gif_upload_complete, handle_dizzy_gif_upload);
  //★ OWNER 2026-07-26: 触摸语音上传(FTP 够不到 SD,见 resolveVoicePath 上面的注释)
  server.on("/voice_upload", HTTP_POST, handle_voice_upload_complete, handle_voice_upload);
  server.on("/gif_upload", HTTP_POST, handle_gif_upload_complete, handle_gif_upload);
  //★ OWNER 2026-07-28: GIF pak 上传(SPIFFS, 白脸治本)。Mac 侧 tools-gif/push_gifpaks.sh
  server.on("/gifpak_upload", HTTP_POST, handle_gifpak_upload_complete, handle_gifpak_upload);
  server.on("/asset_delete", HTTP_POST, handle_asset_delete);

  // Other
  //
  server.onNotFound(handleNotFound);
  server.on("/inline", [](){
    server.send(200, "text/plain", "this works as well");
  });

  server.begin();
  Serial.println("HTTP server started");
  M5.Lcd.println("HTTP server started");  
}

void web_server_handle_client(void)
{
  server.handleClient();
}
