# LocalSend for BlackBerry 10



This is an unofficial native C++/Qt client for LocalSend, targeting BlackBerry 10 devices. It implements the LocalSend protocol ([localsend/protocol](https://github.com/localsend/protocol)) and is inspired by the official implementation ([localsend/localsend](https://github.com/localsend/localsend)).

LocalSend client implementation for BlackBerry 10 with verified bidirectional file transfer.

Target: BB10 10.3.x and Qt4.8, with a native Cascades UI as the final goal. The MVP has two milestones: verify the protocol on a BB10 device, then complete the everyday UI. Full Cascades pages may be deferred. Scope: IPv4 LAN, HTTP, foreground operation, and one ordinary file per transfer. When receiving multiple offered files, accept at most one. HTTPS, directories, resumable transfers, the Download API, background services, and PIN entry are outside this MVP.

For bidirectional HTTP-only interoperability testing, disable encryption in the official PC LocalSend client. BB10 receives ordinary files through the Upload API, so its announcement sets `download` to `false`; this does not disable receiving files.

Bidirectional transfers are mandatory for the MVP: BB10 to PC and PC to BB10 must both transfer a 10 MiB file successfully, with matching SHA-256 hashes. The full UI may be deferred; sending and receiving may not.


- [Protocol baseline and interface constraints](docs/protocol-notes.md)
- [Toolchain, deployment, and acceptance records](docs/build-deploy.md)


Code, identifiers, comments, docstrings, and technical messages use English. 

The project name and description do not imply official LocalSend support. Licensed under the [MIT License](LICENSE).
