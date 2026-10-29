#include "VoiceClip.h"

#include <SD.h>
#include <SPIFFS.h>
#include "PlayMP3.h"

namespace VoiceClip {

namespace {

constexpr int MAX_CLIPS_PER_CATEGORY = 12;
constexpr size_t MAX_NAME_LEN = 48;

//★ OWNER 2026-07-26: 开机扫一次,名字缓存在内存里。
//
// 起因是实测:每次触摸现扫目录,三次里会有一次扫成空(SD 驱动偶发故障,记忆里
// "SD 卡驱动偶发故障(自愈重启)"那条),表现是摸了没声音、也没有任何报错。
// 把最不可靠的操作(目录遍历)从每次触摸的热路径上挪到开机一次,故障面积小一个数量级。
// 代价:用 /voice_upload 传了新片之后,要重启才会被抽中——可接受,换台词本来就不频繁。
// 兜底:缓存为空的分类仍会当场扫一次(见 play())。
//
//★ OWNER 2026-07-28: SPIFFS 优先、SD 回退(SpiffsMirror 那套的一部分)。
// 语音镜像在 /v/<cat>/*.wav(SPIFFS 31 字符路径上限,文件名全是 NN.wav 这种
// 短名,直存放得下,不用像 GIF 那样打 pak)。SPIFFS 不共享 SD 那条会抽风的
// SPI 总线,扫得到就用它——拔了卡、卡坏了、开机抽到坏模式,摸头照样有声。
struct Cache {
  char names[MAX_CLIPS_PER_CATEGORY][MAX_NAME_LEN];
  int count;
  bool fromSpiffs;   // 这一类当前用的是哪个源(播放时要走同一个 FS)
};
Cache s_cache[4];
const char *const s_dirs[4] = {VOICE_CLIP_PAT, VOICE_CLIP_PET,
                               VOICE_CLIP_PET_BACK, VOICE_CLIP_SHAKE};
const char *const s_spiffs_dirs[4] = {"/v/pat", "/v/pet", "/v/pet_back",
                                      "/v/shake"};

int categoryIndex(const char *category)
{
  for (int i = 0; i < 4; i++) {
    if (strcmp(category, s_dirs[i]) == 0) return i;
  }
  return -1;
}

// 把目录里的 .wav 名字读进 out,返回条数。失败(SD 抽风/目录不存在)返回 0。
int scanDir(fs::FS &fs, const char *dirPath, char out[][MAX_NAME_LEN])
{
  File dir = fs.open(dirPath);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return 0;
  }
  int n = 0;
  File f;
  while (n < MAX_CLIPS_PER_CATEGORY && (f = dir.openNextFile())) {
    const char *nm = f.name();
    const size_t len = strlen(nm);
    // 跳过 macOS 往 FAT 卡上写的 AppleDouble 伴生文件(._xxx.wav):
    // 它也是 .wav 结尾,会被抽中,但根本不是音频。
    if (!f.isDirectory() && len > 4 && strcasecmp(nm + len - 4, ".wav") == 0 &&
        len < MAX_NAME_LEN && !(nm[0] == '.' && nm[1] == '_')) {
      strcpy(out[n++], nm);
    }
    f.close();
  }
  dir.close();
  return n;
}

// SPIFFS 优先、SD 回退,把结果写进缓存。返回条数。
int scanCategory(int idx)
{
  int n = scanDir(SPIFFS, s_spiffs_dirs[idx], s_cache[idx].names);
  s_cache[idx].fromSpiffs = n > 0;
  if (n == 0) n = scanDir(SD, s_dirs[idx], s_cache[idx].names);
  s_cache[idx].count = n;
  return n;
}

}  // namespace

void begin(void)
{
  SPIFFS.begin(true);   // 语音镜像所在;各处都这么惰性挂,重复调用无害

  //★ OWNER 2026-07-28: SD 死了(或根本没插卡)就别去建目录了——五次
  // mkdir FAILED 刷屏只会在下次排查时把人引向错误的方向。
  if (SD.cardType() != CARD_NONE) {
    static const char *const dirs[] = {
        VOICE_CLIP_ROOT, VOICE_CLIP_PAT, VOICE_CLIP_PET,
        VOICE_CLIP_PET_BACK, VOICE_CLIP_SHAKE,
    };
    for (const char *d : dirs) {
      if (SD.exists(d)) continue;
      if (SD.mkdir(d)) {
        Serial.printf("[VoiceClip] mkdir %s\n", d);
      } else {
        Serial.printf("[VoiceClip] mkdir FAILED %s\n", d);
      }
    }
  }

  //★ OWNER 2026-07-26: 开机扫一次填缓存, 顺便把每个目录看到了什么打出来。
  //  这行埋点是这轮定位 "Mac 上 md5 验过 40/40 都在, 设备却报 no clip" 的关键 ——
  //  它当场证明了设备看到的是拷贝**之前**的旧目录, 从而把方向从"文件没拷进去"
  //  掰到"目录项坏了"。保留, 只在开机跑一次。
  for (int i = 0; i < 4; i++) {
    const int n = scanCategory(i);
    Serial.printf("[VoiceClip/ls] %s : %d 个 (源=%s)\n", s_dirs[i], n,
                  n == 0 ? "无" : (s_cache[i].fromSpiffs ? "SPIFFS" : "SD"));
    for (int k = 0; k < n; k++) {
      Serial.printf("      %s\n", s_cache[i].names[k]);
    }
  }
}

bool play(const char *category)
{
  const int idx = categoryIndex(category);
  int count = (idx >= 0) ? s_cache[idx].count : 0;

  // 缓存空(开机那次扫失败,或刚用 /voice_upload 传了新分类)就当场补扫一次
  if (count == 0 && idx >= 0) {
    count = scanCategory(idx);
    if (count) Serial.printf("[VoiceClip] %s 缓存补扫到 %d 个 (源=%s)\n",
                             category, count,
                             s_cache[idx].fromSpiffs ? "SPIFFS" : "SD");
  }

  if (count == 0) {
    // 没配语音就安静地不说话——GIF 和舵机那两路照常走,不因为缺素材整条反馈失效。
    Serial.printf("[VoiceClip] no clip in %s\n", category);
    return false;
  }

  const bool spiffs = s_cache[idx].fromSpiffs;
  char path[160];
  snprintf(path, sizeof(path), "%s/%s",
           spiffs ? s_spiffs_dirs[idx] : s_dirs[idx],
           s_cache[idx].names[esp_random() % count]);
  return playWAVFS(spiffs ? (fs::FS &)SPIFFS : (fs::FS &)SD, path);
}

}  // namespace VoiceClip
