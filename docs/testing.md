# 测试与复现

## 自动化测试

```sh
git submodule update --init --recursive
cmake --preset debug -DCMAKE_PREFIX_PATH=/你的/Qt/6.12.0/对应平台目录
cmake --build --preset debug
ctest --preset debug
```

测试目标为 `tst_simulator`，链接与桌面应用相同的 `fplc_core`，使用 QtTest 与 QtNetwork。网络测试临时申请本机高端口，不需要监听 102，不依赖真实 PLC 或 Python。

主要覆盖：

- 工程 JSON 保存/加载往返，非法 JSON、错误版本和错误字段类型；加载失败保留目标模型，保存失败保留旧文件。
- 地址越界、位号非法、类型宽度不匹配、数值溢出、位级重叠、无效规则及跟随循环。
- 真正的 Snap7 客户端通过 TCP 读取初始 REAL/INT，写入大端数值并检查服务器模型。
- S7 单位写命令不覆盖邻位，命令、跟随反馈和客户端内存一致。
- manual 数据不被 tick 重置；界面使用的 `setValue()` 与网络可读内存相同。
- 正弦、斜坡、翻转与依赖跟随的确定性时点；停止状态不推进规则。
- 未注册 DB 和越界读取报错后，正常 DB 继续可读。
- 服务停止/重启保值，运行中拒绝替换工程，停止后应用新工程才重置初值。
- 连续 20 轮客户端连接、读写和断开，其中每四轮在客户端仍连接时停止并重启服务。

客户端与服务端均为 Snap7，这些测试能证明本项目的集成行为，但不是独立协议实现间的兼容性证据。

macOS 另有独立进程目标 `snap7_sigpipe_probe`：恢复 SIGPIPE 的默认处理，将真正的 loopback TCP accepted socket 交给 Snap7，检查 `SO_NOSIGPIPE`，在本地写端关闭后调用发送。预期返回 EPIPE 错误且进程正常退出。独立进程避免通过忽略 SIGPIPE 掩盖缺陷，也不让信号终止整个 QtTest 进程。

GUI 目标 `tst_ui` 检查初始不监听、按钮控制真实 S7 连接、表格编辑只改变目标当前值、工程存取及失败保留状态；CTest 配置使用 offscreen 平台。这不代替原生桌面的视觉检查。

## 手工联调

1. 启动 `FluentPLCSimulator`，打开 `examples/hmi-demo.plcsim.json`，启动服务。
2. 使用独立实现的 S7 客户端连接 `127.0.0.1:1102`、rack 0、slot 1。可选用纯 Python 的 `python-snap7` 3.x，固定版本后按其官方 API 操作。
3. 连续读取 `DB1.DBD4 / REAL`，检查数值在 20–80 之间变化。
4. 写入 `DB1.DBW12 / INT = 1500`，检查桌面当前值与下一次读取均为 1500，持续采集不会恢复为 1200。
5. 对 `DB1.DBX0.1` 发真正的位写，检查 `DB1.DBX0.0` 与 `Q0.0` 在规则 tick 后跟随；检查其他无关位保持原值。不能用“先读整字节再改写整字节”的客户端辅助方法代替这一项。
6. 停止并重新启动，检查手动值保留；停止后重新加载工程，检查设定值恢复为初值。
7. 请求未注册 DB，确认错误；随后重新读取有效 DB，确认连接仍可工作。

需要检查多客户端时，可让 HMI 与另一独立客户端同时采集，交替写入不同的命名变量，并检查各自读回。不要把这项建议当作已完成的多客户端压力测试。

## 当前验证记录

2026-11-15，本机 macOS arm64、Qt 6.12.0、Apple Clang 17、Debug 构建实际通过：

```text
ctest --test-dir build/debug --output-on-failure --no-tests=error
simulator       Passed  4.56 s
ui              Passed  1.96 s
native_sigpipe  Passed  0.32 s
100% tests passed, 0 tests failed out of 3
```

