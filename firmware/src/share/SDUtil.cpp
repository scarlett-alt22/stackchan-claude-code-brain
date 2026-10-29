
#include <Arduino.h>
#include <M5Unified.h>
#include <stdio.h>
#include <SD.h>
#include <SPI.h>
#include <SPIFFS.h>
#include <Preferences.h>

#include "SDUtil.h"

//★ OWNER 2026-07-27: 见 SDUtil.h 里那段长注释(CoreS3 的 MISO/DC 冲突)。
// CoreS3 的 SD 引脚, 官方文档钉死的四个值。
#define SD_SPI_SCK_PIN  36
#define SD_SPI_MISO_PIN 35
#define SD_SPI_MOSI_PIN 37
#define SD_SPI_CS_PIN    4

//★ OWNER 2026-07-28: A/B 开关(存 NVS, 下次开机生效)。见 SDUtil.h 的理由。
static int s_spiBeginArm = -1;   //-1=还没读过, 0=off(旧行为), 1=on(07-27 修复)

bool sd_spi_begin_enabled()
{
  if (s_spiBeginArm < 0) {
    Preferences p;
    // 只读打开: 键不存在时返回默认值 true, 不会写 NVS。
    if (p.begin("sdcfg", true)) {
      s_spiBeginArm = p.getBool("spibegin", true) ? 1 : 0;
      p.end();
    } else {
      s_spiBeginArm = 1;   //命名空间还没建过 -> 默认走修复后的行为
    }
  }
  return s_spiBeginArm == 1;
}

void sd_spi_begin_set(bool enabled)
{
  Preferences p;
  if (!p.begin("sdcfg", false)) {
    Serial.println("[SDcfg] ❌ NVS 打不开");
    return;
  }
  p.putBool("spibegin", enabled);
  p.end();
  s_spiBeginArm = enabled ? 1 : 0;
  Serial.printf("[SDcfg] SPI.begin() 已设为 %s —— 🔴 复位后才生效\n",
                enabled ? "on(07-27 修复)" : "off(07-27 之前的行为)");
}

bool sd_begin_cores3(uint32_t clockHz)
{
  // 🔴 顺序不能反: 必须先按 CoreS3 引脚初始化 SPI, 再 SD.begin()。
  // 反过来(也就是本仓库 07-27 之前的做法)会让 SD 用上屏幕驱动留下的总线状态。
  //
  // ⚠️ 07-28: 这个"反过来更差"目前**只是假设, 没有干净证据**。修复上线那天
  // 失败率反而从 45% 涨到 83%, 当时归因给温度; 07-28 冷机实测 76% —— 温度被
  // 推翻, 于是"修复本身是负面的"重新成为活着的嫌疑。arm 开关就是为了量它:
  // 两臂时钟都锁 10MHz, 唯一变量是这一行 SPI.begin(), 跨开机交替跑。
  const bool arm = sd_spi_begin_enabled();
  // 每次开机打一行, 让 Mac 侧脚本能**验证 arm 真的生效了**, 而不是信自己刚设过。
  static bool s_announced = false;
  if (!s_announced) {
    s_announced = true;
    Serial.printf("[SDcfg] arm=%s clk=%.1fMHz\n", arm ? "on" : "off", clockHz / 1e6);
  }
  if (arm) {
    SPI.begin(SD_SPI_SCK_PIN, SD_SPI_MISO_PIN, SD_SPI_MOSI_PIN, SD_SPI_CS_PIN);
  }
  return SD.begin(SD_SPI_CS_PIN, SPI, clockHz);
}

int read_sd_file(const char* filename, char* buf, int buf_size)
{
  int ret;
  auto fs = SD.open(filename, FILE_READ);
  
  if(fs) {
    size_t sz = fs.size();
    if(sz >= buf_size){
      Serial.printf("SD file size (%d) exceeds buf size (%d).\n", sz, buf_size);
      ret = 0;
    }
    else{
      fs.read((uint8_t*)buf, sz);
      buf[sz] = 0;
      fs.close();
      ret = sz;
    }
  }
  else{
    Serial.println("Failed to open SD.");
    ret = 0;
  }
  return ret;
}


int read_line_from_buf(char* buf, char* out)
{
  static int n_total = 0;
  static char* buf_ = nullptr;
  int n, ret;

  if(buf != nullptr){
    buf_ = buf;
  }

  if(buf_ == nullptr){
    Serial.println("read_line_from_buf: buffer is nullptr.");
    return 0;
  }

  ret = sscanf(buf_, "%s%n", out, &n);
  buf_ += n;

  return ret;
}


// SDカードのファイルをSPIFFSにコピー
bool copySDFileToSPIFFS(const char *path, bool forced) {
  
  Serial.println("Copy SD File to SPIFFS.");

  if(!SPIFFS.begin(true)){
    Serial.println("Failed to mount SPIFFS.");
    return false;
  }

  if (SPIFFS.exists(path) && !forced) {
	  Serial.println("File already exists in SPIFFS.");
	  return true;
  }

  //★ OWNER 2026-07-26: SD SPI 时钟 25MHz -> 10MHz。
  // 起因: 串口反复刷 `sd_diskio.cpp:621 ff_sd_status(): Check status failed`,
  // 表现是目录偶尔扫成空/少读文件(实测 pet_back 4 个只读到 3 个)。
  // **在刚格式化的新卡上照样复现**, 所以不是文件系统坏, 是这条读路本身不稳 ——
  // CoreS3 的 SD 走 SPI 且跟屏幕共总线, 25MHz 偏激进。
  // 降速够不够用(算过): 10MHz 实测 600~800KB/s; 最重的需求是加载一个 90KB 的 GIF
  // (约 150ms, 每 20 秒才一次), 语音播放只吃 48KB/s。余量很大, 不影响任何体验。
  if (!sd_begin_cores3(10000000)) {
    Serial.println("Failed to mount SD.");
    return false;
  }

  File fsrc = SD.open(path, FILE_READ);
  File fdst = SPIFFS.open(path, FILE_WRITE);
  if(!fsrc || !fdst) {
    Serial.println("Failed to open file.");
    return false;
  }

  uint8_t *buf = new uint8_t[4096];
  if (!buf) {
	  Serial.println("Failed to allocate buffer.");
	  return false;
  }

  size_t len, size, ret;
  size = len = fsrc.size();
  while (len) {
	  size_t s = len;
	  if (s > 4096){
		  s = 4096;
    }  

	  fsrc.read(buf, s);
	  if ((ret = fdst.write(buf, s)) < s) {
		  Serial.printf("write failed: %d - %d\n", ret, s);
		  return false;
	  }
	  len -= s;
	  Serial.printf("%d / %d\n", size - len, size);
  }
 
  delete[] buf;
  fsrc.close();
  fdst.close();

  if (!SPIFFS.exists(path)) {
	  Serial.println("no file in SPIFFS.");
	  return false;
  }
  fdst = SPIFFS.open(path);
  len = fdst.size();
  fdst.close();
  if (len != size) {
	 Serial.println("size not match.");
	 return false;
  }
  Serial.println("*** Done. ***\r\n");
  return true;
}

