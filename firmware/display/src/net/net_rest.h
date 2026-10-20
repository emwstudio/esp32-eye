#pragma once

#include <Arduino.h>
#include "state.h"

// REST API（docs/api-contract.md）。所有函数阻塞，应在网络任务中调用。

String server_ws_base();  // SERVER_BASE 的 ws:// 形式

bool rest_health();
bool rest_fetch_cats();                                        // 拉取档案并缓存头像
bool rest_fetch_today();                                       // 拉今日计数
bool rest_fetch_clips(const String &cat_id, const String &date, std::vector<ClipInfo> &out);

// 解码并缓存头像（PSRAM RGB565 缓冲），返回图像描述指针（失败返回空）
void *avatar_img_dsc(const String &cat_id);
void avatar_cache_clear();
