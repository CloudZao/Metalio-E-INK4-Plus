#pragma once

/** @brief 发布网络授时成功并通知板级写回RTC；云端授时与NTP共用。 */
void TimeServiceCommitNetworkTime();

/** @brief 按联网、配网及授时状态决定是否执行NTP，并持有同步期间的网络占用。 */
void TimeServiceSyncNetwork();
