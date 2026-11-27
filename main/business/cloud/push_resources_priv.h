#pragma once

#include <cstdint>
#include <string>
#include "business/cloud/push_resources_library.h"

class Http;

#include <cJSON.h>

constexpr const char* kPushResourcesTag = "PushRes";
constexpr int kPushResourcesHttpTimeoutMs = 30000;
