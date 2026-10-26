#include <posix_tls/posix_tls.h>
#include <posix_tls/posix_tls.hpp>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string.h>
#include <string>
#include <vector>
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>

namespace {

const unsigned char MAGIC_BYTES[4] = {'L', 'S', 'S', 'D'};
const size_t HEADER_SIZE = 4 + 8 + 64; // magic(4) + len(8) + sha256_hex(64) = 76 bytes

double get_time_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return static_cast<double>(tv.tv_sec) * 1000.0 + static_cast<double>(tv.tv_usec) / 1000.0;
}

bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) >= 0;
}

bool wait_fd(int fd, bool for_write, int timeout_ms) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int rc = select(fd + 1, for_write ? NULL : &fds, for_write ? &fds : NULL, NULL, &tv);
    return rc > 0;
}

void pack_u64_be(unsigned char* buf, unsigned long long val) {
    buf[0] = static_cast<unsigned char>((val >> 56) & 0xFF);
    buf[1] = static_cast<unsigned char>((val >> 48) & 0xFF);
    buf[2] = static_cast<unsigned char>((val >> 40) & 0xFF);
    buf[3] = static_cast<unsigned char>((val >> 32) & 0xFF);
    buf[4] = static_cast<unsigned char>((val >> 24) & 0xFF);
    buf[5] = static_cast<unsigned char>((val >> 16) & 0xFF);
    buf[6] = static_cast<unsigned char>((val >> 8) & 0xFF);
    buf[7] = static_cast<unsigned char>(val & 0xFF);
}

unsigned long long unpack_u64_be(const unsigned char* buf) {
    unsigned long long val = 0;
    for (int i = 0; i < 8; ++i) {
        val = (val << 8) | static_cast<unsigned long long>(buf[i]);
    }
    return val;
}

std::string read_file_string(const std::string& path) {
    std::ifstream ifs(path.c_str(), std::ios::in | std::ios::binary);
    if (!ifs.is_open()) return "";
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

bool safe_write_file(const std::string& path, const char* data, size_t len, mode_t mode, bool* created_out = NULL) {
    if (created_out) *created_out = false;
    if (path.empty()) return false;
    // O_CREAT | O_EXCL rejects preexisting identity files; O_NOFOLLOW prevents symlink traversal
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, mode);
    if (fd < 0) return false;
    if (created_out) *created_out = true;

    // Explicitly enforce mode regardless of ambient umask and verify result
    if (fchmod(fd, mode) != 0) {
        close(fd);
        unlink(path.c_str());
        if (created_out) *created_out = false;
        return false;
    }

    size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, data + written, len - written);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            close(fd);
            unlink(path.c_str());
            if (created_out) *created_out = false;
            return false;
        }
        written += static_cast<size_t>(n);
    }
    if (fsync(fd) < 0) {
        close(fd);
        unlink(path.c_str());
        if (created_out) *created_out = false;
        return false;
    }
    if (close(fd) < 0) {
        unlink(path.c_str());
        if (created_out) *created_out = false;
        return false;
    }
    return true;
}

int perform_handshake(posix_tls_connection_t* conn, int fd, bool is_server, int timeout_ms, std::string& err_msg) {
    double deadline = get_time_ms() + static_cast<double>(timeout_ms);
    while (get_time_ms() < deadline) {
        int rc = is_server ? posix_tls_accept(conn) : posix_tls_connect(conn, NULL);
        if (rc == POSIX_TLS_OK) {
            return POSIX_TLS_OK;
        } else if (rc == POSIX_TLS_ERR_WANT_READ || rc == POSIX_TLS_ERR_WANT_WRITE) {
            int rem = static_cast<int>(deadline - get_time_ms());
            if (rem <= 0) break;
            wait_fd(fd, rc == POSIX_TLS_ERR_WANT_WRITE, rem > 50 ? 50 : rem);
        } else {
            const char* detail = posix_tls_get_last_error(conn);
            err_msg = detail ? detail : posix_tls_strerror(rc);
            return rc;
        }
    }
    err_msg = "Handshake deadline exceeded";
    return POSIX_TLS_ERR_IO_FAILED;
}

int write_all_tls(posix_tls_connection_t* conn, int fd, const unsigned char* data, size_t total, int timeout_ms) {
    size_t written_total = 0;
    double deadline = get_time_ms() + static_cast<double>(timeout_ms);
    while (written_total < total) {
        if (get_time_ms() >= deadline) return POSIX_TLS_ERR_IO_FAILED;
        size_t n = 0;
        int rc = posix_tls_write(conn, data + written_total, total - written_total, &n);
        if (rc == POSIX_TLS_OK) {
            written_total += n;
        } else if (rc == POSIX_TLS_ERR_WANT_READ || rc == POSIX_TLS_ERR_WANT_WRITE) {
            int rem = static_cast<int>(deadline - get_time_ms());
            if (rem <= 0) return POSIX_TLS_ERR_IO_FAILED;
            wait_fd(fd, rc == POSIX_TLS_ERR_WANT_WRITE, rem > 50 ? 50 : rem);
        } else {
            return rc;
        }
    }
    return POSIX_TLS_OK;
}

