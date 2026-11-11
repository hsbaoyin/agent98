# Agent98

Windows 9x（优先 Windows 98 SE）上的原生 AI 聊天客户端。程序直接通过
BearSSL 连接现代 HTTPS LLM API，不依赖局域网中转机或第二台电脑。

- C / Win32，OpenWatcom 2.0 构建
- BearSSL 0.6（TLS 1.2，内置 Mozilla 根证书）
- cJSON
- 命令行 `a98.exe` 与原生 GUI `agent98.exe` 共用同一份 `agent98.ini`
- OpenAI 兼容的 Chat Completions 接口，支持流式与非流式
- 本地工具：读文件、写文件、列目录、搜索文件、执行命令（需确认）

## 目录结构

```
src/            程序源码
  agent/        agent 循环、OpenAI API、消息、工具
  core/         配置、日志、路径、字符串、UTF-8/ACP 转换
  gui/          Win32 GUI 与资源
  net/          URL、TCP、BearSSL、HTTP、SSE
third_party/    BearSSL、cJSON、Mozilla CA
tests/          mock 服务器与冒烟测试
tools/          构建辅助脚本
ow/             OpenWatcom 构建规则（由脚本生成）
```

## 构建

OpenWatcom 解压到 `tools/openwatcom/ow`，或设置 `WATCOM` 指向安装目录。

```bat
build_ow.cmd          :: CLI + GUI
build_ow.cmd gui      :: 仅 GUI
build_ow.cmd probe    :: TLS 探测工具
build_ow.cmd clean
```

产物：

- `build/ow/a98.exe`：命令行版
- `build/ow/agent98.exe`：GUI 版
- `build/ow/tlsprobe.exe`：TLS 连通性探测

修改 `src` 下的文件列表后，先运行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\gen_ow_build.ps1
```

### 多线程运行时（重要）

GUI 的 worker 线程会调用 C 运行时（malloc、stdio、日志等）。OpenWatcom
必须使用多线程运行时，并且新线程必须由 CRT 创建：

- 编译选项包含 `-bm`（见 `ow/rules.mk` 中的 `PROJECT_CFLAGS`）
- 使用 `_beginthreadex()`，不要直接使用 `CreateThread()`

否则 CRT 不会为新线程初始化 per-thread 数据，堆操作会随机崩溃
（`0xC0000005`，崩溃位置通常在 `__MemAllocator`）。

## 配置

首次运行会在 exe 同目录生成 `agent98.ini`：

```ini
[api]
base=https://api.openai.com/v1
key=sk-...
model=gpt-4o-mini
stream=1
max_tokens=1024
temperature=0.7
[agent]
workspace=C:\Agent98
allow_shell=0
max_steps=8
[tls]
verify=1
[log]
level=3
```

`workspace` 默认是 exe 同目录下的 `workspace`。日志写入 exe 同目录的
`agent98.log`。

## 测试

### TLS 探测

```bat
build\ow\tlsprobe.exe https://example.com/
```

应输出 TLS 版本、密码套件和 HTTP 状态。

### mock 服务器

```bat
node tests\mock_openai.js 8787
```

在 `build/testrun/agent98.ini` 中把 `api.base` 指向
`http://127.0.0.1:8787/v1`，`key` 可填 `test`。

### CLI

```powershell
"hello`nlist files`n" | .\build\testrun\a98.exe
```

### GUI 冒烟测试

准备 `build/testrun`（包含 `agent98.exe`、`agent98.ini` 和 workspace），
然后运行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests\gui_smoke.ps1
```

测试覆盖流式回复、工具调用与结果渲染、写文件确认的允许/拒绝。

### 工具单元冒烟测试

主机上可用 MinGW/GCC 构建：

```bat
gcc -std=c99 -O1 -w -Isrc -Isrc/core -Isrc/agent -Isrc/net ^
    -Ithird_party/bearssl/inc -Ithird_party/bearssl/src -Ithird_party/cjson ^
    tests\test_tools.c src\core\*.c src\agent\*.c ... -lws2_32 -o build\host\test_tools.exe
```

实际构建时使用 `build/host/sources.rsp` 中的源文件列表即可。

## 已知限制

- 目前仅实现 OpenAI 兼容接口；Anthropic / Gemini 尚未接入
- Win9x 上的 RichEdit 2.0A 不可用时自动回退到 RichEdit 1.0 / EDIT
- 尚未实现代理、自动重试、上下文裁剪
- 关闭窗口时最多等待 worker 3 秒；超时路径仍需加强
