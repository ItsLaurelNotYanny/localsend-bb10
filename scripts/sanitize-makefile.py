import sys
import re

makefile = sys.argv[1] if len(sys.argv) > 1 else "Makefile"

with open(makefile, "r", encoding="utf-8", errors="ignore") as f:
    content = f.read()

# Replace Windows drive letters for bbndk
content = re.sub(r"[zZ]:[/\\]+bbndk", "/bbndk", content)

# Replace all backslashes in paths, preserving line continuations
lines = content.splitlines(True)
new_lines = []
for line in lines:
    stripped = line.rstrip("\r\n")
    if stripped.endswith("\\"):
        prefix = stripped[:-1]
        prefix = prefix.replace("\\", "/")
        new_lines.append(prefix + "\\" + line[len(stripped):])
    else:
        new_lines.append(line.replace("\\", "/"))

content = "".join(new_lines)

# Replace moc.exe path with moc wrapper
content = re.sub(r"/bbndk/host_10_3_1_12/win32/x86/usr/bin/moc\.exe", "moc", content)

# Replace QMAKE command with true to avoid re-invoking windows qmake.exe
content = re.sub(r"^QMAKE\s*=.*$", "QMAKE = true", content, flags=re.MULTILINE)

with open(makefile, "w", encoding="utf-8") as f:
    f.write(content)

print(f"[sanitize-makefile] Successfully sanitized {makefile}")
