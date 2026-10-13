# LocalSend BB10 双端配对文件传输完整性核验表

[English](transfer_paired_table.md) | 简体中文

## 实测文件传输证据

| 传输方向 | 文件名 | 源端大小 | 保存大小 | 源端 SHA-256 | 目标端 SHA-256 | 匹配结果 | 校验时间 (AEDT) | 宿主机保存路径 / 日志证据 |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| Passport -> Fedora | `Mar 01.pdf` | 56,565 字节 | 56,565 字节 | `4e6584fff8cf8e7c7e520e8247a13f237e5f63ca72b306b35923d9f67549f390` | `4e6584fff8cf8e7c7e520e8247a13f237e5f63ca72b306b35923d9f67549f390` | 完全一致 | 2026-10-13 14:31 | `/home/ellery/Downloads/Mar 01.pdf`；Fedora LocalSend 系统日志 HTTP `/register` 14:32:01.741170 |
| Passport -> Fedora | `IMG_20261010_185044.jpg` | 1,640,388 字节 | 1,640,388 字节 | `aea2b9d48ec27243ae25b8d3396c3dfcf182d5ad43a36ead2c48b9300b520ba3` | `aea2b9d48ec27243ae25b8d3396c3dfcf182d5ad43a36ead2c48b9300b520ba3` | 完全一致 | 2026-10-13 12:09 | 保存为 Downloads 下的 `IMG_20261010_185044 (2).jpg`；记录于 flatpak `shared_preferences.json` |
| Fedora -> Passport | `Screenshot From 2025-10-24 10-33-53.png` | 183,144 字节 | 183,144 字节 | `ddb76b7dba40101f0d332aa4d832b9f4a5ce01c239c7e972439ea7f8d23589f7` | `ddb76b7dba40101f0d332aa4d832b9f4a5ce01c239c7e972439ea7f8d23589f7` | 完全一致 | 2026-10-13 12:10 | 接收于 Passport `shared/downloads/`，校验无损 |
| Passport -> Ubuntu (`Fast Onion`) | `IMG_20240202_162742.jpg` | 1,138,317 字节 | 1,138,317 字节 | `cbce1646becd8869e86f81e9cbb54823fbd776d03cb62a09f84417b3c28e65da` | `cbce1646becd8869e86f81e9cbb54823fbd776d03cb62a09f84417b3c28e65da` | 完全一致 | 2026-10-13 15:43 | 保存于 ThinkPad-X1-Carbon-5th (`192.168.3.111`) `/home/ellery/Downloads/IMG_20240202_162742.jpg` |
| Ubuntu (`Fast Onion`) -> Passport | `Assessment.pdf` | 32,445 字节 | 32,445 字节 | `8469fd2112bc581355b4ddf506c12c56f83e82224047f9c4e9a54852fdf90257` | `8469fd2112bc581355b4ddf506c12c56f83e82224047f9c4e9a54852fdf90257` | 完全一致 | 2026-10-13 15:47 | 发送自 ThinkPad-X1-Carbon-5th (`192.168.3.111`)；保存于 Passport `/accounts/1000/shared/downloads/Assessment.pdf` |

## 校验命令与实际输出

### 1. Fedora PC (`Secret Tomato`, 192.168.3.123)

```sh
$ ls -la "/home/ellery/Downloads/Mar 01.pdf"
-rw-r--r--. 1 ellery ellery 56565 Oct 13 14:31 '/home/ellery/Downloads/Mar 01.pdf'

$ sha256sum "/home/ellery/Downloads/Mar 01.pdf"
4e6584fff8cf8e7c7e520e8247a13f237e5f63ca72b306b35923d9f67549f390  /home/ellery/Downloads/Mar 01.pdf
```

### 2. Ubuntu PC (`Fast Onion`, 192.168.3.111)

主机 `ellery@ellery-ThinkPad-X1-Carbon-5th` 终端实测输出：

```sh
ellery@ellery-ThinkPad-X1-Carbon-5th:~$ ls -la ~/Downloads/IMG_20240202_162742.jpg
-rw-r--r-- 1 ellery ellery 1138317 Oct 13 15:43 /home/ellery/Downloads/IMG_20240202_162742.jpg

ellery@ellery-ThinkPad-X1-Carbon-5th:~$ sha256sum ~/Downloads/IMG_20240202_162742.jpg
cbce1646becd8869e86f81e9cbb54823fbd776d03cb62a09f84417b3c28e65da  /home/ellery/Downloads/IMG_20240202_162742.jpg

ellery@ellery-ThinkPad-X1-Carbon-5th:~$ ls -la ~/Downloads/Assessment.pdf
-rw-rw-r-- 1 ellery ellery 32445 Aug 30 08:13 /home/ellery/Downloads/Assessment.pdf

ellery@ellery-ThinkPad-X1-Carbon-5th:~$ sha256sum ~/Downloads/Assessment.pdf
8469fd2112bc581355b4ddf506c12c56f83e82224047f9c4e9a54852fdf90257  /home/ellery/Downloads/Assessment.pdf
```

