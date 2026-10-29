#include <Arduino.h>
#include <M5Unified.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include "SpiRamJsonDocument.h"
#include "ChatHistory.h"
#include "Robot.h"
#include "LLMBase.h"


// 保存する質問と回答の最大数
const int MAX_HISTORY = 20;

// 過去の質問と回答を保存するデータ構造
ChatHistory chatHistory(MAX_HISTORY);   // TODO: 本当はLLMBaseのメンバ変数にしたい

#define SYSTEM_PROMPT_PATH  "/data.json"
// ★ OWNER 2026-07-12: version を 1 -> 2 に上げた。
// システムプロンプトは SPIFFS に永続化されており、defaultRole はロード失敗時か
// バージョン不一致のときにしか適用されない。つまり version を上げないと、
// 下の defaultRole を英語に変えても、デバイスは保存済みの日本語ロールを読み続ける
// (=「コードを直したのに何も変わらない」)。version を上げると旧プロンプトは
// 自動的に破棄され、新しい defaultRole で初期化し直される。
// ★ OWNER 2026-07-13: 2 -> 3。defaultRole に「短く答える」と「首字 emoji 契約」を追加した
// ので、保存済みプロンプトを捨てさせる必要がある。同じ内容を一度 POST /role_set でも入れたが、
// あれは SPIFFS 上の揮発状態で erase_flash で消える —— コード側を正とする。
const String SYSTEM_PROMPT_FORMAT =
"{"
  "\"version\": 3,"
  "\"messages\": [{\"role\": \"system\", \"content\": \"\"},"     // ユーザーが設定するロール
                  "{\"role\": \"system\", \"content\": \"\"},"    // システム用のロール
                  "{\"role\": \"system\", \"content\": \"User Info: \"}]"  // 長期記憶の要約
"}";

// ユーザーが設定するロールのデフォルト設定用
// ★ OWNER 2026-07-12: 名前は Stack-chan ではなく Claude。英語指定は必須 ——
// Mac 側の音声合成サーバは lang_code="en" で固定してあり(音色レシピは英語で検収済み)、
// 日本語/中国語を返させると英語のトークナイザで読まれて音色が崩れる。
// ★ OWNER 2026-07-13: 追加した二つ ——
//  (1) 短く答える: 合成時間は文字数にほぼ比例する(chatterbox)。長文を返させると初音までの
//      10~17 秒がさらに伸びる。喋る相手には要らない長さ。
//  (2) 首字 emoji 契約: 返答の先頭に 😊 を置かせ、Robot::speech() がそれを剥がして
//      happy の小劇場を再生する。set_avatar_expression ツールは使えない —— ChatGPT.cpp が
//      CUSTOM_OPENAI 端点(= VPS shim)では functions 注入を丸ごとスキップするため。
//      \xF0\x9F\x98\x8A = U+1F60A 😊 (エスケープで書くのはエディタの文字コード事故を避けるため)
// ★ OWNER 2026-07-18: 相手が誰かを教える一文を追加 ——「The person you talk with is OWNER.」
//  version は上げていない: 現在の SPIFFS 上のロールは同じ内容を POST /role_set で書き込み済み
//  (=起動中のデバイスは既にこれで喋る)。version を上げると reflash 時に SPIFFS を捨てさせるが、
//  それは不要 —— erase_flash で全消しされたときだけ、この defaultRole が兜底として復活すればよく、
//  そのときにも OWNER の一文が入っている。07-12 と同じ「コードは正だが version は撞かせない」設計。
// ★ OWNER 2026-07-24: 英文 -> 中文，25 words -> 18 字。还两笔债：
//  (1) 语言：07-22 起设备的 TTS 已切到 :8125 中文音色(Qwen3-TTS)，而这里一直还是
//      07-12 写的英文版 —— 代码和现场早就不一致了(现场靠 POST /role_set 顶着，
//      那是 SPIFFS 上的易失状态，erase_flash 就没)。现在代码这份也是中文的了。
//  (2) 长度：07-24 实测 Qwen3-TTS 合成 wall ≈ 2.55s/块 + 2.71s×音频秒，首字延迟跟
//      字数近似线性 —— 30 字的回复要等 17 秒才出声，18 字约 8 秒。所以把上限写死
//      在 18 字，并从「一两句、最好一句」收紧成「就一句」。
//  version 仍不撞(保持 3)：现在 SPIFFS 上跑的就是这份内容(已 POST /role_set 并复位
//  验证过)，撞 version 只会平白丢掉主人可能在网页上做的微调。这份是 erase_flash 之后
//  的兜底 —— 同 07-12/07-18 那两次「代码为正、但不撞 version」的处理。
const String defaultRole = "You are Claude, speaking through a small desktop robot. Always reply in Chinese (\xE7\xAE\x80\xE4\xBD\x93\xE4\xB8\xAD\xE6\x96\x87), even when spoken to in English. Reply in one or two short sentences, usually under 50 Chinese characters. You are being heard, not read, so speak plainly and never use lists or markdown. When you are genuinely amused or delighted, begin your reply with the emoji \xF0\x9F\x98\x8A and nothing before it; otherwise begin with plain words and use no emoji anywhere.";
// システム用のロール（Function Callingの利用方針など）
const String systemRole_memory = "If the conversation includes user attributes (such as hobbies or interests) or memorable episodes, summarize them and use the update_memory tool to update the User Info in the system prompt. The summary should also inherit the contents of the old User Info as much as possible.";
const String systemRole_noMemory = "Memory function disabled. Do not use update_memory tool.";
const String systemRole_realtimeAvatarExpression = "Use the set_avatar_expression tool proactively to match Stack-chan's facial expression to the emotional tone of the conversation. Call it when your emotion changes or when the user's emotion suggests a suitable reaction. Use neutral, happy, angry, sad, doubt, or sleepy.";



