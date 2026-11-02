#include "epd_flash_guard.h"
#include "metalio_i2c1.h"

#include <cstdlib>
#include <nvs.h>
#include <nvs_flash.h>

namespace {
unsigned s_nvs_depth; // Flash 门闩串行化所有任务，非零层数只可能属于当前持有任务

class NvsAccessGuard {
public:
    NvsAccessGuard() {
        // 锁序固定为 Flash → I2C → NVS，普通 I2C 持锁区不得调用 NVS。
        // 仅最外层排空 I2C；SDK 内部 API 嵌套不能重复取得非递归总线锁。
        if (s_nvs_depth++ == 0 && !metalio_i2c1_lock_for_flash(-1)) {
            std::abort();
        }
    }

    ~NvsAccessGuard() {
        if (--s_nvs_depth == 0) metalio_i2c1_unlock();
    }

private:
    EpdFlashGuard flash_guard_;
};
}

// NVS 的查找和读取也可能访问 Flash，相关 C 入口须在 SDK 取得 NVS 锁之前接纳。
#define EPD_NVS_WRAP(result, name, params, args) \
    extern "C" result __real_##name params; \
    extern "C" result __wrap_##name params { \
        NvsAccessGuard guard; \
        return __real_##name args; \
    }

#define EPD_NVS_SCALAR(suffix, type) \
    EPD_NVS_WRAP(esp_err_t, nvs_set_##suffix, \
                 (nvs_handle_t handle, const char* key, type value), (handle, key, value)) \
    EPD_NVS_WRAP(esp_err_t, nvs_get_##suffix, \
                 (nvs_handle_t handle, const char* key, type* value), (handle, key, value))

EPD_NVS_SCALAR(i8, int8_t)
EPD_NVS_SCALAR(u8, uint8_t)
EPD_NVS_SCALAR(i16, int16_t)
EPD_NVS_SCALAR(u16, uint16_t)
EPD_NVS_SCALAR(i32, int32_t)
EPD_NVS_SCALAR(u32, uint32_t)
EPD_NVS_SCALAR(i64, int64_t)
EPD_NVS_SCALAR(u64, uint64_t)
EPD_NVS_SCALAR(float, float)
EPD_NVS_SCALAR(double, double)

EPD_NVS_WRAP(esp_err_t, nvs_open,
             (const char* ns, nvs_open_mode_t mode, nvs_handle_t* handle), (ns, mode, handle))
EPD_NVS_WRAP(esp_err_t, nvs_open_from_partition,
             (const char* part, const char* ns, nvs_open_mode_t mode, nvs_handle_t* handle),
             (part, ns, mode, handle))
EPD_NVS_WRAP(void, nvs_close, (nvs_handle_t handle), (handle))
EPD_NVS_WRAP(esp_err_t, nvs_commit, (nvs_handle_t handle), (handle))
EPD_NVS_WRAP(esp_err_t, nvs_set_str,
             (nvs_handle_t handle, const char* key, const char* value), (handle, key, value))
EPD_NVS_WRAP(esp_err_t, nvs_set_blob,
             (nvs_handle_t handle, const char* key, const void* value, size_t size),
             (handle, key, value, size))
EPD_NVS_WRAP(esp_err_t, nvs_get_str,
             (nvs_handle_t handle, const char* key, char* value, size_t* size),
             (handle, key, value, size))
EPD_NVS_WRAP(esp_err_t, nvs_get_blob,
             (nvs_handle_t handle, const char* key, void* value, size_t* size),
             (handle, key, value, size))
EPD_NVS_WRAP(esp_err_t, nvs_find_key,
             (nvs_handle_t handle, const char* key, nvs_type_t* type), (handle, key, type))
EPD_NVS_WRAP(esp_err_t, nvs_erase_key,
             (nvs_handle_t handle, const char* key), (handle, key))
EPD_NVS_WRAP(esp_err_t, nvs_erase_all, (nvs_handle_t handle), (handle))
EPD_NVS_WRAP(esp_err_t, nvs_purge_all, (nvs_handle_t handle), (handle))
EPD_NVS_WRAP(esp_err_t, nvs_get_stats,
             (const char* part, nvs_stats_t* stats), (part, stats))
EPD_NVS_WRAP(esp_err_t, nvs_get_used_entry_count,
             (nvs_handle_t handle, size_t* count), (handle, count))
EPD_NVS_WRAP(esp_err_t, nvs_entry_find,
             (const char* part, const char* ns, nvs_type_t type, nvs_iterator_t* iterator),
             (part, ns, type, iterator))
EPD_NVS_WRAP(esp_err_t, nvs_entry_find_in_handle,
             (nvs_handle_t handle, nvs_type_t type, nvs_iterator_t* iterator),
             (handle, type, iterator))
EPD_NVS_WRAP(esp_err_t, nvs_entry_next, (nvs_iterator_t* iterator), (iterator))
EPD_NVS_WRAP(esp_err_t, nvs_entry_info,
             (nvs_iterator_t iterator, nvs_entry_info_t* info), (iterator, info))
EPD_NVS_WRAP(void, nvs_release_iterator, (nvs_iterator_t iterator), (iterator))

EPD_NVS_WRAP(esp_err_t, nvs_flash_init, (void), ())
EPD_NVS_WRAP(esp_err_t, nvs_flash_init_partition, (const char* part), (part))
EPD_NVS_WRAP(esp_err_t, nvs_flash_init_partition_ptr,
             (const esp_partition_t* part), (part))
#if CONFIG_NVS_BDL_STACK
EPD_NVS_WRAP(esp_err_t, nvs_flash_init_partition_bdl,
             (const char* part, esp_blockdev_handle_t bdl), (part, bdl))
#endif
EPD_NVS_WRAP(esp_err_t, nvs_flash_deinit, (void), ())
EPD_NVS_WRAP(esp_err_t, nvs_flash_deinit_partition, (const char* part), (part))
EPD_NVS_WRAP(esp_err_t, nvs_flash_erase, (void), ())
EPD_NVS_WRAP(esp_err_t, nvs_flash_erase_partition, (const char* part), (part))
EPD_NVS_WRAP(esp_err_t, nvs_flash_erase_partition_ptr,
             (const esp_partition_t* part), (part))
EPD_NVS_WRAP(esp_err_t, nvs_flash_secure_init, (nvs_sec_cfg_t* cfg), (cfg))
EPD_NVS_WRAP(esp_err_t, nvs_flash_secure_init_partition,
             (const char* part, nvs_sec_cfg_t* cfg), (part, cfg))
EPD_NVS_WRAP(esp_err_t, nvs_flash_generate_keys,
             (const esp_partition_t* part, nvs_sec_cfg_t* cfg), (part, cfg))
EPD_NVS_WRAP(esp_err_t, nvs_flash_read_security_cfg,
             (const esp_partition_t* part, nvs_sec_cfg_t* cfg), (part, cfg))
EPD_NVS_WRAP(esp_err_t, nvs_flash_generate_keys_v2,
             (nvs_sec_scheme_t* scheme, nvs_sec_cfg_t* cfg), (scheme, cfg))
EPD_NVS_WRAP(esp_err_t, nvs_flash_read_security_cfg_v2,
             (nvs_sec_scheme_t* scheme, nvs_sec_cfg_t* cfg), (scheme, cfg))

#undef EPD_NVS_SCALAR
#undef EPD_NVS_WRAP
