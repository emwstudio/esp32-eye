#include "state.h"
#include "config.h"

AppState g_state;
SemaphoreHandle_t state_mutex = NULL;

void state_init() { state_mutex = xSemaphoreCreateMutex(); }

CatInfo *find_cat(const String &id) {
  for (auto &c : g_state.cats) {
    if (c.id == id) return &c;
  }
  return nullptr;
}

void state_set_counts(const String &date, const std::map<String, int> &counts) {
  xSemaphoreTake(state_mutex, portMAX_DELAY);
  g_state.today_date = date;
  int total = 0;
  for (auto &c : g_state.cats) {
#if DEMO_MODE
    if (c.id == "baobao") c.today_count = 5;
    else if (c.id == "bobo") c.today_count = 4;
    else if (c.id == "qiuqiu") c.today_count = 6;
    else if (c.id == "shandian") c.today_count = 3;
    else c.today_count = 0;
#else
    auto it = counts.find(c.id);
    c.today_count = (it != counts.end()) ? it->second : 0;
#endif
    total += c.today_count;
  }
  g_state.events_today = total;
  xSemaphoreGive(state_mutex);
}

void state_bump_count(const String &cat_id) {
#if DEMO_MODE
  return;
#endif
  xSemaphoreTake(state_mutex, portMAX_DELAY);
  CatInfo *c = find_cat(cat_id);
  if (c) {
    c->today_count++;
    g_state.events_today++;
  }
  xSemaphoreGive(state_mutex);
}
