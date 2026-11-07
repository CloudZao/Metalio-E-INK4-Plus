#pragma once

#include "cloud_screen/push/push_resources_library.h"

#include <cstdint>
#include <string>

class Http;

#include <cJSON.h>

namespace reader {

constexpr const char* TAG = "PushRes";
constexpr int kHttpTimeoutMs = 30000;

}  // namespace reader