int read_exact_tls(posix_tls_connection_t* conn, int fd, unsigned char* buf, size_t total, int timeout_ms) {
    size_t read_total = 0;
    double deadline = get_time_ms() + static_cast<double>(timeout_ms);
    while (read_total < total) {
        if (get_time_ms() >= deadline) return POSIX_TLS_ERR_IO_FAILED;
        size_t n = 0;
        int rc = posix_tls_read(conn, buf + read_total, total - read_total, &n);
        if (rc == POSIX_TLS_OK) {
            if (n == 0) return POSIX_TLS_ERR_CLOSED;
            read_total += n;
        } else if (rc == POSIX_TLS_ERR_WANT_READ || rc == POSIX_TLS_ERR_WANT_WRITE) {
            int rem = static_cast<int>(deadline - get_time_ms());
            if (rem <= 0) return POSIX_TLS_ERR_IO_FAILED;
            wait_fd(fd, rc == POSIX_TLS_ERR_WANT_WRITE, rem > 50 ? 50 : rem);
        } else {
            return rc;
        }
    }
    return POSIX_TLS_OK;
}

void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " <command> [options]\n\n"
              << "Commands:\n"
              << "  generate-identity  Generate self-signed EC P-256 identity and print fingerprint\n"
              << "                     --cert <file>               Path to save certificate PEM\n"
              << "                     --key <file>                Path to save private key PEM\n\n"
              << "  fingerprint        Calculate SHA-256 fingerprint of a PEM certificate\n"
              << "                     --cert <file>               Path to certificate PEM\n\n"
              << "  server             Run LocalSend-style TLS receiver\n"
              << "                     --port <port>               Port to listen on (default: 8443)\n"
              << "                     --cert <file>               Server certificate PEM\n"
              << "                     --key <file>                Server private key PEM\n"
              << "                     --output <file>             Path to save committed received file\n"
              << "                     --min-tls-version <version> TLSv1.2 or TLSv1.3 (default: TLSv1.3)\n"
              << "                     --expected-pin <pin>        Expected client pin (optional)\n"
              << "                     --timeout-ms <ms>           Operation timeout in ms (default: 10000)\n"
              << "                     --disconnect-after <bytes>  Simulate disconnect after bytes (test hook)\n\n"
              << "  client             Run LocalSend-style TLS sender\n"
              << "                     --host <host>               Target host (default: 127.0.0.1)\n"
              << "                     --port <port>               Target port (default: 8443)\n"
              << "                     --file <file>               Input file to transmit\n"
              << "                     --expected-pin <pin>        Expected server certificate fingerprint\n"
              << "                     --cert <file>               Client certificate PEM (optional)\n"
              << "                     --key <file>                Client private key PEM (optional)\n"
              << "                     --min-tls-version <version> TLSv1.2 or TLSv1.3 (default: TLSv1.3)\n"
              << "                     --timeout-ms <ms>           Operation timeout in ms (default: 10000)\n"
              << "                     --disconnect-after <bytes>  Simulate disconnect after bytes (test hook)\n"
              << std::endl;
}