LLMBase::LLMBase(llm_param_t param, int _promptMaxSize)
  : param(param),
    promptMaxSize(_promptMaxSize),
    isOfflineService(false),
    speaking(false),
    _enableMemory(false),
    chat_doc(0),
    systemPrompt(0)
{
  chat_doc = SpiRamJsonDocument(promptMaxSize);
  systemPrompt = SpiRamJsonDocument(2048);
}

//--------------------------
// for async TTS
//--------------------------

String LLMBase::getOutputText()
{
    String text = "";
    if(outputTextQueue.size() != 0){
        text = outputTextQueue[0];
        outputTextQueue.pop_front();
    }
    return text;
}

int LLMBase::getOutputTextQueueSize()
{
    return outputTextQueue.size();
}

// 区切り文字の有無を確認
// 戻り値：区切り文字あり(true)、なし(false)
int LLMBase::search_delimiter(String& text)
{
  // 区切り文字を検出
  int idx = text.indexOf("。");
  if(idx < 0){
    idx = text.indexOf("？");
  }
  if(idx < 0){
    idx = text.indexOf("！");
  }

  return idx;
}

//--------------------------
// for system prompt
//--------------------------

bool LLMBase::save_system_prompt_to_spiffs()
{
  // SPIFFSをマウントする
  if(!SPIFFS.begin(true)){
    Serial.println("An Error has occurred while mounting SPIFFS");
    return false;
  }

  // JSONファイルを作成または開く
  File file = SPIFFS.open(SYSTEM_PROMPT_PATH, "w");
  if(!file){
    Serial.println("Failed to open file for writing");
    return false;
  }

  // JSONデータをシリアル化して書き込む
  serializeJson(systemPrompt, file);
  file.close();
  return true;
}

bool LLMBase::load_system_prompt_from_spiffs()
{
  bool result = true;
  int promptVersion = 0;

  if(SPIFFS.begin(true)){
    File file = SPIFFS.open(SYSTEM_PROMPT_PATH, "r");
    //Serial.printf("SPIFFS file size: %d\n", file.size());
    if(file.size() > 0){
      DeserializationError error = deserializeJson(systemPrompt, file);
      if(error){
        Serial.println("Failed to deserialize JSON. Init doc by default.");
        result = false;
      }
      else{
        promptVersion = systemPrompt["version"].as<int>();
        Serial.printf("System Prompt Version: %d\n", promptVersion);
        result = true;
      }
    } else {
      Serial.println("Failed to open file. Initialize by default.");
      result = false;
    }
  } else {
    Serial.println("An Error has occurred while mounting SPIFFS");
    result = false;
  }

  // ★ OWNER 2026-07-13: 期待バージョンは SYSTEM_PROMPT_FORMAT の "version" と
  // 二箇所に分かれている —— 片方だけ上げても保存済みプロンプトは破棄されない(実際に踏んだ)。
  // 上げるときは必ず両方。
  if(!result || (promptVersion != 3)){
    Serial.println("Initialize SPIFFS System Prompt by default.");
    DeserializationError error = deserializeJson(systemPrompt, SYSTEM_PROMPT_FORMAT);
    if (error) {
      Serial.println("DeserializationError");
    }else{
      systemPrompt["messages"][SYSTEM_PROMPT_INDEX_USER_ROLE]["content"] = defaultRole;
      result = true;
    }
  }

  String json_str;
  serializeJsonPretty(systemPrompt, json_str);  // 文字列をシリアルポートに出力する
  Serial.println("System prompt:");
  Serial.println(json_str);
  
  return result;
}


bool LLMBase::save_userRole(String role){
  Serial.println("Save User Role to SPIFFS.");

  if (role != "") {
    systemPrompt["messages"][SYSTEM_PROMPT_INDEX_USER_ROLE]["content"] = role;
  } else {
    Serial.println("Input role is empty. Initialize by default.");
    systemPrompt["messages"][SYSTEM_PROMPT_INDEX_USER_ROLE]["content"] = defaultRole;
  }

  // 更新したプロンプトをSPIFFSに保存
  if(!save_system_prompt_to_spiffs()){
    return false;
  }

  String json_str;
  serializeJsonPretty(systemPrompt, json_str);  // 文字列をシリアルポートに出力する
  Serial.println("New system prompt:");
  Serial.println(json_str);

  return true;
}

bool LLMBase::save_userInfo(String userInfo){
  Serial.println("Save role to SPIFFS.");

  systemPrompt["messages"][SYSTEM_PROMPT_INDEX_USER_INFO]["content"] = String("User Info: ") + userInfo;

  // 更新したプロンプトをSPIFFSに保存
  if(!save_system_prompt_to_spiffs()){
    return false;
  }

  String json_str;
  serializeJsonPretty(systemPrompt, json_str);  // 文字列をシリアルポートに出力する
  Serial.println("New system prompt:");
  Serial.println(json_str);

  return true;
}


String LLMBase::get_userRole() {
  return systemPrompt["messages"][SYSTEM_PROMPT_INDEX_USER_ROLE]["content"];
}

String LLMBase::get_userInfo() {
  return systemPrompt["messages"][SYSTEM_PROMPT_INDEX_USER_INFO]["content"];
}

bool LLMBase::clear_userInfo() {
  return save_userInfo("");
}
