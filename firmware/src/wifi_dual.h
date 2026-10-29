#pragma once
// ★ OWNER 2026-10-04: 双 Wi-Fi 选网纯逻辑（不依赖 Arduino，可在 Linux 上 g++ 单测）。
// 输入 yaml 里配好的两个 AP，输出去重后的有序候选列表，交给 WiFiMulti 扫描后
// 连信号最好的一个。wifi 必选、wifi2 可选；都没配 -> 空列表 -> 走 SmartConfig。
#include <string>
#include <vector>

struct WifiAp {
  std::string ssid;
  std::string password;
};

inline std::vector<WifiAp> wifi_build_ap_list(const WifiAp& wifi, const WifiAp& wifi2) {
  std::vector<WifiAp> out;
  if (!wifi.ssid.empty()) {
    out.push_back(wifi);
  }
  // wifi2 为空 = 没配；与 wifi 同 SSID = 去重。空密码的开放网络照样加入。
  if (!wifi2.ssid.empty() && wifi2.ssid != wifi.ssid) {
    out.push_back(wifi2);
  }
  return out;
}
