#pragma once

#include <Arduino.h>
#include <vector>
#include <map>

// 全局应用状态（网络任务写，UI 读，持 state_mutex 访问）

struct CatInfo {
  String id;
  String name;
  String avatar_path;   // LittleFS 缓存路径，空 = 未缓存
  int today_count = 0;
};

struct ClipInfo {
  long id = 0;
  String cat_id;
  double start_ts = 0;
  double duration_s = 0;
  String status;
};

struct AppState {
  std::vector<CatInfo> cats;
  String today_date;
  int events_today = 0;
  bool wifi_ok = false;
  bool server_ok = false;
  bool ws_ok = false;
  bool offline_shown = false;  // 离线页是否正在显示（UI 任务写，健康任务读）
  int pending_event_id = -1;   // confirming 中的事件
  String pending_cat_id;       // cat_confirmed 后对应猫
};

extern AppState g_state;
extern SemaphoreHandle_t state_mutex;

void state_init();

CatInfo *find_cat(const String &id);
void state_set_counts(const String &date, const std::map<String, int> &counts);
void state_bump_count(const String &cat_id);
