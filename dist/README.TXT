Agent98 0.1.0 - Windows 9x 原生 AI 客户端
==========================================

Agent98 是一个运行在 Windows 95/98/Me 上的原生 32 位程序，直接通过
BearSSL 连接现代的 HTTPS LLM 服务（OpenAI、DeepSeek、Moonshot/Kimi、
SiliconFlow、Ollama 等 OpenAI 兼容接口），不需要局域网中转机，也不需要
第二台电脑。


系统要求
--------
- Windows 95 OSR2 / 98 / 98SE / Me（推荐 Windows 98 SE）
- 486 以上 CPU，16 MB 内存以上
- 能访问外网的网络连接（Winsock 2 或 1.1）
- 系统日期和时间必须正确，否则 TLS 证书校验会失败


安装
----
1. 把整个文件夹复制到本地硬盘，例如 C:\AGENT98
   注意：不要直接在光盘或只读目录里运行，程序需要在 exe 同目录写
   配置、日志和 workspace。
2. 首次运行会自动生成 AGENT98.INI。
3. 用记事本打开 AGENT98.INI，填写 [api] 下的 key / base / model，
   或者运行 AGENT98.EXE，在 Tools -> Settings... 里填写。


文件说明
--------
AGENT98.EXE    图形界面版（推荐）
A98.EXE        命令行版，两个 exe 共用同一份 AGENT98.INI
TLSPROBE.EXE   TLS 连通性探测：TLSPROBE.EXE https://example.com/
SAMPLE.INI     配置示例，复制成 AGENT98.INI 使用
AGENT98.INI    首次运行后自动生成（含 API 密钥，不要外传）
AGENT98.LOG    运行日志，出问题时请看这个文件


快速开始
--------
1. 运行 AGENT98.EXE
2. Tools -> Settings...
3. Provider 选择服务商，填入 API key；本机 Ollama 选 Ollama 并留空 key
4. 点 Test connection，看到 HTTP 200 即表示地址和密钥可用
5. 回到主窗口，在下方输入框提问


命令行版
--------
  A98.EXE
输入问题后回车；/help 查看命令，Ctrl+C 取消当前请求。
也可以用管道批量输入，例如：
  TYPE QUESTIONS.TXT | A98.EXE


工具与安全
----------
模型可以调用本地工具：读文件、写文件、列目录、搜索文件、执行命令。
- 写文件和执行命令每次都会弹出确认对话框。
- workspace 目录之外默认不可访问，可在 INI 里打开 allow_outside。
- run_command 使用 16 位 COMMAND.COM，只支持 ver、dir、type、set、
  mem 等基本命令，不支持 cmd.exe / PowerShell 语法。


常见问题
--------
Q: 提示 "no entropy available" 或 TLS 握手失败？
A: 检查系统时间是否正确，以及能否用 TLSPROBE.EXE 连通目标站点。

Q: 界面是英文？
A: 0.1.0 只有英文界面，中文通过 RichEdit 正常显示。

Q: 窗口关闭很慢？
A: 关闭时会等待后台线程最多 3 秒。

Q: 程序崩溃，Dr. Watson 报 <unknown>？
A: 先确认程序是从硬盘运行的，然后把 AGENT98.LOG 的最后几十行发出来。


版本
----
0.1.0  首个发布版：CLI + GUI、BearSSL TLS 1.2、流式对话、本地工具、
       服务商预设与连接测试、运行时环境注入。