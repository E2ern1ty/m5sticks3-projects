#pragma once
// 复制为 secrets.h 填入真实值（secrets.h 已被 .gitignore 排除）
//
// R2 密钥：Cloudflare 控制台 -> R2 -> 管理 API 令牌 -> 创建（对象读写）
//   得到 Access Key ID / Secret Access Key

static const char *WIFI_SSID = "your-wifi-name";
static const char *WIFI_PASS = "your-wifi-password";

static const char *R2_HOST = "a484e848424b32442bf4ef50dd53ea9a.r2.cloudflarestorage.com";
static const char *R2_BUCKET = "test";
static const char *R2_PREFIX = "stick3";
static const char *R2_ACCESS_KEY = "your-r2-access-key";
static const char *R2_SECRET = "your-r2-secret";
