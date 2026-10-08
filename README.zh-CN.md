# LocalSend for BlackBerry 10

[English](README.md) | 简体中文

这是面向 BlackBerry 10 设备的非官方 LocalSend 原生 C++/Qt 客户端。它实现 [LocalSend 协议](https://github.com/localsend/protocol)，并参考了 [官方实现](https://github.com/localsend/localsend)。

当前处于规划阶段，尚未实现客户端，也未完成 BB10 编译或真机验证。

目标系统：BB10 10.3.x、Qt4.8，最终使用 Cascades 原生界面。MVP 分两步：先在 BB10 真机验证协议，再完善日常操作界面；完整 Cascades 页面可后置。范围为 IPv4 局域网、HTTP、前台运行、每次传输一个普通文件；接收多个候选时最多接受其中一个。暂不支持 HTTPS、目录、断点续传、Download API、后台服务和 PIN 输入。

HTTP-only 双向互通验收时，PC 官方 LocalSend 需要关闭加密。BB10 接收普通文件使用 Upload API，因此设备公告中的 `download` 为 `false`，这不表示不能接收文件。

双向收发是 MVP 的硬性要求：BB10→PC 与 PC→BB10 都必须成功传输 10 MiB 文件并核对 SHA-256。可以后置完整界面，不能后置发送或接收能力。

- [实现任务与交付约定](docs/mvp-plan.zh-CN.md)
- [协议基线与接口约束](docs/protocol-notes.zh-CN.md)
- [工具链、部署与验收记录](docs/build-deploy.zh-CN.md)
- [贡献者指引](AGENTS.zh-CN.md)

代码、标识符、注释、文档字符串及技术消息使用英文。每份文档维护英文和简体中文版本，修改时同步两份。

本项目名称与上述声明不表示获得 LocalSend 官方支持。许可证须在开始复制或引入外部代码前明确；引用协议与直接复制官方实现应分别处理。
