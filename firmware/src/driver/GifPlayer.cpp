// Clawd GIF 小剧场 (OWNER 2026-07-11; 同日 v2 剧场模式) — 实现说明:
// - GIF 文件整个读进 PSRAM 再解码(避免播放中反复占 SD 总线);合成缓冲 320x240 RGB565 也在 PSRAM,
//   处理 PIL 保存的帧间差分/透明像素(透明=保留上一帧内容)
// - 播放走独立 FreeRTOS task;接管屏幕用 avatar.pauseDraw()(帧间生效),不用 vTaskSuspend
//   (可能把 drawLoop 冻在 SPI 事务中间)
// - 剧场模式(v2): s_theater=true 时任务退出**不** resumeDraw——场景切换(stop→start)之间
//   avatar 不会闪出来,屏幕定格在上一帧直到新场景接管;只有 gif_theater_end 交还屏幕
// - 所有公开接口只从 loop task 上下文调用,不做互斥
#include <SD.h>
#include <SPIFFS.h>
#include <M5Unified.h>
#include <Avatar.h>
#include <AnimatedGIF.h>
#include "GifPlayer.h"

extern m5avatar::Avatar avatar;

#define GIF_SCREEN_W   320
#define GIF_SCREEN_H   240
#define GIF_MAX_FILES  24     // 单场景目录最多参与随机的文件数
#define GIF_SCAN_ATTEMPTS 3   // CoreS3 的 SD 偶发单次状态失败;场景扫描短重试后再判空
#define GIF_SCAN_RETRY_MS 80

static AnimatedGIF s_gif;
static uint8_t*  s_fileBuf  = nullptr;   // PSRAM,按需增长,常驻复用
static size_t    s_fileCap  = 0;
static size_t    s_fileSize = 0;
static uint16_t* s_frameBuf = nullptr;   // PSRAM 全屏合成缓冲

static volatile bool s_playing  = false;
static volatile bool s_stopReq  = false;
static bool s_once = false;
static bool s_theater = false;
static char s_sceneDir[96] = {0};
static m5avatar::BatteryIcon s_batteryIcon;
static constexpr int BATTERY_OVERLAY_W = 35;
static constexpr int BATTERY_OVERLAY_H = 15;
//OWNER 2026-07-24: 左上角挪到右上角(主人盯着屏幕确认过左上角能看到、但要挪位置)
static constexpr int BATTERY_OVERLAY_X = GIF_SCREEN_W - BATTERY_OVERLAY_W - 5;
static constexpr int BATTERY_OVERLAY_Y = 5;

static void draw_battery_overlay() {
  s_batteryIcon.drawOverlay(
      &M5.Display, BATTERY_OVERLAY_X, BATTERY_OVERLAY_Y,
      M5.Display.color565(0, 0, 0),
      M5.Display.color565(250, 243, 224),
      avatar.getBatteryIconStatus(), avatar.getBatteryLevel());
}

// GIF rows are sent straight to the panel. Restore the one icon row that was
// just covered instead of waiting for the other ~220 rows in the frame; that
// delay made the overlay disappear/reappear continuously.
static void draw_battery_overlay_row(int y) {
  int32_t oldX, oldY, oldW, oldH;
  M5.Display.getClipRect(&oldX, &oldY, &oldW, &oldH);
  M5.Display.setClipRect(BATTERY_OVERLAY_X, y, BATTERY_OVERLAY_W, 1);
  draw_battery_overlay();
  M5.Display.setClipRect(oldX, oldY, oldW, oldH);
}

bool gif_scene_is_playing() { return s_playing; }

bool gif_theater_on() { return s_theater; }

// 探到 idle 素材才开剧场(没插SD/没拷素材=不开,整机回退原版橙脸行为)
bool gif_theater_begin() {
  s_theater = gif_scene_start(GIF_SCENE_IDLE);
  Serial.printf("[GifPlayer] theater %s: %s\n",
                s_theater ? "started" : "unavailable", GIF_SCENE_IDLE);
  return s_theater;
}

void gif_theater_end() {
  s_theater = false;
  if (s_playing) gif_scene_stop();   // 任务退出路径会 resumeDraw(s_theater 已放下)
  else avatar.resumeDraw();
}

