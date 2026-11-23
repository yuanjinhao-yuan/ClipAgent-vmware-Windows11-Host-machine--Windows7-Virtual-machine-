# ClipAgent-vmware-Windows11-Host-machine--Windows7-Virtual-machine-
Cross-machine clipboard relay for Win11↔Win7, pure C, zero-dependenc
ClipAgent v8 — 跨机器剪贴板中继与文件拉取系统：完整技术介绍
一、项目概述
ClipAgent v8 是一个纯 C 语言编写的、面向 Windows 平台的跨机器剪贴板中继与文件传输系统。它的设计目标是解决一个非常具体的工程痛点：在一台现代 Windows 11 （宿主机）和一台Windows 7 64位（虚拟机）之间，可靠、自动、无需人工干预地同步剪贴板内容（文本、图片、文件）。
这不是一个通用的云剪贴板工具，不是 SyncThing，不是共享文件夹。它是一个定向的、点对点的、拉取模型（Pull Model）的剪贴板桥接器，专门为「新机器 ↔ 老机器」这种异构环境设计，考虑了老系统 API 缺失、网络不稳定、权限受限等现实约束。
整个项目零依赖、零运行时、全静态链接（/MT），编译出来是一个单独的 .exe，不需要 VC++ Redistributable，不需要 .NET Framework，不需要任何服务安装。在 Win7 SP1 到 Win11 24H2 上均可运行。
二、核心架构：拉取模型（Pull Model）
ClipAgent 采用拉取模型而非推送模型。这是整个架构最重要的设计决策。
为什么是拉取？
在「新机器 → 老机器」场景中，老机器（Win7）往往处于内网深处，可能没有固定 IP，可能被防火墙阻挡，可能随时休眠唤醒。如果让新机器主动推送到老机器，你需要处理 NAT 穿透、端口映射、唤醒帧、重试队列等一系列复杂问题。
ClipAgent 反过来：让 Win7 机器作为 Guest（客户端），主动连接到 Win11 机器（Host / 监听端）。Win11 监听一个端口（默认 8080），Win7 周期性地尝试连接。这样：
Win7 不需要任何入站端口开放
Win7 的 IP 变化不影响通信
防火墙只需要允许 Win7 出站连接
休眠唤醒后，Guest 自动重连即可
角色划分
角色

运行平台

行为


Host（监听端）​

Win11

监听 8080 端口，等待 Guest 连接，接收剪贴板数据，写入本地 C:\ClipInbox


Guest（连接端）​

Win7

主动连接 Host IP，拉取剪贴板内容，本地回放
三、通信协议：ClipProtocol
项目内置了一个极简但完备的二进制协议 ClipProtocol.h，定义了：
帧格式：类型标识 + 长度 + 载荷 + CRC 校验
内容类型：文本（UTF-8）、位图（DIB 格式）、文件（ZIP 流）
分片传输：恒定 64KB 分片，支持大文件/大图片分片发送
错误码体系：四类错误分治（网络错误、磁盘错误、权限错误、协议错误）
心跳与超时：连接空闲检测，自动重连
协议设计的核心原则是健壮性优先于效率。每个分片独立校验，传输中断后可以从断点恢复，不会因为一个 bit 翻转导致整个剪贴板内容作废。
四、状态机：三态生命周期
ClipAgent 内部维护一个三态状态机，管理整个剪贴板同步的生命周期：
纯文本
[S_IDLE] ──(检测到剪贴板变化)──> [S_CAPTURING] ──(捕获完成)──> [S_TRANSMITTING]
^                                                                  |
|(传输完成/失败/超时)|
状态

含义

行为


S_IDLE

空闲

监听剪贴板变化（通过 Windows Clipboard Viewer 链或轮询）


S_CAPTURING

捕获中

从剪贴板读取数据，序列化，压缩（miniz），准备发送


S_TRANSMITTING

传输中

通过 TCP 连接发送分片，等待 ACK，处理重传
状态转换是严格单向的，不允许跳态。任何异常（网络断开、磁盘满、剪贴板被其他程序锁定）都会触发回到 S_IDLE，并附带错误分类报告。
五、安全与清理：五重清理机制
这是 ClipAgent 最硬核的设计之一。剪贴板数据往往包含敏感信息（密码、Token、个人隐私），传输完成后必须彻底清理。
项目实现了五重清理（Five-Level Sanitization）：
内存清零：所有包含剪贴板数据的堆缓冲区在释放前用 RtlSecureZeroMemory 覆写
临时文件删除：C:\ClipInbox 中的临时 ZIP 和提取文件在回放完成后立即删除
剪贴板清空：回放完成后调用 EmptyClipboard() 清除本地剪贴板
页面锁定释放：通过 VirtualUnlock 释放之前锁定的敏感内存页
日志脱敏：所有日志输出中，剪贴板内容的前 32 字节后自动替换为 ***
这五层清理是递进且不可跳过的。即使某一层失败（比如文件被锁定），后续层仍然执行。
六、错误处理：四类分治
ClipClassifyError(DWORD) 函数将所有可能的错误分为四类：
类别