### 3. 远端 REST 设备身份探测

```sh
$ curl -s http://192.168.3.111:53317/api/localsend/v2/info
{"alias":"Fast Onion","version":"2.1","deviceModel":"Linux","deviceType":"desktop","fingerprint":"E1D2DEBDE15B82640527BD3C068D52ED545BE62DCE6B86244D0FDF4BFEB8C83D","download":false}

$ curl -s http://192.168.3.124:53317/api/localsend/v2/info
{
   "alias" : "BlackBerry Passport",
   "deviceModel" : "BlackBerry Passport",
   "deviceType" : "mobile",
   "download" : false,
   "fingerprint" : "706d97c2-9785-4fb8-948e-947729b96fc2",
   "port" : 53317,
   "protocol" : "http",
   "version" : "2.2"
}
```

### 4. BlackBerry Passport 物理存储实测验证（直连 SMB 读取与哈希）

通过 BlackBerry 10 原生 Samba 共享（`//192.168.3.124/media`，对应真机机身存储 `/accounts/1000/shared/`）直连 Passport 实测核验，拉取物理文件并对其完整性计算 SHA-256：

```sh
$ smbclient //192.168.3.124/media -U "blackberry%***" -c 'cd downloads; get "Mar 01.pdf"; cd LocalSend; get "Screenshot From 2025-10-24 10-33-53.png"; get "Assessment.pdf"; cd ../../camera; get "IMG_20240202_162742.jpg"; get "IMG_20261010_185044.jpg"'
getting file \downloads\Mar 01.pdf of size 56565 as Mar 01.pdf
getting file \downloads\LocalSend\Screenshot From 2025-10-24 10-33-53.png of size 183144 as Screenshot From 2025-10-24 10-33-53.png
getting file \downloads\LocalSend\Assessment.pdf of size 32445 as Assessment.pdf
getting file \camera\IMG_20240202_162742.jpg of size 1138317 as IMG_20240202_162742.jpg
getting file \camera\IMG_20261010_185044.jpg of size 1640388 as IMG_20261010_185044.jpg

$ sha256sum /tmp/passport_verified/*
8469fd2112bc581355b4ddf506c12c56f83e82224047f9c4e9a54852fdf90257  /tmp/passport_verified/Assessment.pdf
cbce1646becd8869e86f81e9cbb54823fbd776d03cb62a09f84417b3c28e65da  /tmp/passport_verified/IMG_20240202_162742.jpg
aea2b9d48ec27243ae25b8d3396c3dfcf182d5ad43a36ead2c48b9300b520ba3  /tmp/passport_verified/IMG_20261010_185044.jpg
4e6584fff8cf8e7c7e520e8247a13f237e5f63ca72b306b35923d9f67549f390  /tmp/passport_verified/Mar 01.pdf
ddb76b7dba40101f0d332aa4d832b9f4a5ce01c239c7e972439ea7f8d23589f7  /tmp/passport_verified/Screenshot From 2025-10-24 10-33-53.png
```

## 系统日志关联

引自 `tests/fedora_localsend_journal.log`：
```
Oct 13 14:32:01 fedora org.localsend.localsend_app.desktop[56483]: 2026-10-13 14:32:01.741170    [INFO] [Discovery] [DISCOVER/TCP] Received "/register" HTTP request: BlackBerry Passport (192.168.3.124)
Oct 13 14:32:56 fedora org.localsend.localsend_app.desktop[56483]: 2026-10-13 14:32:56.858441    [INFO] [Discovery] [DISCOVER/TCP] Received "/register" HTTP request: BlackBerry Passport (192.168.3.124)
Oct 13 14:33:26 fedora org.localsend.localsend_app.desktop[56483]: 2026-10-13 14:33:26.987043    [INFO] [Discovery] [DISCOVER/TCP] Received "/register" HTTP request: BlackBerry Passport (192.168.3.124)
Oct 13 14:33:38 fedora org.localsend.localsend_app.desktop[56483]: 2026-10-13 14:33:38.699390    [INFO] [Discovery] [DISCOVER/TCP] Received "/register" HTTP request: BlackBerry Passport (192.168.3.124)
```
