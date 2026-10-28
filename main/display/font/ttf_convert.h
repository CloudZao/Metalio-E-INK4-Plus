#pragma once

#include <cstddef>
#include <cstdint>

/** 设备端 TTF/OTF → .ef 转换（后台任务，UI 轮询原子状态）。 */
namespace ttf_convert {

/**
 * @brief 启动转换：读 template_ef_path 的字集，渲染 sizes_px 各档 2bpp，
 * 输出 {family}_{px}_2.ef 到 out_dir。family 取 TTF 文件名 stem。
 * size_count 通常为 1（UI 默认 40px，可选 20–80）；最多 3。
 * 运行中重复调用返回 false。失败原因经 Error() 获取。
 */
bool Start(const char* ttf_path, const char* template_ef_path, const char* out_dir,
           const uint16_t* sizes_px, int size_count);

/** @brief 查询转换状态：0 空闲 / 1 转换中 / 2 完成 / 3 失败 */
int State();

/** @brief 查询转换进度，0-100（按已渲染码点/总码点×档数） */
int Percent();

/** @brief 获取失败时的错误信息（静态缓冲，≤64B） */
const char* Error();

/** @brief 查询后台转换任务是否仍在运行或收尾。 */
bool Busy();

/** @brief 请求取消转换，由后台任务释放资源后退出。 */
void Cancel();

/** @brief 生成输出名前缀：TTF/OTF 文件名 stem 净化（与 Start 写入规则一致）。 */
void FamilyFromPath(const char* ttf_path, char* out, size_t out_len);

/**
 * @brief 写入输出文件名清单到 buf，每行一个（如 fam_25_2.ef）。
 * sizes 为空或 size_count<=0 时默认单档 25px。
 */
void FormatOutputFileList(const char* ttf_path, const uint16_t* sizes_px, int size_count,
                          char* buf, size_t buf_len);

}  // namespace ttf_convert