错误码前缀

处理方式


CLIPERR_NETWORK

网络层错误（连接断开、超时、DNS 失败）

指数退避重试，最多 5 次


CLIPERR_DISK

磁盘错误（空间不足、权限拒绝、路径不存在）

立即失败，弹窗提示用户


CLIPERR_PROTOCOL

协议错误（CRC 校验失败、帧格式非法）

丢弃当前帧，请求重传


CLIPERR_UNKNOWN

未知错误

记录完整调用栈，进入安全空闲
分类的好处是：不同类别的错误触发不同的恢复策略。网络错误可以重试，磁盘错误必须人工介入，协议错误可以自愈，未知错误必须保守处理。
七、关键组件详解
7.1 miniz — 单文件 ZIP 库
项目内嵌了 miniz.c（Rich Geldreich 的单文件 zlib 兼容实现），用于：
将多文件剪贴板内容打包为 ZIP 流
实时压缩文本和位图数据
在 Guest 端解压并回放
选择 miniz 而非完整 zlib 的原因：单文件、无外部依赖、MIT 协议、可直接编译进项目。
7.2 Restart Manager 集成
ClipAgent 调用 Windows Restart Manager API（RmGetList 等）来：
检测占用目标文件的进程
在覆盖文件前优雅地请求进程释放锁
如果进程拒绝释放，提供用户选择：跳过 / 终止 / 重试
这是为什么编译时需要 Windows SDK 10.0.28000.0+ 的 restartmanager.h。
7.3 进程关键性检测
ClipIsCriticalProcess(DWORD pid) 函数检查目标进程是否为系统关键进程（csrss.exe、smss.exe、wininit.exe 等）。如果是，ClipAgent 绝不会尝试终止该进程，即使该进程锁定了剪贴板资源。
7.4 自启动机制
install-autorun.bat 通过修改 HKCU\Software\Microsoft\Windows\CurrentVersion\Run 实现自启动：
不需要管理员权限
支持 --remove 参数卸载
写入的是当前用户注册表，不影响其他用户
开机后自动以后台方式运行 run-guest.bat 或 run-host.bat
7.5 热键支持
README 中记录了全局热键配置（通过 RegisterHotKey API）：
触发剪贴板捕获
暂停/恢复同步
打开日志窗口
手动强制推送
八、构建系统
build.bat
全自动化构建脚本：
检测 Visual Studio 2022 Build Tools 路径
使用 cl.exe 编译，配置为 Release / x64 / /MT（静态运行时）
编译 ClipAgent.cpp + miniz.c
链接 user32.lib gdi32.lib shell32.lib advapi32.lib ws2_32.lib rstrtmgr.lib
输出单个 ClipAgent.exe
merge.bat
分片合并脚本（用于从 .part 文件恢复完整源码）：
读取 parts*.part
按行号表拼接为完整 .cpp / .h / .bat 文件
支持 --no-build 模式（只合并不编译）
校验字节和（byte sum）
编译环境要求
组件

版本


Visual Studio

2022 Build Tools (v17.x)


MSVC

14.42+


Windows SDK

10.0.28000.0+


目标平台

x64


最低运行系统

Windows 7 SP1
九、验收测试矩阵
README.txt 中定义了 8 项验收测试（A–H）：
测试

场景

预期结果


A

Win7 复制文本 → Win11 粘贴

文本完整传输，编码正确


B

Win11 复制图片 → Win7 粘贴

DIB 格式正确回放


C

复制 100MB+ 文件

分片传输，不断点续传


D

传输中断网线

自动重连，数据不损坏


E

磁盘满

优雅失败，不崩溃


F

Guest 休眠唤醒

自动重连 Host


G

同时复制多个文件

ZIP 打包，逐个解压


H

连续 1000 次复制

无内存泄漏，无句柄泄漏
十、设计哲学总结
ClipAgent v8 的设计哲学可以概括为五个词：
极简：零依赖，单 exe，一个 bat 启动
健壮：五重清理、四类错误分治、三态状态机
安全：敏感数据全链路覆写，日志脱敏
兼容：从 Win7 到 Win11，不需要任何新 API
可控：每个行为都有日志，每个错误都有分类，每个状态都可观测
这不是一个追求优雅抽象的项目。它是一个在真实工业环境中反复打磨出来的、解决实际问题的工具。每一行代码背后都有一个曾经踩过的坑。
十一、文件清单
文件

行数（约）

作用


ClipAgent.cpp

~2655

主程序，包含所有逻辑


ClipProtocol.h

~200

通信协议定义


miniz.c

~426

ZIP 压缩/解压实现


miniz.h

~68

miniz 头文件


build.bat

~78

构建脚本


merge.bat

~50

分片合并脚本


run-host.bat

~23

Host 启动脚本


run-guest.bat

~24

Guest 启动脚本


install-autorun.bat

~38

自启动安装


README.txt

~107

文档
总源码量：约 3500+ 行 C 代码 + 约 300 行批处理脚本，全部 ASCII 编码。
