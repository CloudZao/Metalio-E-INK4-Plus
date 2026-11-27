#pragma once
#include <lvgl.h>
#include "business/app_store/app_store.h"

class AppStoreScreen {
public:
    /** @brief 创建应用商店及已安装应用管理页 */
    static lv_obj_t* Create();

    /** @brief 待机时取消在途请求并释放保网，唤醒后恢复前台保网 */
    static void SetStandby(bool standby);

    /** @brief 异步打开指定的本机应用 */
    static void Launch(const AppStoreItem& item);
};