int cmd_generate_identity(int argc, char** argv) {
    std::string cert_file, key_file;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--cert" && i + 1 < argc) cert_file = argv[++i];
        else if (arg == "--key" && i + 1 < argc) key_file = argv[++i];
    }

    if (cert_file.empty() || key_file.empty()) {
        std::cerr << "Error: --cert and --key are required for generate-identity" << std::endl;
        return 1;
    }

    char* cert_pem = NULL;
    size_t cert_len = 0;
    char* key_pem = NULL;
    size_t key_len = 0;

    int rc = posix_tls_generate_self_signed_cert(&cert_pem, &cert_len, &key_pem, &key_len);
    if (rc != POSIX_TLS_OK) {
        std::cerr << "Error generating self-signed certificate: " << posix_tls_strerror(rc) << std::endl;
        return 1;
    }

    bool cert_created = false;
    bool key_created = false;

    // Safely write certificate with mode 0644
    if (!safe_write_file(cert_file, cert_pem, cert_len, 0644, &cert_created)) {
        std::cerr << "Error: failed to write certificate to " << cert_file << " (" << ::strerror(errno) << ")" << std::endl;
        posix_tls_free_string(cert_pem);
        posix_tls_free_string(key_pem);
        return 1;
    }

    // Safely write private key with owner-only mode 0600
    if (!safe_write_file(key_file, key_pem, key_len, 0600, &key_created)) {
        std::cerr << "Error: failed to write private key to " << key_file << " (" << ::strerror(errno) << ")" << std::endl;
        if (cert_created) unlink(cert_file.c_str());
        posix_tls_free_string(cert_pem);
        posix_tls_free_string(key_pem);
        return 1;
    }

    char fingerprint[65];
    rc = posix_tls_calculate_cert_fingerprint(cert_pem, cert_len, fingerprint);
    posix_tls_free_string(cert_pem);
    posix_tls_free_string(key_pem);

    if (rc != POSIX_TLS_OK) {
        std::cerr << "Error calculating certificate fingerprint: " << posix_tls_strerror(rc) << std::endl;
        if (cert_created) unlink(cert_file.c_str());
        if (key_created) unlink(key_file.c_str());
        return 1;
    }

    std::cout << "FINGERPRINT=" << fingerprint << std::endl;
    return 0;
}

int cmd_fingerprint(int argc, char** argv) {
    std::string cert_file;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--cert" && i + 1 < argc) cert_file = argv[++i];
    }

    if (cert_file.empty()) {
        std::cerr << "Error: --cert is required for fingerprint" << std::endl;
        return 1;
    }

    std::string cert_pem = read_file_string(cert_file);
    if (cert_pem.empty()) {
        std::cerr << "Error reading certificate file: " << cert_file << std::endl;
        return 1;
    }

    char fingerprint[65];
    int rc = posix_tls_calculate_cert_fingerprint(cert_pem.c_str(), cert_pem.size(), fingerprint);
    if (rc != POSIX_TLS_OK) {
        std::cerr << "Error calculating fingerprint: " << posix_tls_strerror(rc) << std::endl;
        return 1;
    }

    std::cout << "FINGERPRINT=" << fingerprint << std::endl;
    return 0;
}

