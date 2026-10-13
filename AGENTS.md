# Contributor Instructions

English | [Simplified Chinese](AGENTS.zh-CN.md)

- Write source code, identifiers, comments, docstrings, build-script comments, logs, and technical messages in English.
- Maintain every project document in English and Simplified Chinese. Use the default `.md` name for English and `.zh-CN.md` for Chinese; include language-switch links and update both together.
- Agent reading policy: When inspecting or reading project context, agents only need to read the English version (or user-specified files); do not redundantly read both language versions. Manually created documents without translations may be used directly. Chinese documents are primarily for human users and readers.
- Keep code samples and their comments in English in both documentation versions. Non-English filename fixtures may use Unicode escapes to test the required data without translating code.
- Before implementation, read README.md, docs/mvp-plan.md, docs/protocol-notes.md, and docs/build-deploy.md. Current scope is planning only until implementation is requested.
- Bidirectional BB10-to-PC and PC-to-BB10 transfers are mandatory for the MVP. Full Cascades pages may follow protocol validation; sending and receiving may not.
