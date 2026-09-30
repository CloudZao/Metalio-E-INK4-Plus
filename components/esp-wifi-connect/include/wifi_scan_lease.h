#ifndef WIFI_SCAN_LEASE_H_
#define WIFI_SCAN_LEASE_H_

/**
 * 主动扫网结果租约（全机唯一协作点）。
 *
 * ESP-IDF：同一次 WIFI_EVENT_SCAN_DONE 的 AP 表只能被 esp_wifi_scan_get_ap_records 取走一次。
 * WifiStation / WifiConfigurationAp 等消费者在 IsHeld() 时必须跳过取表，把结果留给租约持有者；
 * 跳过时仍应自行维护各自的扫网定时器，避免租约期间断档。
 *
 * 持有方（如产测独立扫网）须保证：TryAcquire → scan_stop → scan_start → 在 handler 内取表 → Release。
 * 任意失败路径都要 Release，禁止跨模块长期占租约。
 */
namespace WifiScanLease {

bool TryAcquire(const char* owner_tag);
void Release();
bool IsHeld();

}  // namespace WifiScanLease

#endif  // WIFI_SCAN_LEASE_H_
