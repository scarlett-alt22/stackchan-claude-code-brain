#include "SpiffsMirror.h"

#include <SD.h>
#include <SPIFFS.h>

namespace {

// FNV-1a 32 位, 与 WebAPI.cpp 上传校验同一套(那份在匿名命名空间里, 这里自带一份)
constexpr uint32_t FNV1A_INIT = 2166136261u;
inline uint32_t fnv1aUpdate(uint32_t h, const uint8_t* p, size_t n)
{
  while (n--) { h ^= *p++; h *= 16777619u; }
  return h;
}

// SPIFFS 的对象名上限 31 字符(SPIFFS_OBJ_NAME_LEN 32 含结尾 0)。超长 = 打不开,
// 静默失败。镜像路径都是短的, 这里仍然逐个防守——将来加长文件名时第一时间报出来。
constexpr size_t SPIFFS_PATH_MAX = 31;

// 镜像写入前要求的最小剩余空间。SPIFFS 满了之后写入/GC 行为会劣化,
// 宁可跳过镜像(SD 还在正路上)也别把 role 等既有文件挤出险区。
constexpr size_t SPIFFS_HEADROOM = 64 * 1024;

struct Stats {
  int copied = 0, skipped = 0, failed = 0;
};

uint32_t fileFnv(fs::FS& fs, const char* path, bool* ok)
{
  File f = fs.open(path, FILE_READ);
  *ok = static_cast<bool>(f);
  if (!f) return 0;
  uint32_t h = FNV1A_INIT;
  uint8_t buf[512];
  int n;
  while ((n = f.read(buf, sizeof(buf))) > 0) h = fnv1aUpdate(h, buf, (size_t)n);
  f.close();
  return h;
}

// SD -> SPIFFS 单文件拷贝: 写 .t 临时文件 -> 读回 FNV 对源校验 -> rename 换正身。
// 任何一步失败都删掉 .t, 不碰既有正身(宁缺勿坏, 缺了还有 SD 正路兜着)。
bool copyVerified(const char* sdPath, const char* spPath)
{
  if (strlen(spPath) + 2 > SPIFFS_PATH_MAX) {   // +2 = ".t"
    Serial.printf("[Mirror] 路径超 SPIFFS 上限, 跳过: %s\n", spPath);
    return false;
  }

  File src = SD.open(sdPath, FILE_READ);
  if (!src) {
    Serial.printf("[Mirror] SD 读不开: %s\n", sdPath);
    return false;
  }
  const size_t srcSize = src.size();

  const size_t freeBytes = SPIFFS.totalBytes() - SPIFFS.usedBytes();
  if (freeBytes < srcSize + SPIFFS_HEADROOM) {
    Serial.printf("[Mirror] SPIFFS 空间不足(free=%u need=%u), 跳过: %s\n",
                  (unsigned)freeBytes, (unsigned)(srcSize + SPIFFS_HEADROOM), spPath);
    src.close();
    return false;
  }

  String tmpPath = String(spPath) + ".t";
  SPIFFS.remove(tmpPath);
  File dst = SPIFFS.open(tmpPath, FILE_WRITE);
  if (!dst) {
    Serial.printf("[Mirror] SPIFFS 写不开: %s\n", tmpPath.c_str());
    src.close();
    return false;
  }

  uint32_t srcHash = FNV1A_INIT;
  uint8_t buf[1024];
  bool ok = true;
  int n;
  while (ok && (n = src.read(buf, sizeof(buf))) > 0) {
    srcHash = fnv1aUpdate(srcHash, buf, (size_t)n);
    ok = dst.write(buf, (size_t)n) == (size_t)n;
  }
  src.close();
  dst.close();

  // 读回校验: write() 全成功也不代表落盘内容是对的(07-27 SD 上的教训,
  // 对内部 flash 同样适用这条纪律——判据落在产出物上, 不落在返回值上)
  if (ok) {
    bool readOk = false;
    const uint32_t backHash = fileFnv(SPIFFS, tmpPath.c_str(), &readOk);
    ok = readOk && backHash == srcHash;
    if (!ok) Serial.printf("[Mirror] 读回校验不符: %s\n", spPath);
  }
  if (ok) {
    SPIFFS.remove(spPath);
    ok = SPIFFS.rename(tmpPath, spPath);
    if (!ok) Serial.printf("[Mirror] rename 失败: %s\n", spPath);
  }
  if (!ok) SPIFFS.remove(tmpPath);
  return ok;
}

// contentCheck=true: 目标存在也要做内容级 FNV 比对(小文件用)。
// contentCheck=false: 只比缺失/大小(大文件用, 内容新鲜度由上传口的单文件同步保证)。
void mirrorOne(const char* sdPath, const char* spPath, bool contentCheck,
               bool force, Stats& st)
{
  File src = SD.open(sdPath, FILE_READ);
  if (!src) { st.failed++; Serial.printf("[Mirror] SD 缺源文件: %s\n", sdPath); return; }
  const size_t srcSize = src.size();
  src.close();

  bool need = force;
  if (!need) {
    File dst = SPIFFS.open(spPath, FILE_READ);
    if (!dst) {
      need = true;
    } else {
      need = dst.size() != srcSize;
      dst.close();
      if (!need && contentCheck) {
        bool a = false, b = false;
        const uint32_t hs = fileFnv(SD, sdPath, &a);
        const uint32_t hd = fileFnv(SPIFFS, spPath, &b);
        need = !(a && b && hs == hd);
      }
    }
  }
  if (!need) { st.skipped++; return; }

  if (copyVerified(sdPath, spPath)) st.copied++;
  else st.failed++;
}

bool nameLooksLikeJunk(const char* nm)
{
  // macOS 往 FAT 卡塞的 AppleDouble(._xxx)和我们自己的 .bak/.tmp 一律不进镜像
  return (nm[0] == '.' && nm[1] == '_') || strstr(nm, ".bak") || strstr(nm, ".tmp");
}

const char* const VOICE_CATS[4] = {"pat", "pet", "pet_back", "shake"};

}  // namespace

