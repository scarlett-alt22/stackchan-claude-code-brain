// Unit test for the pure AP-selection logic shipped in firmware-new/src/wifi_dual.h.
// Compile: g++ -std=c++17 -Wall -Wextra -o wifi_dual_test wifi_dual_test.cpp && ./wifi_dual_test
#include <cstdio>
#include <string>
#include <vector>
#include "../../src/wifi_dual.h"

static int failures = 0;

static void check(const char* name, const std::vector<WifiAp>& got,
                  const std::vector<std::string>& want_ssids) {
  bool ok = got.size() == want_ssids.size();
  for (size_t i = 0; ok && i < got.size(); i++) ok = got[i].ssid == want_ssids[i];
  printf("%-28s -> [", name);
  for (size_t i = 0; i < got.size(); i++) printf("%s%s", i ? ", " : "", got[i].ssid.c_str());
  printf("]  %s\n", ok ? "OK" : "FAIL");
  if (!ok) failures++;
}

int main() {
  WifiAp home{"HomeWiFi", "home-pass"};
  WifiAp hotspot{"PhoneHotspot", "hot-pass"};
  WifiAp empty{"", ""};

  check("only wifi", wifi_build_ap_list(home, empty), {"HomeWiFi"});
  check("wifi + wifi2", wifi_build_ap_list(home, hotspot), {"HomeWiFi", "PhoneHotspot"});
  check("neither configured", wifi_build_ap_list(empty, empty), {});
  check("wifi2 only", wifi_build_ap_list(empty, hotspot), {"PhoneHotspot"});
  check("duplicate ssid deduped", wifi_build_ap_list(home, home), {"HomeWiFi"});
  // open network (empty password) is still a valid candidate
  WifiAp open{"CafeFree", ""};
  check("open ap kept", wifi_build_ap_list(open, empty), {"CafeFree"});
  // passwords travel with their ssids, in order
  auto list = wifi_build_ap_list(home, hotspot);
  bool pw_ok = list.size() == 2 && list[0].password == "home-pass" && list[1].password == "hot-pass";
  printf("%-28s -> %s\n", "passwords paired in order", pw_ok ? "OK" : "FAIL");
  if (!pw_ok) failures++;

  printf(failures ? "FAILURES: %d\n" : "all tests passed\n", failures);
  return failures ? 1 : 0;
}