// AnimatedGIF 行回调:先合成进全屏缓冲(透明像素=保留旧值),再推整行
static void gifDrawCb(GIFDRAW* pDraw) {
  int y = pDraw->iY + pDraw->y;
  if (y >= GIF_SCREEN_H) return;
  int w = pDraw->iWidth;
  if (pDraw->iX + w > GIF_SCREEN_W) w = GIF_SCREEN_W - pDraw->iX;
  if (w <= 0) return;

  uint16_t* pal = (uint16_t*)pDraw->pPalette;
  uint8_t*  src = pDraw->pPixels;
  uint16_t* dst = &s_frameBuf[y * GIF_SCREEN_W + pDraw->iX];

  if (pDraw->ucDisposalMethod == 2) {   // restore-to-background: 透明像素落成背景色
    for (int x = 0; x < w; x++) {
      if (src[x] == pDraw->ucTransparent) src[x] = pDraw->ucBackground;
    }
    pDraw->ucHasTransparency = 0;
  }
  if (pDraw->ucHasTransparency) {
    uint8_t tr = pDraw->ucTransparent;
    for (int x = 0; x < w; x++) {
      if (src[x] != tr) dst[x] = pal[src[x]];
    }
  } else {
    for (int x = 0; x < w; x++) dst[x] = pal[src[x]];
  }
  M5.Display.pushImage(pDraw->iX, y, w, 1, dst);
  if (y >= BATTERY_OVERLAY_Y &&
      y < BATTERY_OVERLAY_Y + BATTERY_OVERLAY_H &&
      pDraw->iX < BATTERY_OVERLAY_X + BATTERY_OVERLAY_W &&
      pDraw->iX + w > BATTERY_OVERLAY_X) {
    draw_battery_overlay_row(y);
  }
}

static bool ensure_frame_buf() {
  if (s_frameBuf == nullptr) {
    s_frameBuf = (uint16_t*)heap_caps_malloc(GIF_SCREEN_W * GIF_SCREEN_H * 2, MALLOC_CAP_SPIRAM);
    if (s_frameBuf == nullptr) {
      Serial.println("[GifPlayer] frameBuf PSRAM alloc failed");
      return false;
    }
  }
  return true;
}

//★ OWNER 2026-07-28: 白脸治本——素材主路改 SPIFFS pak, SD 退居回退。
// 白脸 = 开机 idle 扫描恰逢 SD 坏模式, 剧场起不来, 整轮开机回退经典 avatar(奶白脸)。
// SPIFFS 在内部 flash 上, 跟 SD/SPI 总线那摊事(MISO/DC 冲突、坏模式开机)物理无关,
// 人设存那儿从没坏过——把"画脸"对 SD 的依赖整个移除, 白脸就没有存在前提了。
// 🔴 为什么是 pak 不是逐文件: SPIFFS 全路径上限 31 字符(CONFIG_SPIFFS_OBJ_NAME_LEN=32,
//    Arduino 预编译框架改不了), 而素材名最长 27 字符, 怎么拼都放不下; 换 LittleFS
//    要格式化分区 -> 人设(LLMBase 的 role)就没了。所以每场景一个包: /g/<scene>.pak。
// 格式 GPK1(Mac 侧 tools-gif/make_gifpak.py 生成):
//   "GPK1" + u16le count + count 条 { u8 nameLen, name, u32le off, u32le len } + 素材体
// off 相对文件头。索引才几百字节, 每次现读不缓存——素材热更后下一次挑片立刻生效。
struct GifPick {
  bool     fromPak;
  char     path[160];   // pak 路径, 或回退时 SD 上的 gif 完整路径
  uint32_t off, len;    // fromPak 时有效
};

static const char* scene_basename(const char* sceneDir) {
  const char* s = strrchr(sceneDir, '/');
  return s ? s + 1 : sceneDir;
}

static bool spiffs_ready() {
  static bool tried = false, ok = false;
  if (!tried) {   // 人设那套(LLMBase)也是 begin(true); 已挂载时 begin 直接返回 true
    tried = true;
    ok = SPIFFS.begin(true);
    if (!ok) Serial.println("[GifPlayer] SPIFFS mount failed");
  }
  return ok;
}