void spiffs_mirror_from_sd(bool force)
{
  if (!SPIFFS.begin(true)) {
    Serial.println("[Mirror] SPIFFS 挂不上, 镜像跳过");
    return;
  }
  const uint32_t t0 = millis();
  Stats st;

  // 1) 三份配置 yaml(内容级比对)
  mirrorOne("/app/AiStackChanEx/SC_ExConfig.yaml", "/SC_ExConfig.yaml", true, force, st);
  mirrorOne("/yaml/SC_BasicConfig.yaml", "/yaml/SC_BasicConfig.yaml", true, force, st);
  mirrorOne("/yaml/SC_SecConfig.yaml", "/yaml/SC_SecConfig.yaml", true, force, st);

  // 2) SD 根目录的 *.pem(yaml 的 customRootCAFile 引用的就是这些根路径)
  File root = SD.open("/");
  if (root && root.isDirectory()) {
    File f;
    while ((f = root.openNextFile())) {
      const char* nm = f.name();
      const size_t len = strlen(nm);
      const bool isPem = len > 4 && strcasecmp(nm + len - 4, ".pem") == 0;
      const bool isDir = f.isDirectory();
      f.close();
      if (isDir || !isPem || nameLooksLikeJunk(nm)) continue;
      String sd = String("/") + nm;
      mirrorOne(sd.c_str(), sd.c_str(), true, force, st);
    }
    root.close();
  }

  // 3) 触摸语音(缺失/大小比对; 内容级新鲜度走 /voice_upload 的单文件同步)
  for (const char* cat : VOICE_CATS) {
    String sdDir = String("/app/AiStackChanEx/voice/") + cat;
    File dir = SD.open(sdDir);
    if (!dir || !dir.isDirectory()) { if (dir) dir.close(); continue; }
    File f;
    while ((f = dir.openNextFile())) {
      const char* nm = f.name();
      const size_t len = strlen(nm);
      const bool isWav = len > 4 && strcasecmp(nm + len - 4, ".wav") == 0;
      const bool isDir = f.isDirectory();
      f.close();
      if (isDir || !isWav || nameLooksLikeJunk(nm)) continue;
      String sd = sdDir + "/" + nm;
      String sp = String("/v/") + cat + "/" + nm;
      mirrorOne(sd.c_str(), sp.c_str(), false, force, st);
    }
    dir.close();
  }

  Serial.printf("[Mirror] copied=%d skipped=%d failed=%d (%lums)\n",
                st.copied, st.skipped, st.failed,
                (unsigned long)(millis() - t0));
}

bool spiffs_mirror_voice_file(const String& sdPath)
{
  const char* prefix = "/app/AiStackChanEx/voice/";
  if (!sdPath.startsWith(prefix)) return false;
  String sp = String("/v/") + sdPath.substring(strlen(prefix));
  if (!SPIFFS.begin(true)) return false;
  const bool ok = copyVerified(sdPath.c_str(), sp.c_str());
  Serial.printf("[Mirror] voice %s -> %s %s\n", sdPath.c_str(), sp.c_str(),
                ok ? "ok" : "FAILED");
  return ok;
}

void spiffs_mirror_voice_remove(const String& sdPath)
{
  const char* prefix = "/app/AiStackChanEx/voice/";
  if (!sdPath.startsWith(prefix)) return;
  String sp = String("/v/") + sdPath.substring(strlen(prefix));
  if (SPIFFS.exists(sp)) {
    SPIFFS.remove(sp);
    Serial.printf("[Mirror] voice 镜像已删: %s\n", sp.c_str());
  }
}

bool spiffs_mirror_handle_command(const String& cmd)
{
  String c = cmd;
  c.trim();
  if (c != "mirror") return false;
  if (SD.cardType() == CARD_NONE) {
    Serial.println("[Mirror] SD 没挂载, 镜像不了");
    return true;
  }
  spiffs_mirror_from_sd(true);
  return true;
}
