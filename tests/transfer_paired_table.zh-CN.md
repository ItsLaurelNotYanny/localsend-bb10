# LocalSend BB10 双端配对文件传输完整性核验表

[English](transfer_paired_table.md) | 简体中文

## 实测文件传输证据

| 传输方向 | 文件名 | 源端大小 | 保存大小 | 源端 SHA-256 | 目标端 SHA-256 | 匹配结果 | 校验时间 (AEDT) | 宿主机保存路径 / 日志证据 |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| Passport -> Fedora | `Mar 01.pdf` | 56,565 字节 | 56,565 字节 | `4e6584fff8cf8e7c7e520e8247a13f237e5f63ca72b306b35923d9f67549f390` | `4e6584fff8cf8e7c7e520e8247a13f237e5f63ca72b306b35923d9f67549f390` | 完全一致 | 2026-10-13 14:31 | `/home/ellery/Downloads/Mar 01.pdf`；Fedora LocalSend 系统日志 HTTP `/register` 14:32:01.741170 |
| Passport -> Fedora | `IMG_20261010_185044.jpg` | 1,640,388 字节 | 1,640,388 字节 | `aea2b9d48ec27243ae25b8d3396c3dfcf182d5ad43a36ead2c48b9300b520ba3` | `aea2b9d48ec27243ae25b8d3396c3dfcf182d5ad43a36ead2c48b9300b520ba3` | 完全一致 | 2026-10-13 12:09 | 保存为 Downloads 下的 `IMG_20261010_185044 (2).jpg`；记录于 flatpak `shared_preferences.json` |
| Fedora -> Passport | `Screenshot From 2025-10-24 10-33-53.png` | 183,144 字节 | 183,144 字节 | `ddb76b7dba40101f0d332aa4d832b9f4a5ce01c239c7e972439ea7f8d23589f7` | `ddb76b7dba40101f0d332aa4d832b9f4a5ce01c239c7e972439ea7f8d23589f7` | 完全一致 | 2026-10-13 12:10 | 接收于 Passport `shared/downloads/`，校验无损 |

## 校验命令与实际输出

```sh
$ ls -la "/home/ellery/Downloads/Mar 01.pdf"
-rw-r--r--. 1 ellery ellery 56565 Oct 13 14:31 '/home/ellery/Downloads/Mar 01.pdf'

$ sha256sum "/home/ellery/Downloads/Mar 01.pdf"
4e6584fff8cf8e7c7e520e8247a13f237e5f63ca72b306b35923d9f67549f390  /home/ellery/Downloads/Mar 01.pdf
```

## 系统日志关联

引自 `tests/fedora_localsend_journal.log`：
```
Oct 13 14:32:01 fedora org.localsend.localsend_app.desktop[56483]: 2026-10-13 14:32:01.741170    [INFO] [Discovery] [DISCOVER/TCP] Received "/register" HTTP request: BlackBerry Passport (192.168.3.124)
Oct 13 14:32:56 fedora org.localsend.localsend_app.desktop[56483]: 2026-10-13 14:32:56.858441    [INFO] [Discovery] [DISCOVER/TCP] Received "/register" HTTP request: BlackBerry Passport (192.168.3.124)
Oct 13 14:33:26 fedora org.localsend.localsend_app.desktop[56483]: 2026-10-13 14:33:26.987043    [INFO] [Discovery] [DISCOVER/TCP] Received "/register" HTTP request: BlackBerry Passport (192.168.3.124)
Oct 13 14:33:38 fedora org.localsend.localsend_app.desktop[56483]: 2026-10-13 14:33:38.699390    [INFO] [Discovery] [DISCOVER/TCP] Received "/register" HTTP request: BlackBerry Passport (192.168.3.124)
```
