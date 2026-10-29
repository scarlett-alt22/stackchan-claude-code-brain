// Clawd GIF 小剧场 (OWNER 2026-07-11; 同日 v2 升级剧场模式)
// v2 剧场模式: 屏幕常态=小剧场,橙脸退役为无SD回退。开机 gif_theater_begin() 探到 idle
// 素材即开启;开启后 avatar 永不 resume(场景切换之间不闪脸),按状态切场景:
//   idle(闲逛) / listening(听主人说话) / thinking(等LLM) / talking(念回复) / happy / error
// 素材(2026-07-28 起两级): ①主路 SPIFFS /g/<scene>.pak(内部 flash, 不受 SD 坏模式
//       影响——白脸治本; Mac 侧 tools-gif/push_gifpaks.sh 打包推送) ②回退 SD
//       /app/AiStackChanEx/gif/<scene>/*.gif。素材 320x240, 来自 clawd-on-desk /
//       cc-gifs, 经 Mac 侧脚本铺奶白底+缩放; 版权归 Anthropic/原作者, 不入 git。
// pak 和 SD 都探不到素材时所有接口静默失败:剧场不开=行为退回经典奶白脸,
// AiStackChanMod::idle() 会每 15s 低频重试把剧场找回来(白脸不再一锁一整轮开机)。
#ifndef _GIF_PLAYER_H_
#define _GIF_PLAYER_H_

#include <Arduino.h>

#define GIF_SCENE_ROOT      "/app/AiStackChanEx/gif"
#define GIF_SCENE_THINKING  GIF_SCENE_ROOT "/thinking"
#define GIF_SCENE_IDLE      GIF_SCENE_ROOT "/idle"
#define GIF_SCENE_ERROR     GIF_SCENE_ROOT "/error"
#define GIF_SCENE_HAPPY     GIF_SCENE_ROOT "/happy"
#define GIF_SCENE_LISTENING GIF_SCENE_ROOT "/listening"
#define GIF_SCENE_TALKING   GIF_SCENE_ROOT "/talking"
#define GIF_SCENE_DIZZY     GIF_SCENE_ROOT "/dizzy"
//★ OWNER 2026-07-26: 逆毛专属。之前逆毛借的是 dizzy —— 但 dizzy 是"被摇晃"的反应,
//  两个动作共用一个画面就等于没分开。素材 clawd-react-annoyed.gif(生气)。
#define GIF_SCENE_ANNOYED   GIF_SCENE_ROOT "/annoyed"

// GIF_HAPPY_CHANCE(1/5 骰子)已于 2026-07-13 删除: happy 改由 Claude 回复的首字 emoji 触发
// (见 Robot::speech 的 strip_leading_emoji),不再随机。触发频率靠 prompt 调,不用重烧。
#define GIF_SCENE_HOLD_MS     20000UL                 // 循环模式同一部片最少播多久再随机换片

// 循环播放场景目录里随机挑的 GIF,播完一个换下一个(幂等:同场景已在播则直接返回)
bool gif_scene_start(const char* sceneDir);
// 停止播放并恢复 avatar(阻塞到播放任务真正退出;未在播则no-op)
void gif_scene_stop();
// 播一遍随机挑的 GIF(阻塞到播完,自动恢复 avatar)
bool gif_scene_play_once(const char* sceneDir);
bool gif_scene_is_playing();

// 剧场模式开关。begin 在 setup() 末尾调(mod->init() 早于 avatar.init(16),不能挂那);
// 切走 mod 时 end 交还屏幕,切回时 mod::init 再 begin。闲时回 idle 场景由 mod::idle 维持
bool gif_theater_begin();
void gif_theater_end();
bool gif_theater_on();

#endif //_GIF_PLAYER_H_