// 从 /g/<scene>.pak 随机挑一条。pak 缺/坏/越界都返回 false, 调用方回退 SD 老路。
static bool pick_random_gif_pak(const char* sceneDir, GifPick& out) {
  if (!spiffs_ready()) return false;
  char pakPath[32];
  snprintf(pakPath, sizeof(pakPath), "/g/%s.pak", scene_basename(sceneDir));
  File p = SPIFFS.open(pakPath, FILE_READ);
  if (!p) return false;
  const uint32_t fileSize = p.size();
  uint8_t hdr[6];
  const bool bad = (p.read(hdr, sizeof(hdr)) != sizeof(hdr)) || memcmp(hdr, "GPK1", 4) != 0;
  const uint16_t count = bad ? 0 : (uint16_t)(hdr[4] | (hdr[5] << 8));
  if (bad || count == 0) {
    p.close();
    Serial.printf("[GifPlayer] pak 坏/空: %s\n", pakPath);
    return false;
  }
  const uint32_t want = esp_random() % count;
  uint32_t off = 0, len = 0;
  for (uint32_t i = 0; i <= want; i++) {   // 索引顺序读到第 want 条就够
    uint8_t nl;
    char name[48];
    if (p.read(&nl, 1) != 1 || nl == 0 || nl >= sizeof(name) ||
        p.read((uint8_t*)name, nl) != nl) { p.close(); return false; }
    uint8_t v[8];
    if (p.read(v, sizeof(v)) != sizeof(v)) { p.close(); return false; }
    off = (uint32_t)v[0] | ((uint32_t)v[1] << 8) | ((uint32_t)v[2] << 16) | ((uint32_t)v[3] << 24);
    len = (uint32_t)v[4] | ((uint32_t)v[5] << 8) | ((uint32_t)v[6] << 16) | ((uint32_t)v[7] << 24);
  }
  p.close();
  if (len == 0 || off > fileSize || len > fileSize - off) {
    Serial.printf("[GifPlayer] pak 条目越界: %s\n", pakPath);
    return false;
  }
  out.fromPak = true;
  strlcpy(out.path, pakPath, sizeof(out.path));
  out.off = off;
  out.len = len;
  static bool s_announced = false;
  if (!s_announced) { s_announced = true; Serial.println("[GifPlayer] 素材源=SPIFFS pak"); }
  return true;
}

// 扫一次场景目录,随机挑一个 .gif 并拼出完整路径。
// 调用方负责短重试:CoreS3 的 SD 偶发一次 ff_sd_status,紧接着同目录又能正常遍历。
static bool pick_random_gif_once(const char* sceneDir, char* out, size_t outLen) {
  File dir = SD.open(sceneDir);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return false;
  }
  char names[GIF_MAX_FILES][48];
  int n = 0;
  File f;
  while (n < GIF_MAX_FILES && (f = dir.openNextFile())) {
    const char* nm = f.name();   // esp32 core 2.x: 不带路径的文件名
    size_t len = strlen(nm);
    if (!f.isDirectory() && len > 4 && strcasecmp(nm + len - 4, ".gif") == 0 && len < sizeof(names[0])) {
      strcpy(names[n++], nm);
    }
    f.close();
  }
  dir.close();
  if (n == 0) return false;
  snprintf(out, outLen, "%s/%s", sceneDir, names[esp_random() % n]);
  return true;
}

static bool pick_random_gif(const char* sceneDir, GifPick& out) {
  //★ 2026-07-28: pak 优先(内部 flash, 不经过 SD)。pak 缺/坏才走 SD 老路+短重试。
  if (pick_random_gif_pak(sceneDir, out)) return true;
  out.fromPak = false;
  out.off = out.len = 0;
  for (int attempt = 1; attempt <= GIF_SCAN_ATTEMPTS; attempt++) {
    if (pick_random_gif_once(sceneDir, out.path, sizeof(out.path))) {
      if (attempt > 1) {
        Serial.printf("[GifPlayer] scene scan recovered (%d/%d): %s\n",
                      attempt, GIF_SCAN_ATTEMPTS, sceneDir);
      }
      return true;
    }
    if (attempt < GIF_SCAN_ATTEMPTS) {
      Serial.printf("[GifPlayer] scene scan failed (%d/%d), retry: %s\n",
                    attempt, GIF_SCAN_ATTEMPTS, sceneDir);
      vTaskDelay(pdMS_TO_TICKS(GIF_SCAN_RETRY_MS));
    }
  }
  Serial.printf("[GifPlayer] scene unavailable after %d scans: %s\n",
                GIF_SCAN_ATTEMPTS, sceneDir);
  return false;
}

static bool load_file(const char* path) {
  File f = SD.open(path, FILE_READ);
  if (!f) return false;
  size_t sz = f.size();
  if (sz == 0) { f.close(); return false; }
  if (sz > s_fileCap) {
    uint8_t* nb = (uint8_t*)heap_caps_realloc(s_fileBuf, sz, MALLOC_CAP_SPIRAM);
    if (nb == nullptr) { f.close(); Serial.println("[GifPlayer] fileBuf PSRAM alloc failed"); return false; }
    s_fileBuf = nb;
    s_fileCap = sz;
  }
  size_t rd = f.read(s_fileBuf, sz);
  f.close();
  if (rd != sz) return false;
  s_fileSize = sz;
  return true;
}

