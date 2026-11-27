# 摄像与图片

**中文** | [English](README.md)

用于验证 ELF 图片显示、图像处理、摄像头采集及完整刷新能力的示例应用。中间区域显示摄像头取景或内置图片，底部提供显示模式和处理参数控制。支持中英文界面，跟随宿主语言。

应用提供取景和显示效果调整，不保存照片或录像，也不包含相册管理。软件曝光等参数只处理显示图像，不修改摄像头传感器设置。

## 使用

- 图像来源：启动时选择摄像头；点击 JPEG、PNG、A2I1 或 A2L8 切换到对应的内置图片。图片资源随 ELF 打包，无需另外复制到 SD 卡。
- 显示模式：选择 16 灰、8 灰、抖点或黑白。397 默认使用抖点，选择不支持的灰阶模式会返回错误并保留原模式。
- 参数：用“上一项／下一项”切换对比度、亮度、软件曝光、阴影、高光、Gamma、清晰度、旋转、中心裁剪、水平镜像、垂直镜像和反相，再用“−／+”调整。清晰度负值为柔化，正值为锐化；界面不提供黑白阈值调节。
- 恢复默认：恢复中性处理参数、完整裁剪区域和板型默认显示模式。摄像头模式下可暂停／继续；图片模式下对应按钮用于重载当前图片。全刷按钮请求当前画面完整刷新。

对比度、亮度、阴影、高光和清晰度默认 0%，界面步长为 25%；软件曝光默认 0 EV，步长 0.25 EV；Gamma 默认 1.00，步长 0.25。旋转步长 90 度；中心裁剪以 10% 为步长保留原图宽高的 20%—100%；镜像和反相使用“−”关闭、“+”开启。这些是示例界面的调节步长，宿主接口的参数精度与范围见[图片与显示效果](../README_zn.md#图片与显示效果)。

取景交付后至少等待 800 ms 再采集，实际显示速度还受摄像头和墨水屏刷新限制。暂停保留已显示帧，仍可调整效果；待机、离页和退出会取消采集，唤醒且页面可交互后按原需求恢复。采集失败后可重新选择摄像头重试。

## 图片素材

[build_content.py](build_content.py) 使用仓库现有图片，保持比例缩小后编码为四个资源：JPEG 来自 `docs/images/product.png`，PNG 来自 `docs/images/product_en.png`，A2I1 和 A2L8 共用 `tools/a2i1/bg_shutdown2.jpg`。上述路径均相对仓库根目录。

A2I1 源图已二值化，切换灰阶模式不能恢复丢失的层次。脚本生成 `assets.bin`、`image_assets.h` 和 `icon.pbm`；生成文件不纳入版本控制。

## 构建与安装

需要 Python、Pillow 以及与目标匹配的 ESP 工具链。按[开发手册](../README_zn.md#2-快速开始)配置环境，在仓库根目录执行，并将编译器占位替换为实际路径：

```powershell
# 470：ESP32-S31 / RISC-V
./tools/elf_app/build_app.ps1 -Compiler '<riscv32-esp-elf-gcc.exe 的完整路径>' -AppDirectory ./tools/elf_app/imaging -Target esp32s31 -OutputDirectory ./tools/elf_app/imaging/build/470

# 397：ESP32-S3 / Xtensa
./tools/elf_app/build_app.ps1 -Compiler '<xtensa-esp32s3-elf-gcc.exe 的完整路径>' -AppDirectory ./tools/elf_app/imaging -Target esp32s3 -OutputDirectory ./tools/elf_app/imaging/build/397
```

两版分别输出到 `build/470/imaging.elf` 和 `build/397/imaging.elf`。脚本生成素材并嵌入图标，不生成安装 JSON。手动安装时，将 `imaging.elf` 与匹配的 `manifest.json` 放在 SD 卡的 `metalio/e-ink/apps/imaging/`；清单的 `installedName` 使用 `imaging`，470 的 `target` 为 `esp32s31`，397 为 `esp32s3`。重建后同步清单大小与 SHA256，也可通过应用商店发布。

## 兼容性与限制

需要 ABI 1，以及 `XZ_APP_CAP_IMAGE`、`XZ_APP_CAP_IMAGE_TONE`、`XZ_APP_CAP_CAMERA` 和 `XZ_APP_CAP_FULL_REFRESH`。宿主内容区至少为 480×600；能力或接口不足时入口返回 `XZ_APP_ERR_UNSUPPORTED`。支持接口不表示摄像头已经连接，内置图片可在未连接摄像头时使用。

470 支持灰阶显示；397 原生黑白，不支持 16/8 灰阶模式，两种目标架构的 ELF 不能混用。397 的 USB 摄像头与 USB 虚拟磁盘互斥。8 灰模式只改变图像量化，不保证更快刷新；全刷为异步请求，不代表推白动画或视频帧率。

完整的接口参数、格式限制和生命周期约定见[图片与显示效果](../README_zn.md#图片与显示效果)及[摄像头取景与完整刷新](../README_zn.md#摄像头取景与完整刷新)。



## 源码与调用流程

`imaging.c` 管理界面、图像来源、处理参数和状态轮询；`build_content.py` 管理测试素材及图标生成，`image_assets.h` 只保存资源范围。

1. `app_main` 检查 ABI、接口字段、能力和内容区，创建图片控件与参数控件，注册托管定时器并选择初始来源。
2. `SelectSource` 切换摄像头或资源图片。资源图片以偏移、长度调用 `image_load_asset`；该调用成功仅表示请求接纳，`Tick` 读取状态并更新结果提示。
3. `SelectAdjustment` 选择处理项，`AdjustValue` 修改应用保存的参数，`ApplyEffects` 调用 `image_set_effects`。失败时显示结果码；处理范围与次序由公共接口定义。
4. `Pause` 暂停或恢复取景；图片模式下重载资源。`Refresh` 请求异步全刷，不能视为物理刷新已完成。
5. 摄像头、图片任务、控件和定时器的待机与退出收尾由宿主管理；应用不直接访问硬件或保留退出后的接口引用。

## 接口使用示例

下面片段展示已完成能力检查、持有有效 `api` 和 `picture` 后的处理步骤；示例不替代应用初始化：

```c
xz_app_image_effects_t effects = {0};
effects.struct_size = sizeof(effects);
int result = api->image_get_effects(api->host_context, picture, &effects);
if (result == XZ_APP_OK) {
    effects.rotation = 90;
    effects.brightness = 25;
    result = api->image_set_effects(api->host_context, picture, &effects);
}
```

源图选择 JPEG，恢复默认后增加亮度并旋转 90 度，可检查方向与调色；再切换 A2I1，可观察二值源图无法恢复灰阶。摄像头不可用时改用内置图片，采集恢复后重新选择摄像头。完整参数、异步结果与资源限制见[公共图片接口](../README_zn.md#图片与显示效果)。
