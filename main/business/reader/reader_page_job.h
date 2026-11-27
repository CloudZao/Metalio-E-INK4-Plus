#pragma once
#include <atomic>
#include "business/reader/book_session.h"

enum class ReaderPageStage : uint8_t {
    kIdle, // 没有在途准备任务
    kNavigation, // 导航仍可能访问源会话
    kBackground, // 生成独立背景
    kFont, // 预热当前游标的字体
    kRaster, // 生成独立正文栅格
    kFinishing, // 任务释放资源并交付结果
};

struct ReaderPageResult {
    std::unique_ptr<BookSession> cursor;      // 独立导航与页面所有权
    std::shared_ptr<RasterImage> background;  // 背景几何变化时生成，其余页面共享
    RasterImage raster;                       // 后台完成的正文像素
    std::vector<int> image_tops;              // 与页内插图顺序一致的顶部坐标
    lv_coord_t column_width = 0;              // 正文实际列宽
    int ink_height = 0;                       // 正文墨迹高度
    int value = 0;                            // 请求参数
    int direction = 0;                        // 实际源位置的移动方向，与正文像素一同发布
    BookSession::NavigationKind kind = BookSession::NavigationKind::kRefresh;  // 请求类型
    uint32_t reclaim_generation = 0; // 可选结果所属的内存回收代次
    uint32_t token = 0;                                                        // 会话代次
    int64_t navigation_us = 0, raster_us = 0, queue_us = 0;                    // 后台各阶段耗时
    int64_t started_us = 0;                                                    // 请求被接受时刻
    bool navigation_ready = false; // 已物化目标页并关闭导航文件句柄，可直接补绘
    bool chapter_only = false; // 本次只准备章节分页，暂不生成栅格
    bool ok = false;                                                           // 导航和物化结果
    bool dark = false;                                                         // 准备页面时的界面明暗快照
    bool deferred = false; // 预取因公共需求或执行权忙而延后，允许稍后重试
    bool prefetch = false;                                                     // 仅预取，不发布位置或保存阅读进度
};

struct ReaderPageJob {
    std::atomic<bool>* loading_chapter = nullptr; // 由页面工作模块持有，任务结束前有效
    void (*completed)(std::unique_ptr<ReaderPageResult>) = nullptr; // 后台完成后交给接收端
    bool (*prewarm)(const Page*, const std::atomic<bool>*) = nullptr; // 绘制层的字形预热入口
    std::unique_ptr<ReaderPageResult> result;               // 完成后转交 UI 的结果
    const lv_font_t* font = nullptr;                        // 工作期间保持存活的正文字体
    int width = 0, height = 0, underline = 0;               // 分页视口和下划线模式
    int background = 0, text_left = 0, text_top = 0;        // 背景模式与正文屏幕坐标
    int screen_width = 0, screen_height = 0;                // 整页背景尺寸
    TextStyle style;                                        // 布局快照及本次绘制的装饰参数
    std::shared_ptr<RasterImage> background_image;          // 页面准备期间保持背景存活
    bool antialias = false, vertical = false, rtl = false;  // 抗锯齿、竖排和列方向
};

/** @brief 接纳独立正文准备，由工作任务完成导航与绘制。 */
bool ReaderPreparePage(std::unique_ptr<ReaderPageJob> work);

/** @brief 返回在途准备阶段，只有导航阶段仍会访问源会话。 */
ReaderPageStage ReaderPagePreparationStage();

/** @brief 将正在准备的前台目标提升为前台优先级，在下一个阶段边界生效。 */
void PromoteReaderPagePreparation();

/** @brief 请求在导航、字形预热及绘制安全点取消当前正文准备。 */
void CancelReaderPagePreparation();
