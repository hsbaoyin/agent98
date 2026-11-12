# Agent98

Windows 9x（优先 Windows 98 SE）上的原生 AI 聊天客户端。程序直接通过
BearSSL 连接现代 HTTPS LLM API，不依赖局域网中转机或第二台电脑。

- C / Win32，OpenWatcom 2.0 构建
- BearSSL 0.6（TLS 1.2，内置 Mozilla 根证书）
- cJSON
- 命令行 `a98.exe` 与原生 GUI `agent98.exe` 共用同一份 `agent98.ini`
- OpenAI 兼容的 Chat Completions 接口，支持流式与非流式
- 本地工具：读文件、写文件、列目录、搜索文件、执行命令（需确认）
- 自动向模型注入运行环境（Windows 版本、workspace、COMMAND.COM 约束）

## 发布文件

`dist/` 里是已经编译好的 Windows 9x 版本：

| 文件 | 说明 |
|---|---|
| `dist/agent98.exe` | 图形界面版（推荐） |
| `dist/a98.exe` | 命令行版，与 GUI 共用 `agent98.ini` |
| `dist/tlsprobe.exe` | TLS 连通性探测 |
| `dist/sample.ini` | 配置示例 |
| `dist/README.TXT` | 安装与使用说明（GBK 编码，记事本可直接打开） |

把 `dist/` 里的文件复制到硬盘目录（例如 `C:\AGENT98`）再运行。不要直接在
光盘或只读目录里运行：程序需要在 exe 同目录创建 `agent98.ini`、
`agent98.log` 和 workspace。

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
dist/           已编译好的发布文件
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

### Win9x 链接选项

`tools/gen_ow_build.ps1` 生成的所有 wlink 响应文件都带有：

```
option offset=0x400000
option norelocs
option version=4.0
option stack=262144
```

固定基址、去掉重定位表、镜像版本 4.0，Win9x 的 PE 加载器才愿意加载；
256 KB 栈给 BearSSL 客户端上下文留足空间。改动这些选项后需要重新运行
`tools\gen_ow_build.ps1`，再 `build_ow.cmd clean` 全量重建。

### 启动日志

`agent98.exe` 和 `a98.exe` 启动后的第一件事就是在 exe 同目录打开
`agent98.log`，记录版本和 exe 目录，然后才读取 INI。排查启动崩溃时先看
这个文件：如果它完全没有生成，说明程序还没进入 `main` / `WinMain`。

## 配置

首次运行会在 exe 同目录生成 `agent98.ini`：

```ini
[api]
base=https://api.openai.com/v1
key=sk-...
model=gpt-4o-mini
system_prompt=You are Agent98...
extra_headers=X-Title: Agent98
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

### GUI 设置

`Tools -> Settings...` 可以直接填写所有 API 参数：

- **Provider** 预设：OpenAI、DeepSeek、Moonshot/Kimi、SiliconFlow、
  Ollama（本地）、Custom。选择预设会填入对应的 API 地址和默认模型。
- **API base / API key / Model**：任意 OpenAI 兼容服务；Ollama 的 key
  可以留空。
- **System prompt**：多行系统提示词。
- **Temperature / Max tokens / Max steps**。
- **Extra headers**：每行一个 `Name: value`，也可以在同一行用 `|`
  分隔，用于 OpenRouter 等需要额外头的服务。
- **Test connection**：向 `{api_base}/models` 发一个 GET 请求，显示
  HTTP 状态。401/403 表示密钥问题，404 表示地址问题，5xx 表示服务端
  可达但出错。
- **Workspace / TLS / shell / stream** 开关。

所有字段保存到 `agent98.ini`。

### 运行时环境注入

每次请求都会在 system 消息最前面自动加入一段探测结果，再把用户填写的
system prompt 作为 `User instructions` 追加在后面：

- 通过 `GetVersionExA` 识别的系统版本，例如
  `Windows 98 Second Edition 4.10.2222 A`
- 当前 workspace 和进程工作目录
- 核心约束：`run_command` 使用 16 位 `COMMAND.COM`，不是 `cmd.exe`
  或 PowerShell；不要使用 `2>&1`、`&&`、`||`、stderr 管道、`curl`、
  `tasklist`，优先使用内置文件工具和 `ver`、`mem`、`dir`、`set`、`type`

这样模型不会在 Win9x 上建议无法执行的 PowerShell 命令。该段内容不需要
配置，也不能通过 INI 关闭。

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

### 设置对话框冒烟测试

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests\settings_smoke.ps1
```

测试会在 8788 端口启动一个临时 mock 服务器，验证服务商预设、API
base/model 填写、`Test connection` 的 HTTP 200 结果，以及 system
prompt、temperature、extra headers 是否写入 INI。测试结束后会恢复
原来的 `agent98.ini`。

> 注意：`build/testrun/agent98.exe` 是手工复制的测试副本，重新构建后
> 需要把 `build/ow/agent98.exe` 复制过去再跑 GUI 测试。

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
