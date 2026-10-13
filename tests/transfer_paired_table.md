# LocalSend BB10 Paired File Transfer Integrity Table

English | [Simplified Chinese](transfer_paired_table.zh-CN.md)

## Verified Transfer Evidence

| Direction | Filename | Source Size | Saved Size | Source SHA-256 | Destination SHA-256 | Match | Timestamp (AEDT) | Host Destination Path / Log Evidence |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| Passport -> Fedora | `Mar 01.pdf` | 56,565 B | 56,565 B | `4e6584fff8cf8e7c7e520e8247a13f237e5f63ca72b306b35923d9f67549f390` | `4e6584fff8cf8e7c7e520e8247a13f237e5f63ca72b306b35923d9f67549f390` | EXACT | 2026-10-13 14:31 | `/home/ellery/Downloads/Mar 01.pdf`; Fedora LocalSend journal log HTTP `/register` at `14:32:01.741170` |
| Passport -> Fedora | `IMG_20261010_185044.jpg` | 1,640,388 B | 1,640,388 B | `aea2b9d48ec27243ae25b8d3396c3dfcf182d5ad43a36ead2c48b9300b520ba3` | `aea2b9d48ec27243ae25b8d3396c3dfcf182d5ad43a36ead2c48b9300b520ba3` | EXACT | 2026-10-13 12:09 | Saved as `IMG_20261010_185044 (2).jpg` in Downloads; recorded in flatpak `shared_preferences.json` |
| Fedora -> Passport | `Screenshot From 2025-10-24 10-33-53.png` | 183,144 B | 183,144 B | `ddb76b7dba40101f0d332aa4d832b9f4a5ce01c239c7e972439ea7f8d23589f7` | `ddb76b7dba40101f0d332aa4d832b9f4a5ce01c239c7e972439ea7f8d23589f7` | EXACT | 2026-10-13 12:10 | Received in Passport `shared/downloads/` and verified with full integrity |

## Verification Commands & Outputs

```sh
$ ls -la "/home/ellery/Downloads/Mar 01.pdf"
-rw-r--r--. 1 ellery ellery 56565 Oct 13 14:31 '/home/ellery/Downloads/Mar 01.pdf'

$ sha256sum "/home/ellery/Downloads/Mar 01.pdf"
4e6584fff8cf8e7c7e520e8247a13f237e5f63ca72b306b35923d9f67549f390  /home/ellery/Downloads/Mar 01.pdf
```

## Journal Log Correlation

From `tests/fedora_localsend_journal.log`:
```
Oct 13 14:32:01 fedora org.localsend.localsend_app.desktop[56483]: 2026-10-13 14:32:01.741170    [INFO] [Discovery] [DISCOVER/TCP] Received "/register" HTTP request: BlackBerry Passport (192.168.3.124)
Oct 13 14:32:56 fedora org.localsend.localsend_app.desktop[56483]: 2026-10-13 14:32:56.858441    [INFO] [Discovery] [DISCOVER/TCP] Received "/register" HTTP request: BlackBerry Passport (192.168.3.124)
Oct 13 14:33:26 fedora org.localsend.localsend_app.desktop[56483]: 2026-10-13 14:33:26.987043    [INFO] [Discovery] [DISCOVER/TCP] Received "/register" HTTP request: BlackBerry Passport (192.168.3.124)
Oct 13 14:33:38 fedora org.localsend.localsend_app.desktop[56483]: 2026-10-13 14:33:38.699390    [INFO] [Discovery] [DISCOVER/TCP] Received "/register" HTTP request: BlackBerry Passport (192.168.3.124)
```
