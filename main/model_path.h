#pragma once

/** @brief esp-sr types stub（S31 无 AFE） */
typedef struct srmodel_list {
    int dummy;
} srmodel_list_t;

#ifdef __cplusplus
extern "C" {
#endif

static inline srmodel_list_t* esp_srmodel_init(const char* /*partition*/) {
    return nullptr;
}

static inline void esp_srmodel_deinit(srmodel_list_t* /*models*/) {}

#ifdef __cplusplus
}
#endif