//★ 2026-07-28: 统一加载入口——pak 条目按 off/len 切读, SD 回退整文件读。
static bool load_pick(const GifPick& pk) {
  if (!pk.fromPak) return load_file(pk.path);
  File f = SPIFFS.open(pk.path, FILE_READ);
  if (!f) return false;
  if (pk.len > s_fileCap) {
    uint8_t* nb = (uint8_t*)heap_caps_realloc(s_fileBuf, pk.len, MALLOC_CAP_SPIRAM);
    if (nb == nullptr) { f.close(); Serial.println("[GifPlayer] fileBuf PSRAM alloc failed"); return false; }
    s_fileBuf = nb;
    s_fileCap = pk.len;
  }
  const bool ok = f.seek(pk.off) && f.read(s_fileBuf, pk.len) == pk.len;
  f.close();
  if (ok) s_fileSize = pk.len;
  return ok;
}

static void gifTask(void*) {
  do {
    GifPick pk;
    if (!pick_random_gif(s_sceneDir, pk) || !load_pick(pk)) {
      Serial.printf("[GifPlayer] no playable gif in %s\n", s_sceneDir);
      break;
    }
    uint32_t sceneStartMs = millis();
    do {   // 同一部片重复播,撑满 GIF_SCENE_HOLD_MS 再换(短片如cc-gifs只有~1秒/圈,不加这层会乱跳)
      if (!s_gif.open(s_fileBuf, (int)s_fileSize, gifDrawCb)) {
        Serial.printf("[GifPlayer] open failed: %s\n", pk.path);
        s_stopReq = true;
        break;
      }
      int delayMs;
      while (!s_stopReq && s_gif.playFrame(true, &delayMs)) {
        // AnimatedGIF writes the scene directly to the display. Add the
        // battery only after each completed frame so GIF pixels cannot cover
        // it, and use the battery task's cached state instead of polling I2C.
        draw_battery_overlay();
      }
      s_gif.close();
    } while (!s_stopReq && !s_once && (millis() - sceneStartMs) < GIF_SCENE_HOLD_MS);
  } while (!s_stopReq && !s_once);   // 循环模式:换下一部随机片

  if (!s_theater) avatar.resumeDraw();   // 剧场模式:场景间不闪脸,屏幕定格到下一场景接管
  s_playing = false;
  vTaskDelete(nullptr);
}

static bool gif_start_internal(const char* sceneDir, bool once) {
  if (s_playing) {
    if (!once && strcmp(sceneDir, s_sceneDir) == 0) return true;   // 同场景已在播
    gif_scene_stop();
  }
  if (!ensure_frame_buf()) return false;
  static bool s_gifInit = false;
  if (!s_gifInit) {   // 真机实测(2026-07-11):LE 出紫绿花屏,此处 M5GFX pushImage 按面板序直推,要大端
    s_gif.begin(GIF_PALETTE_RGB565_BE);
    s_gifInit = true;
  }
  // 先确认场景真有素材(pak 或 SD 目录),再接管屏幕(都没有=完全不打扰原行为)
  GifPick probe;
  if (!pick_random_gif(sceneDir, probe)) return false;

  strlcpy(s_sceneDir, sceneDir, sizeof(s_sceneDir));
  s_once = once;
  s_stopReq = false;
  s_playing = true;
  avatar.pauseDraw();
  vTaskDelay(pdMS_TO_TICKS(40));   // 等 drawLoop 画完可能正在画的最后一帧(AVATAR_LOOP_DELAY_MS=33)
  if (xTaskCreate(gifTask, "gifPlay", 4096, nullptr, 2, nullptr) != pdPASS) {
    avatar.resumeDraw();
    s_playing = false;
    return false;
  }
  return true;
}

bool gif_scene_start(const char* sceneDir) { return gif_start_internal(sceneDir, false); }

bool gif_scene_play_once(const char* sceneDir) {
  if (!gif_start_internal(sceneDir, true)) return false;
  while (s_playing) vTaskDelay(pdMS_TO_TICKS(20));   // 阻塞到播完(调用方都在 loop task)
  return true;
}

void gif_scene_stop() {
  if (!s_playing) return;
  s_stopReq = true;
  while (s_playing) vTaskDelay(pdMS_TO_TICKS(10));
}
