# 录音机

**中文** | [English](README.md)

单文件 ELF 示例，展示宿主录音与 WAV 播放接口。支持开始录音、暂停、继续、停止并保存；显示实际录入的秒数、已保存路径和总时长。保存后可播放、暂停、继续及停止，进度条显示播放进度，不支持拖动定位。音量使用系统音量。

重新录音会在保存成功后覆盖上一条；准备失败、空录音或写入失败保留上一条。没有多录音列表、重命名或删除入口。仅支持 16kHz、单声道、16 位 PCM WAV，不提供录音增益、编码格式或采样率调节。

正式文件位于 `settings/apps/<应用名称>--<appId>/recording.wav`，相对 `metalio/e-ink/`；临时文件位于对应 `temp/app_store/<应用名称>--<appId>/recording.wav.part`。设置、使用记录和录音共用现有应用私有数据目录，不新增顶层目录。更新保留录音；删除应用数据或卸载应用时与私有数据一起清理。录制期间不要通过 USB 或其他工具修改源文件。

录音与播放互斥。点击重新录音先停止播放，等待归还硬件后开始。录音及暂停期间阻止无操作自动待机；电源键待机、退出或离开前台会结束并保存录音，唤醒后不会自行开始新录音。系统音频优先，抢占导致本次录音失败，保留上一条。播放复用宿主待机暂停和画面就绪后继续的规则。断电不保证正在录制的临时文件可恢复。

## 构建与安装

按[开发手册](../DEVELOPMENT_zn.md)配置工具链后执行：

```powershell
./tools/elf_app/build_app.ps1 -Compiler $compiler -AppDirectory ./tools/elf_app/recorder
```

输出 `recorder.elf`，手动安装时使用交付文件名 `录音机.elf`，与匹配的 `manifest.json` 放在 `metalio/e-ink/apps/录音机/`。重建后同步清单大小与 SHA256，或通过应用商店发布。先升级支持 `XZ_APP_CAP_RECORDING` 与 WAV 播放的固件；旧固件能力不足时入口返回 `XZ_APP_ERR_UNSUPPORTED`。

宿主参数、返回值及生命周期约定见[录音开发说明](../DEVELOPMENT_zn.md#录音xz_app_cap_recording)。
