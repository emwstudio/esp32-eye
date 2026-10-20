#include "state.h"

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
    auto it = counts.find(c.id);
    c.today_count = (it != counts.end()) ? it->second : 0;
    total += c.today_count;
  }
  g_state.events_today = total;
  xSemaphoreGive(state_mutex);
}

void state_bump_count(const String &cat_id) {
  xSemaphoreTake(state_mutex, portMAX_DELAY);
  CatInfo *c = find_cat(cat_id);
  if (c) {
    c->today_count++;
    g_state.events_today++;
  }
  xSemaphoreGive(state_mutex);
}
