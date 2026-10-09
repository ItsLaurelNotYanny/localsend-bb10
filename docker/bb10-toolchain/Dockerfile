FROM debian:bookworm-slim

# Install 32-bit Wine, GNU make, build-essential, and Python
RUN dpkg --add-architecture i386 && \
    apt-get update -qq && \
    apt-get install -y --no-install-recommends \
        wine \
        wine32 \
        build-essential \
        make \
        python3 \
        ca-certificates && \
    rm -rf /var/lib/apt/lists/*

ENV WINEDEBUG=-all
ENV WINEARCH=win32
ENV WINEPREFIX=/root/.wine

# Pre-initialize Wine prefix
RUN wineboot --init

# Default BB10 NDK paths (mounted at /bbndk or pre-installed)
ENV QNX_TARGET=/bbndk/target_10_3_1_995/qnx6
ENV QNX_HOST=/bbndk/host_10_3_1_12/win32/x86
ENV QNX_CONFIGURATION=/bbndk/configuration
ENV MAKEFLAGS="-I/bbndk/target_10_3_1_995/qnx6/usr/include"

# Install wrappers and toolchain scripts
COPY bin/sanitize-makefile.py /usr/local/bin/sanitize-makefile.py
COPY bin/bb10-build /usr/local/bin/bb10-build
RUN chmod +x /usr/local/bin/sanitize-makefile.py /usr/local/bin/bb10-build

# Create wrapper scripts for Wine and BlackBerry Java tools in /usr/local/bin
RUN for tool in qcc ntoarmv7-gcc ntoarmv7-g++ ntoarmv7-ar ntoarmv7-ranlib ntoarmv7-strip ntoarm-objcopy ntoarmv7-objcopy qmake moc; do \
        case "$tool" in \
            moc) exename="moc.exe" ;; \
            ntoarm-objcopy) exename="ntoarmv7-objcopy.exe" ;; \
            *) exename="${tool}.exe" ;; \
        esac; \
        printf '#!/bin/bash\nexec wine "${QNX_HOST}/usr/bin/%s" "$@"\n' "$exename" > "/usr/local/bin/$tool"; \
        chmod +x "/usr/local/bin/$tool"; \
    done && \
    printf '#!/bin/bash\nexport PATH="/bbndk/features/com.qnx.tools.jre.linux.x86_64_1.7.0.51/jre/bin:${QNX_HOST}/usr/bin:$PATH"\nexec "${QNX_HOST}/usr/bin/blackberry-nativepackager" "$@"\n' > /usr/local/bin/blackberry-nativepackager && \
    chmod +x /usr/local/bin/blackberry-nativepackager && \
    printf '#!/bin/bash\nexport PATH="/bbndk/features/com.qnx.tools.jre.linux.x86_64_1.7.0.51/jre/bin:${QNX_HOST}/usr/bin:$PATH"\nexec "${QNX_HOST}/usr/bin/blackberry-deploy" "$@"\n' > /usr/local/bin/blackberry-deploy && \
    chmod +x /usr/local/bin/blackberry-deploy

WORKDIR /workspace

# Default entrypoint: build project mounted in /workspace
CMD ["bb10-build", "/workspace"]