int cmd_server(int argc, char** argv) {
    int port = 8443;
    std::string cert_file, key_file, output_file;
    std::string min_ver_str = "TLSv1.3";
    std::string expected_pin;
    int timeout_ms = 10000;
    unsigned long long disconnect_after = 0;

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) port = std::atoi(argv[++i]);
        else if (arg == "--cert" && i + 1 < argc) cert_file = argv[++i];
        else if (arg == "--key" && i + 1 < argc) key_file = argv[++i];
        else if (arg == "--output" && i + 1 < argc) output_file = argv[++i];
        else if (arg == "--min-tls-version" && i + 1 < argc) min_ver_str = argv[++i];
        else if (arg == "--expected-pin" && i + 1 < argc) expected_pin = argv[++i];
        else if (arg == "--timeout-ms" && i + 1 < argc) timeout_ms = std::atoi(argv[++i]);
        else if (arg == "--disconnect-after" && i + 1 < argc) disconnect_after = strtoull(argv[++i], NULL, 10);
    }

    if (cert_file.empty() || key_file.empty() || output_file.empty()) {
        std::cerr << "Error: --cert, --key, and --output are required for server" << std::endl;
        return 1;
    }

    std::string cert_pem = read_file_string(cert_file);
    std::string key_pem = read_file_string(key_file);
    if (cert_pem.empty() || key_pem.empty()) {
        std::cerr << "Error reading credentials from files" << std::endl;
        return 1;
    }

    // Create listening socket
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        std::cerr << "Error creating socket: " << ::strerror(errno) << std::endl;
        return 1;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in saddr;
    std::memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    saddr.sin_port = htons(static_cast<uint16_t>(port));

    if (bind(listen_fd, reinterpret_cast<struct sockaddr*>(&saddr), sizeof(saddr)) < 0) {
        std::cerr << "Error binding to port " << port << ": " << ::strerror(errno) << std::endl;
        close(listen_fd);
        return 1;
    }

    if (listen(listen_fd, 5) < 0) {
        std::cerr << "Error listening on socket: " << ::strerror(errno) << std::endl;
        close(listen_fd);
        return 1;
    }

    std::cout << "LISTENING port=" << port << std::endl;
    std::cout.flush();

    // Wait for incoming connection with timeout
    if (!wait_fd(listen_fd, false, timeout_ms)) {
        std::cerr << "Timeout waiting for client connection" << std::endl;
        close(listen_fd);
        return 2;
    }

    struct sockaddr_in caddr;
    socklen_t clen = sizeof(caddr);
    int client_fd = accept(listen_fd, reinterpret_cast<struct sockaddr*>(&caddr), &clen);
    close(listen_fd); // We only serve one connection in demo mode

    if (client_fd < 0) {
        std::cerr << "Error accepting connection: " << ::strerror(errno) << std::endl;
        return 1;
    }

    set_nonblocking(client_fd);

    // Initialize posix_tls context
    posix_tls_context_t* ctx = posix_tls_context_create(POSIX_TLS_POLICY_P2P);
    if (!ctx) {
        std::cerr << "Error creating posix_tls context" << std::endl;
        close(client_fd);
        return 1;
    }

    int rc = posix_tls_context_set_credentials(ctx, cert_pem.c_str(), cert_pem.size(), key_pem.c_str(), key_pem.size());
    if (rc != POSIX_TLS_OK) {
        std::cerr << "Error setting credentials: " << posix_tls_strerror(rc) << std::endl;
        posix_tls_context_destroy(ctx);
        close(client_fd);
        return 1;
    }

    posix_tls_version_t min_ver = (min_ver_str == "TLSv1.2") ? POSIX_TLS_VERSION_1_2 : POSIX_TLS_VERSION_1_3;
    posix_tls_context_set_min_tls_version(ctx, min_ver);

    if (!expected_pin.empty()) {
        rc = posix_tls_context_set_expected_fingerprint(ctx, expected_pin.c_str());
        if (rc != POSIX_TLS_OK) {
            std::cerr << "Error setting expected fingerprint: " << posix_tls_strerror(rc) << std::endl;
            posix_tls_context_destroy(ctx);
            close(client_fd);
            return 1;
        }
    }

    posix_tls_connection_t* conn = posix_tls_connection_create(ctx, client_fd);
    if (!conn) {
        std::cerr << "Error creating posix_tls connection" << std::endl;
        posix_tls_context_destroy(ctx);
        close(client_fd);
        return 1;
    }
    // Note: Once posix_tls_connection_create succeeds, conn owns client_fd.
    // Do NOT close client_fd manually on cleanup paths; posix_tls_connection_destroy handles it.

    // TLS Accept Handshake
    double t_start = get_time_ms();
    std::string err_msg;
    rc = perform_handshake(conn, client_fd, true, timeout_ms, err_msg);
    double t_handshake = get_time_ms() - t_start;

    if (rc != POSIX_TLS_OK) {
        std::cerr << "Server TLS handshake failed: " << err_msg << " (rc=" << rc << ")" << std::endl;
        posix_tls_connection_destroy(conn);
        posix_tls_context_destroy(ctx);
        return (rc == POSIX_TLS_ERR_PIN_MISMATCH) ? 6 : 2;
    }

    char version_buf[32] = {0};
    char cipher_buf[64] = {0};
    posix_tls_connection_get_negotiated_version(conn, version_buf, sizeof(version_buf));
    posix_tls_connection_get_cipher(conn, cipher_buf, sizeof(cipher_buf));

    std::cout << "HANDSHAKE_OK version=" << version_buf << " cipher=" << cipher_buf
              << " duration_ms=" << std::fixed << std::setprecision(2) << t_handshake << std::endl;
    std::cout.flush();

    // Read header (76 bytes: magic + length + sha256)
    unsigned char header_buf[HEADER_SIZE];
    rc = read_exact_tls(conn, client_fd, header_buf, HEADER_SIZE, timeout_ms);
    if (rc != POSIX_TLS_OK) {
        std::cerr << "Error reading transfer header: " << posix_tls_strerror(rc) << std::endl;
        posix_tls_shutdown(conn);
        posix_tls_connection_destroy(conn);
        posix_tls_context_destroy(ctx);
        return 2;
    }

    if (std::memcmp(header_buf, MAGIC_BYTES, 4) != 0) {
        std::cerr << "Error: invalid magic bytes in header" << std::endl;
        posix_tls_shutdown(conn);
        posix_tls_connection_destroy(conn);
        posix_tls_context_destroy(ctx);
        return 2;
    }

    unsigned long long expected_bytes = unpack_u64_be(header_buf + 4);
    std::string expected_sha256(reinterpret_cast<const char*>(header_buf + 12), 64);

    // Create an exclusive, secure temporary regular file using mkstemp in output directory
    std::string dir = ".";
    size_t last_slash = output_file.rfind('/');
    if (last_slash != std::string::npos) {
        dir = output_file.substr(0, last_slash);
        if (dir.empty()) dir = "/";
    }
    std::string tmpl = dir + "/.lssd_recv_XXXXXX";
    std::vector<char> tmpl_buf(tmpl.begin(), tmpl.end());
    tmpl_buf.push_back('\0');
    int tmp_fd = mkstemp(&tmpl_buf[0]);
    if (tmp_fd < 0) {
        std::cerr << "Error creating temporary file in " << dir << ": " << ::strerror(errno) << std::endl;
        posix_tls_shutdown(conn);
        posix_tls_connection_destroy(conn);
        posix_tls_context_destroy(ctx);
        return 1;
    }
    std::string tmp_output = &tmpl_buf[0];

    unsigned char chunk[16384];
    unsigned long long received_bytes = 0;
    bool aborted = false;
    double deadline = get_time_ms() + static_cast<double>(timeout_ms);

    while (received_bytes < expected_bytes) {
        if (get_time_ms() >= deadline) {
            std::cerr << "Timeout waiting for payload data" << std::endl;
            aborted = true;
            break;
        }

        size_t to_read = sizeof(chunk);
        if (expected_bytes - received_bytes < static_cast<unsigned long long>(to_read)) {
            to_read = static_cast<size_t>(expected_bytes - received_bytes);
        }

        size_t n = 0;
        rc = posix_tls_read(conn, chunk, to_read, &n);
        if (rc == POSIX_TLS_OK) {
            if (n == 0) {
                std::cerr << "Connection closed prematurely by peer after " << received_bytes << " bytes" << std::endl;
                aborted = true;
                break;
            }
            ssize_t wn = write(tmp_fd, chunk, n);
            if (wn != static_cast<ssize_t>(n)) {
                std::cerr << "Error writing payload to temporary file: " << ::strerror(errno) << std::endl;
                aborted = true;
                break;
            }
            received_bytes += n;

            if (disconnect_after > 0 && received_bytes >= disconnect_after) {
                std::cout << "Test hook triggered: intentionally disconnecting server after " << received_bytes << " bytes" << std::endl;
                aborted = true;
                break;
            }
        } else if (rc == POSIX_TLS_ERR_WANT_READ || rc == POSIX_TLS_ERR_WANT_WRITE) {
            int rem = static_cast<int>(deadline - get_time_ms());
            if (rem <= 0) {
                std::cerr << "Timeout waiting for socket I/O" << std::endl;
                aborted = true;
                break;
            }
            wait_fd(client_fd, rc == POSIX_TLS_ERR_WANT_WRITE, rem > 50 ? 50 : rem);
        } else {
            std::cerr << "Error reading payload data: " << posix_tls_strerror(rc) << std::endl;
            aborted = true;
            break;
        }
    }
    if (aborted || received_bytes != expected_bytes) {
        close(tmp_fd);
        unlink(tmp_output.c_str());
        posix_tls_shutdown(conn);
        posix_tls_connection_destroy(conn);
        posix_tls_context_destroy(ctx);
        return 4; // Transfer aborted / disconnected
    }

    if (fsync(tmp_fd) != 0) {
        std::cerr << "Error syncing temporary file: " << ::strerror(errno) << std::endl;
        close(tmp_fd);
        unlink(tmp_output.c_str());
        posix_tls_shutdown(conn);
        posix_tls_connection_destroy(conn);
        posix_tls_context_destroy(ctx);
        return 1;
    }

    if (close(tmp_fd) != 0) {
        std::cerr << "Error closing temporary file: " << ::strerror(errno) << std::endl;
        unlink(tmp_output.c_str());
        posix_tls_shutdown(conn);
        posix_tls_connection_destroy(conn);
        posix_tls_context_destroy(ctx);
        return 1;
    }

    // Verify SHA-256 of received file
    char actual_sha256[65];
    rc = posix_tls_sha256_file(tmp_output.c_str(), actual_sha256);
    if (rc != POSIX_TLS_OK || expected_sha256 != actual_sha256) {
        std::cerr << "TRANSFER_FAILED reason=checksum_mismatch expected=" << expected_sha256
                  << " actual=" << actual_sha256 << std::endl;
        unlink(tmp_output.c_str());
        std::string nak = "ERR: SHA256 MISMATCH\n";
        write_all_tls(conn, client_fd, reinterpret_cast<const unsigned char*>(nak.data()), nak.size(), timeout_ms);
        posix_tls_shutdown(conn);
        posix_tls_connection_destroy(conn);
        posix_tls_context_destroy(ctx);
        return 3;
    }

    // Atomic commit to target output file (POSIX rename atomically overwrites destination symlink without following)
    if (std::rename(tmp_output.c_str(), output_file.c_str()) != 0) {
        std::cerr << "Error renaming temporary file to " << output_file << ": " << ::strerror(errno) << std::endl;
        unlink(tmp_output.c_str());
        posix_tls_shutdown(conn);
        posix_tls_connection_destroy(conn);
        posix_tls_context_destroy(ctx);
        return 1;
    }

    // Send ACK to client and check transmission result
    std::string ack = "OK: SHA256 MATCH " + std::string(actual_sha256) + "\n";
    rc = write_all_tls(conn, client_fd, reinterpret_cast<const unsigned char*>(ack.data()), ack.size(), timeout_ms);
    if (rc != POSIX_TLS_OK) {
        std::cerr << "TRANSFER_COMMITTED_BUT_ACK_FAILED reason=ack_delivery_failed rc=" << rc << " output=" << output_file << std::endl;
        posix_tls_shutdown(conn);
        posix_tls_connection_destroy(conn);
        posix_tls_context_destroy(ctx);
        return 5;
    }

    std::cout << "TRANSFER_OK bytes=" << received_bytes << " sha256=" << actual_sha256 << " output=" << output_file << std::endl;
    std::cout.flush();

    posix_tls_shutdown(conn);
    posix_tls_connection_destroy(conn);
    posix_tls_context_destroy(ctx);
    return 0;
}