`SimulatorTest` 报告 39 项通过，`UiTest` 报告 6 项通过（各自包含 init/cleanup）；EPIPE 探针输出 `result=-1, socketError=32`，正常退出。网络测试在允许普通 loopback 监听的环境中执行；受限沙箱禁止 bind 时的失败不计为功能验证。详细本地日志位于 `build/debug/Testing/Temporary/LastTest.log`。

随后更新 GUI 测试 fixture 和资源嵌入并重新编译，只重跑受影响的 UI 测试：

```text
ctest --test-dir build/debug -R '^ui$' --output-on-failure
1/1 Test #2: ui  Passed  2.48 s
100% tests passed, 0 tests failed out of 1
```

本次 `UiTest` 仍为 6 项通过，没有缺失图标资源告警；未重复运行已通过且未变化的 core/native 测试。`LastTest.log` 始终记录最近一次 CTest 调用，所以此时内容为 UI 子集。

示例文件已由同一构建的 `FluentPLCServer --write-demo` 生成，`--project examples/hmi-demo.plcsim.json --check` 验证通过；地址和初值与默认桌面示例相同。

独立协议实现交叉验证也已完成：纯 Python `python-snap7==3.5.0` 客户端真实连接本机命令行服务，观察 REAL 温度采样从 50.0 变化到约 53.610，INT 写入 −234 并读回一致；BOOL 真正位写及 follow 更新使 DB1 字节 0 从 `0x83` 变成 `0x80`，同时保留无关的 bit 7。该验证未使用本项目的 Snap7 客户端封装，因此补充了跨实现通信证据。

macOS arm64 的 **Debug 便携 `.app` 包**已生成于 `dist/FluentPLC Simulator.app`，`codesign` 严格校验通过；应用入口、随包 CLI 和 Snap7 动态库的依赖检查仅出现随包相对路径或系统路径。该产物不是 Release 构建。原生桌面启动截图仍在验收中。

Release 构建及 Windows/Linux 运行状态仍待验证。CI 配置不等同于已经在远端跑过并通过。

### 打包应用原生启动

通过 macOS Launch Services 启动便携 `.app` 并生成实际窗口截图，应用正常启动和退出。已检查原生窗口的文字、布局、服务配置及变量表；截图见 `docs/images/main-window.png`。生产包使用 Cocoa 平台插件，测试构建使用 offscreen 插件。

### macOS Release 构建

在本机 Apple Silicon 上完成 Release 全量构建。优化构建的 CTest 三组全部通过（simulator / ui / native_sigpipe，7.79 秒），最终窗口标题调整后只重跑受影响的 UI 组并通过。已检查 Release 原生窗口截图；便携应用的生成命令为 `python3 scripts/package_macos.py --build-dir build/release --output "dist/FluentPLC-Simulator-0.1.0-macOS-arm64/FluentPLC Simulator.app"`。

### 语言、主题与仓库信息

Release 构建中的 UI 与 i18n 两组测试已通过，覆盖默认 Auto、系统语言解析、手动切换与保存、运行中保持 S7 连接和值、编辑输入保持、仓库卡翻译和最小窗口滚动。源文使用英文；中文词典逐项检查覆盖率和占位符，允许英文标点及单位符号。已生成并检查中英、明暗监视页与设置页截图。

### 0.1.1 应用更新与弹窗按钮

macOS arm64 Release 的五组 CTest（simulator / ui / i18n / update / native_sigpipe）通过。OTA 回归复用 HMI 更新器的发布解析、版本比较与下载校验测试，补充取消下载和不完整下载检查；UI 回归覆盖更新检查偏好的保存和翻译；新增、编辑弹窗的取消按钮样式通过明暗主题截图核对。

使用临时目录中的真实打包应用和本机 Release API，验证 SHA-256 不匹配时保留旧应用，以及下载、校验、解压、退出后实际替换应用并保留外部工程文件。该验证也修正了更新助手继承命令行输出管道的问题。修正后仅重跑受影响的 update / i18n 两组并通过。
