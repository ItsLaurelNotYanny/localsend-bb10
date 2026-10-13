# LocalSend for BlackBerry 10

English | [Simplified Chinese](README.zh-CN.md)

An unofficial, native C++ / Qt Cascades client bringing modern **[LocalSend](https://github.com/localsend/localsend)** LAN file sharing to legacy **BlackBerry 10** devices (BlackBerry Passport, Classic, Q10, Z30, etc.).

This project is built purely out of passion for retro hardware preservation—giving beloved legacy devices a second life as practical, seamless tools in modern local networks.

> [!NOTE]
> This project is an independent community effort and is **not** officially affiliated with or endorsed by the upstream LocalSend team. It implements the open [LocalSend Protocol v2](https://github.com/localsend/protocol).

---

## Features

- **Native Cascades UI**: Designed specifically for BB10 with native look and feel, responsive layout, and dark/light system theme support.
- **Local Network Discovery**:
  - Multicast (`224.0.0.167`) and broadcast UDP announcements.
  - Active subnet scanning with paced unicast fallback to bypass Wi-Fi AP client isolation.
  - Automatic peer card rendering with live status badges.
- **Seamless Bidirectional Transfers**:
  - **PC / Phone -> BlackBerry 10**: Instant transfer acceptance banner right on the Radar tab or via the Transfers tab.
  - **BlackBerry 10 -> PC / Phone**: Native Cascades file picker to select and send photos, documents, and media.
  - Full cryptographic integrity verification using streaming SHA-256 hashing.
- **Settings & Diagnostics**:
  - Customizable device alias and network port.
  - Built-in diagnostic log viewer via BlackBerry 10 Docs To Go.

---

## Prerequisites & Installation

Due to the shutdown of official BlackBerry signing infrastructure in 2022, installing custom native applications on BlackBerry 10 requires the device to be rooted.

### 1. Rooting your BB10 Device
If your device is not yet rooted, you can achieve root using community-developed tools such as **bb10root** / **bb10mt** (BlackBerry 10 MultiTool):
- Tools and community guides can be found at [bb10.root.sx](https://bb10.root.sx) and the [r/BlackberryPhoenix](https://www.reddit.com/r/BlackberryPhoenix/) community.
- **Safety note**: The custom autoloader / root image is generated locally on your machine by unpacking and patching official BlackBerry signed firmware autoloaders. It does not introduce unknown third-party binaries into the core system, making it transparent and verifiable.

### 2. Installing LocalSend BAR
Once rooted, you can deploy the pre-built `.bar` package directly via `blackberry-deploy` or standard BB10 management tools:
```sh
# Deploying via BlackBerry 10 WebWorks / NDK tooling
blackberry-deploy -installApp -device <DEVICE_IP> -password <DEVICE_PASSWORD> localsend-bb10.bar
```

---

## Interoperability with Official LocalSend Clients

To share files between your BlackBerry 10 device and modern clients (Windows, macOS, Linux, Android, iOS):
1. Ensure both devices are connected to the same local Wi-Fi / LAN network.
2. In the official LocalSend client on your PC or smartphone, navigate to **Settings** and ensure **Encryption (HTTPS)** is disabled (set to standard HTTP mode), as the BB10 client communicates over standard HTTP.
3. Your BlackBerry device will appear automatically in the nearby devices list!

---

## Documentation & Developer Guide

- [Architecture & Protocol Notes](docs/protocol-notes.md)
- [Build & Deployment Instructions](docs/build-deploy.md)
- [Paired Transfer Test Records](tests/transfer_paired_table.md)
- [Contributor & Agent Instructions](AGENTS.md)

---

## License

This project is licensed under the [MIT License](LICENSE).