int cmd_client(int argc, char** argv) {
    std::string host = "127.0.0.1";
    int port = 8443;
    std::string input_file, expected_pin;
    std::string cert_file, key_file;
    std::string min_ver_str = "TLSv1.3";
    int timeout_ms = 10000;
    unsigned long long disconnect_after = 0;

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) host = argv[++i];
        else if (arg == "--port" && i + 1 < argc) port = std::atoi(argv[++i]);
        else if (arg == "--file" && i + 1 < argc) input_file = argv[++i];
        else if (arg == "--expected-pin" && i + 1 < argc) expected_pin = argv[++i];
        else if (arg == "--cert" && i + 1 < argc) cert_file = argv[++i];
        else if (arg == "--key" && i + 1 < argc) key_file = argv[++i];
        else if (arg == "--min-tls-version" && i + 1 < argc) min_ver_str = argv[++i];
        else if (arg == "--timeout-ms" && i + 1 < argc) timeout_ms = std::atoi(argv[++i]);
        else if (arg == "--disconnect-after" && i + 1 < argc) disconnect_after = strtoull(argv[++i], NULL, 10);
    }

    if (input_file.empty() || expected_pin.empty()) {
        std::cerr << "Error: --file and --expected-pin are required for client" << std::endl;
        return 1;
    }

    char file_sha256[65];
    int rc = posix_tls_sha256_file(input_file.c_str(), file_sha256);
    if (rc != POSIX_TLS_OK) {
        std::cerr << "Error computing SHA-256 for file " << input_file << ": " << posix_tls_strerror(rc) << std::endl;
        return 1;
    }

    std::ifstream file_in(input_file.c_str(), std::ios::in | std::ios::binary | std::ios::ate);
    if (!file_in.is_open()) {
        std::cerr << "Error opening file " << input_file << std::endl;
        return 1;
    }
    unsigned long long file_size = static_cast<unsigned long long>(file_in.tellg());
    file_in.seekg(0, std::ios::beg);

    // Create client TCP socket
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        std::cerr << "Error creating socket: " << ::strerror(errno) << std::endl;
        return 1;
    }

    struct sockaddr_in saddr;
    std::memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, host.c_str(), &saddr.sin_addr) <= 0) {
        struct hostent* he = gethostbyname(host.c_str());
        if (!he) {
            std::cerr << "Error resolving host: " << host << std::endl;
            close(sock_fd);
            return 1;
        }
        std::memcpy(&saddr.sin_addr, he->h_addr_list[0], he->h_length);
    }

    // Set non-blocking before connect to enforce finite connect timeout
    set_nonblocking(sock_fd);
    if (connect(sock_fd, reinterpret_cast<struct sockaddr*>(&saddr), sizeof(saddr)) < 0) {
        if (errno != EINPROGRESS) {
            std::cerr << "Error connecting to " << host << ":" << port << ": " << ::strerror(errno) << std::endl;
            close(sock_fd);
            return 1;
        }
        if (!wait_fd(sock_fd, true, timeout_ms)) {
            std::cerr << "Timeout connecting to " << host << ":" << port << std::endl;
            close(sock_fd);
            return 1;
        }
        int so_err = 0;
        socklen_t len = sizeof(so_err);
        if (getsockopt(sock_fd, SOL_SOCKET, SO_ERROR, &so_err, &len) < 0 || so_err != 0) {
            std::cerr << "Error connecting to " << host << ":" << port << ": "
                      << ::strerror(so_err != 0 ? so_err : errno) << std::endl;
            close(sock_fd);
            return 1;
        }
    }

    // Initialize posix_tls context
    posix_tls_context_t* ctx = posix_tls_context_create(POSIX_TLS_POLICY_P2P);
    if (!ctx) {
        std::cerr << "Error creating posix_tls context" << std::endl;
        close(sock_fd);
        return 1;
    }

    rc = posix_tls_context_set_expected_fingerprint(ctx, expected_pin.c_str());
    if (rc != POSIX_TLS_OK) {
        std::cerr << "Error setting expected fingerprint pin: " << posix_tls_strerror(rc) << std::endl;
        posix_tls_context_destroy(ctx);
        close(sock_fd);
        return 1;
    }

    posix_tls_version_t min_ver = (min_ver_str == "TLSv1.2") ? POSIX_TLS_VERSION_1_2 : POSIX_TLS_VERSION_1_3;
    posix_tls_context_set_min_tls_version(ctx, min_ver);

    if (!cert_file.empty() && !key_file.empty()) {
        std::string cert_pem = read_file_string(cert_file);
        std::string key_pem = read_file_string(key_file);
        rc = posix_tls_context_set_credentials(ctx, cert_pem.c_str(), cert_pem.size(), key_pem.c_str(), key_pem.size());
        if (rc != POSIX_TLS_OK) {
            std::cerr << "Error setting client credentials: " << posix_tls_strerror(rc) << std::endl;
            posix_tls_context_destroy(ctx);
            close(sock_fd);
            return 1;
        }
    }

    posix_tls_connection_t* conn = posix_tls_connection_create(ctx, sock_fd);
    if (!conn) {
        std::cerr << "Error creating posix_tls connection" << std::endl;
        posix_tls_context_destroy(ctx);
        close(sock_fd);
        return 1;
    }
    // Note: Once posix_tls_connection_create succeeds, conn owns sock_fd.
    // Do NOT close sock_fd manually on cleanup paths; posix_tls_connection_destroy handles it.

    // TLS Connect Handshake
    double t_start = get_time_ms();
    std::string err_msg;
    rc = perform_handshake(conn, sock_fd, false, timeout_ms, err_msg);
    double t_handshake = get_time_ms() - t_start;

    if (rc != POSIX_TLS_OK) {
        if (rc == POSIX_TLS_ERR_PIN_MISMATCH) {
            std::cerr << "ERROR: PIN_MISMATCH (posix_tls_connect failed with POSIX_TLS_ERR_PIN_MISMATCH: "
                      << err_msg << ")" << std::endl;
            posix_tls_connection_destroy(conn);
            posix_tls_context_destroy(ctx);
            return 6; // Distinct code 6 for POSIX_TLS_ERR_PIN_MISMATCH
        }
        std::cerr << "Client TLS handshake failed: " << err_msg << " (rc=" << rc << ")" << std::endl;
        posix_tls_connection_destroy(conn);
        posix_tls_context_destroy(ctx);
        return 2;
    }

    char version_buf[32] = {0};
    char cipher_buf[64] = {0};
    posix_tls_connection_get_negotiated_version(conn, version_buf, sizeof(version_buf));
    posix_tls_connection_get_cipher(conn, cipher_buf, sizeof(cipher_buf));

    std::cout << "HANDSHAKE_OK version=" << version_buf << " cipher=" << cipher_buf
              << " duration_ms=" << std::fixed << std::setprecision(2) << t_handshake << std::endl;
    std::cout.flush();

    // Prepare header
    unsigned char header[HEADER_SIZE];
    std::memcpy(header, MAGIC_BYTES, 4);
    pack_u64_be(header + 4, file_size);
    std::memcpy(header + 12, file_sha256, 64);

    double t_transfer_start = get_time_ms();

    // Send header
    rc = write_all_tls(conn, sock_fd, header, HEADER_SIZE, timeout_ms);
    if (rc != POSIX_TLS_OK) {
        std::cerr << "Error sending transfer header: " << posix_tls_strerror(rc) << std::endl;
        posix_tls_shutdown(conn);
        posix_tls_connection_destroy(conn);
        posix_tls_context_destroy(ctx);
        return 2;
    }

    // Stream file payload
    char chunk[16384];
    unsigned long long sent_bytes = 0;
    bool aborted = false;

    while (sent_bytes < file_size) {
        size_t to_read = sizeof(chunk);
        if (file_size - sent_bytes < static_cast<unsigned long long>(to_read)) {
            to_read = static_cast<size_t>(file_size - sent_bytes);
        }
        file_in.read(chunk, to_read);
        std::streamsize bytes_read = file_in.gcount();
        if (bytes_read <= 0) break;

        rc = write_all_tls(conn, sock_fd, reinterpret_cast<const unsigned char*>(chunk), static_cast<size_t>(bytes_read), timeout_ms);
        if (rc != POSIX_TLS_OK) {
            std::cerr << "Error transmitting payload data: " << posix_tls_strerror(rc) << std::endl;
            aborted = true;
            break;
        }
        sent_bytes += bytes_read;

        if (disconnect_after > 0 && sent_bytes >= disconnect_after) {
            std::cout << "Test hook triggered: intentionally disconnecting client after " << sent_bytes << " bytes" << std::endl;
            aborted = true;
            break;
        }
    }
    file_in.close();

    if (aborted) {
        posix_tls_connection_destroy(conn);
        posix_tls_context_destroy(ctx);
        return 4; // Transfer aborted / disconnected
    }

    // Read acknowledgment response from server under strict timeout budget
    std::string ack_str;
    double ack_deadline = get_time_ms() + static_cast<double>(timeout_ms);
    bool ack_complete = false;

    while (get_time_ms() < ack_deadline && ack_str.size() < 256) {
        char ack_chunk[64];
        size_t n = 0;
        rc = posix_tls_read(conn, ack_chunk, sizeof(ack_chunk), &n);
        if (rc == POSIX_TLS_OK) {
            if (n == 0) break;
            ack_str.append(ack_chunk, n);
            if (ack_str.find('\n') != std::string::npos) {
                ack_complete = true;
                break;
            }
        } else if (rc == POSIX_TLS_ERR_WANT_READ || rc == POSIX_TLS_ERR_WANT_WRITE) {
            int rem = static_cast<int>(ack_deadline - get_time_ms());
            if (rem <= 0) break;
            wait_fd(sock_fd, rc == POSIX_TLS_ERR_WANT_WRITE, rem > 50 ? 50 : rem);
        } else {
            break;
        }
    }

    if (!ack_complete) {
        std::cerr << "Timeout or incomplete ACK received from server: " << ack_str << std::endl;
        posix_tls_shutdown(conn);
        posix_tls_connection_destroy(conn);
        posix_tls_context_destroy(ctx);
        return 3;
    }

    double t_transfer_end = get_time_ms();
    double transfer_duration_ms = t_transfer_end - t_transfer_start;
    double throughput_mb_s = (transfer_duration_ms > 0.0) ?
        (static_cast<double>(sent_bytes) / (1024.0 * 1024.0)) / (transfer_duration_ms / 1000.0) : 0.0;

    // Validate ACK structure and exact digest match
    std::string clean_ack = ack_str;
    while (!clean_ack.empty() && (clean_ack[clean_ack.size() - 1] == '\n' || clean_ack[clean_ack.size() - 1] == '\r')) {
        clean_ack.erase(clean_ack.size() - 1);
    }

    std::string expected_ack_prefix = "OK: SHA256 MATCH ";
    if (clean_ack.compare(0, expected_ack_prefix.size(), expected_ack_prefix) != 0) {
        std::cerr << "Server reported error or invalid acknowledgment: " << clean_ack << std::endl;
        posix_tls_shutdown(conn);
        posix_tls_connection_destroy(conn);
        posix_tls_context_destroy(ctx);
        return 3;
    }

    std::string confirmed_digest = clean_ack.substr(expected_ack_prefix.size());
    if (confirmed_digest != file_sha256) {
        std::cerr << "ACK digest mismatch! Expected " << file_sha256 << " but server confirmed " << confirmed_digest << std::endl;
        posix_tls_shutdown(conn);
        posix_tls_connection_destroy(conn);
        posix_tls_context_destroy(ctx);
        return 3;
    }

    std::cout << "TRANSFER_OK bytes=" << sent_bytes
              << " duration_ms=" << std::fixed << std::setprecision(2) << transfer_duration_ms
              << " throughput_mb_s=" << std::fixed << std::setprecision(2) << throughput_mb_s
              << " ack=\"" << clean_ack << "\"" << std::endl;

    posix_tls_shutdown(conn);
    posix_tls_connection_destroy(conn);
    posix_tls_context_destroy(ctx);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);

    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    std::string command = argv[1];
    if (command == "generate-identity") {
        return cmd_generate_identity(argc, argv);
    } else if (command == "fingerprint") {
        return cmd_fingerprint(argc, argv);
    } else if (command == "server") {
        return cmd_server(argc, argv);
    } else if (command == "client") {
        return cmd_client(argc, argv);
    } else if (command == "--help" || command == "-h" || command == "help") {
        print_usage(argv[0]);
        return 0;
    } else {
        std::cerr << "Unknown command: " << command << std::endl;
        print_usage(argv[0]);
        return 1;
    }
}
