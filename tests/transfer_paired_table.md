# LocalSend BB10 Paired File Transfer Integrity Table

English | [Simplified Chinese](transfer_paired_table.zh-CN.md)

## Verified Transfer Evidence

| Direction | Filename | Source Size | Saved Size | Source SHA-256 | Destination SHA-256 | Match | Timestamp (AEDT) | Host Destination Path / Log Evidence |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| Passport -> Fedora | `Mar 01.pdf` | 56,565 B | 56,565 B | `4e6584fff8cf8e7c7e520e8247a13f237e5f63ca72b306b35923d9f67549f390` | `4e6584fff8cf8e7c7e520e8247a13f237e5f63ca72b306b35923d9f67549f390` | EXACT | 2026-10-13 14:31 | `/home/ellery/Downloads/Mar 01.pdf`; Fedora LocalSend journal log HTTP `/register` at `14:32:01.741170` |
| Passport -> Fedora | `IMG_20261010_185044.jpg` | 1,640,388 B | 1,640,388 B | `aea2b9d48ec27243ae25b8d3396c3dfcf182d5ad43a36ead2c48b9300b520ba3` | `aea2b9d48ec27243ae25b8d3396c3dfcf182d5ad43a36ead2c48b9300b520ba3` | EXACT | 2026-10-13 12:09 | Saved as `IMG_20261010_185044 (2).jpg` in Downloads; recorded in flatpak `shared_preferences.json` |
| Fedora -> Passport | `Screenshot From 2025-10-24 10-33-53.png` | 183,144 B | 183,144 B | `ddb76b7dba40101f0d332aa4d832b9f4a5ce01c239c7e972439ea7f8d23589f7` | `ddb76b7dba40101f0d332aa4d832b9f4a5ce01c239c7e972439ea7f8d23589f7` | EXACT | 2026-10-13 12:10 | Received in Passport `shared/downloads/` and verified with full integrity |
| Passport -> Ubuntu (`Fast Onion`) | `IMG_20240202_162742.jpg` | 1,138,317 B | 1,138,317 B | `cbce1646becd8869e86f81e9cbb54823fbd776d03cb62a09f84417b3c28e65da` | `cbce1646becd8869e86f81e9cbb54823fbd776d03cb62a09f84417b3c28e65da` | EXACT | 2026-10-13 15:43 | Saved on ThinkPad-X1-Carbon-5th (`192.168.3.111`) `/home/ellery/Downloads/IMG_20240202_162742.jpg` |
| Ubuntu (`Fast Onion`) -> Passport | `Assessment.pdf` | 32,445 B | 32,445 B | `8469fd2112bc581355b4ddf506c12c56f83e82224047f9c4e9a54852fdf90257` | `8469fd2112bc581355b4ddf506c12c56f83e82224047f9c4e9a54852fdf90257` | EXACT | 2026-10-13 15:47 | Sent from ThinkPad-X1-Carbon-5th (`192.168.3.111`); saved in Passport `/accounts/1000/shared/downloads/Assessment.pdf` |

## Verification Commands & Outputs

### 1. Fedora PC (`Secret Tomato`, 192.168.3.123)

```sh
$ ls -la "/home/ellery/Downloads/Mar 01.pdf"
-rw-r--r--. 1 ellery ellery 56565 Oct 13 14:31 '/home/ellery/Downloads/Mar 01.pdf'

$ sha256sum "/home/ellery/Downloads/Mar 01.pdf"
4e6584fff8cf8e7c7e520e8247a13f237e5f63ca72b306b35923d9f67549f390  /home/ellery/Downloads/Mar 01.pdf
```

### 2. Ubuntu PC (`Fast Onion`, 192.168.3.111)

Terminal output from host `ellery@ellery-ThinkPad-X1-Carbon-5th`:

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

### 3. Remote REST Device Identity Probes

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

## Journal Log Correlation

From `tests/fedora_localsend_journal.log`:
```
Oct 13 14:32:01 fedora org.localsend.localsend_app.desktop[56483]: 2026-10-13 14:32:01.741170    [INFO] [Discovery] [DISCOVER/TCP] Received "/register" HTTP request: BlackBerry Passport (192.168.3.124)
Oct 13 14:32:56 fedora org.localsend.localsend_app.desktop[56483]: 2026-10-13 14:32:56.858441    [INFO] [Discovery] [DISCOVER/TCP] Received "/register" HTTP request: BlackBerry Passport (192.168.3.124)
Oct 13 14:33:26 fedora org.localsend.localsend_app.desktop[56483]: 2026-10-13 14:33:26.987043    [INFO] [Discovery] [DISCOVER/TCP] Received "/register" HTTP request: BlackBerry Passport (192.168.3.124)
Oct 13 14:33:38 fedora org.localsend.localsend_app.desktop[56483]: 2026-10-13 14:33:38.699390    [INFO] [Discovery] [DISCOVER/TCP] Received "/register" HTTP request: BlackBerry Passport (192.168.3.124)
```
