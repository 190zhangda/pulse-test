# Pulse 自定义补丁 v1.0.60-c1（2026-11-16）

基于官方 Pulse 1.0.60 源码的自定义修改集合。

## 补丁内容

1. **快捷键**
   - 单栏 = `Alt+1`，左右分屏 = `Alt+2`（四预设 Alt+1..4，菜单提示同步）
   - 常用标签切换 = `Alt+Shift+1..9`
   - 打开/关闭设置 = `Ctrl+,`
   - 切换主题 = `Alt+/`
2. **标题栏**：删除窗口管理按钮（右键标题栏空白仍可打开窗口管理菜单）。
3. **设置瘦身**
   - 外观页：只保留主题 + 语言（其余卡片全部删除）
   - 语言默认跟随系统（保持官方默认，无需改动）
   - 「文件操作」页删除，其行合并进「文件列表」页
   - 其余页（文件列表/标签页与窗口/启动与集成/搜索与索引/快速预览/扩展商店/AI 助手/右键菜单/快捷键/重复文件）保留
4. **快速预览 › 悬停延迟**：三选一改为连续滑条（0–5000 ms，每 500 ms 一个刻度，右缘实时数值，拖动即生效）。
5. **暂存区（托盘）**：从首页侧栏完全移除（含设置页相关行）。
6. **修复 pptx/ppt 等幻灯片文档快速预览**：Office 预览宿主启动失败时回退到内置草图渲染，不再显示「预览宿主不可用」。

## 目录

- `files\`           全部修改过的源文件（按仓库相对路径）
- `apply_patch.ps1`  应用补丁（自动备份原文件到 `backup\<时间戳>\`）
- `restore_patch.ps1` 还原最近一次（或指定时间戳）备份
- `build_all.ps1`    应用后一键构建 + 打包便携版

## 使用方法（官方更新后）

```powershell
# 1. 解压官方新版源码到 D:\GitHub\pulse（或你的源码根目录）
# 2. 应用补丁（自动备份被覆盖的官方文件）
powershell -ExecutionPolicy Bypass -File dist\pulse-custom-patch\apply_patch.ps1 -PulseRoot D:\GitHub\pulse
# 3. 构建 + 打包
powershell -ExecutionPolicy Bypass -File dist\pulse-custom-patch\build_all.ps1
```

### 冲突处理

补丁以「整文件覆盖」方式应用。官方更新若大改了补丁涉及的文件（本补丁 33 个文件，见 `files\` 目录清单），覆盖后编译失败的可能性集中在：

- `src\ui\ui_settings_core.cpp` / `settings_layout_sections.h` / `ui_settings_view.cpp`（设置页结构）
- `src\app\app_input.cpp` / `app_runtime.cpp`（输入分发/BuildVm）
- `src\app\keymap.cpp/.h` / `explorer_shortcuts.h`（快捷键表）

若编译报错，用 `restore_patch.ps1` 还原后逐文件对比官方新版与 `files\` 里的补丁版手工合并（补丁内改动都以 `// 悬停延迟`、`// Staging tray removed by customization`、`customization:` 等注释标记，便于检索）。

### 还原

```powershell
powershell -ExecutionPolicy Bypass -File dist\pulse-custom-patch\restore_patch.ps1 -PulseRoot D:\GitHub\pulse
```

## 备注

- `selftest_1b2.cpp` / `settings_flow_test.cpp` / `settings_interaction_test.cpp` / `app_controllers_test.cpp` / `preview_test_main.cpp` 是测试文件，官方更新时可直接用补丁版覆盖（保证测试与新行为一致）。
- `dist\perf-report-1.0.60.html`（性能报告）与补丁无关，未包含。
