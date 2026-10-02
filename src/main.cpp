// httpd — a multithreaded static file server in C++17.
//
// Usage:
//   httpd [--port N] [--docroot PATH] [--threads N]
//
// Only POSIX sockets and the C++ standard library are used.

#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <thread>

#include "server.hpp"

namespace {

struct Config {
    int port = 8080;
    std::string docroot = "./public";
    std::size_t threads = 0;  // 0 = hardware concurrency
};

void usage(const char* prog) {
    std::cout << "Usage: " << prog << " [--port N] [--docroot PATH] [--threads N]\n"
              << "\n"
              << "  --port N       TCP port to listen on (1-65535, default 8080)\n"
              << "  --docroot PATH Directory to serve files from (default ./public)\n"
              << "  --threads N    Worker thread pool size, 1-128\n"
              << "                 (default: hardware concurrency)\n"
              << "  --help, -h     Show this message\n";
}

bool parse_int(const char* s, long& out) {
    char* end = nullptr;
    errno = 0;
    long v = std::strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') return false;
    out = v;
    return true;
}

bool parse_args(int argc, char** argv, Config& cfg) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            std::exit(0);
        }
        auto need_value = [&](const char* name, std::string& out) -> bool {
            if (i + 1 >= argc) {
                std::cerr << "error: " << name << " requires a value\n";
                return false;
            }
            out = argv[++i];
            return true;
        };
        std::string val;
        if (arg == "--port") {
            if (!need_value("--port", val)) return false;
            long v = 0;
            if (!parse_int(val.c_str(), v) || v < 1 || v > 65535) {
                std::cerr << "error: --port must be 1-65535\n";
                return false;
            }
            cfg.port = static_cast<int>(v);
        } else if (arg == "--docroot") {
            if (!need_value("--docroot", val)) return false;
            cfg.docroot = val;
        } else if (arg == "--threads") {
            if (!need_value("--threads", val)) return false;
            long v = 0;
            if (!parse_int(val.c_str(), v) || v < 1 || v > 128) {
                std::cerr << "error: --threads must be 1-128\n";
                return false;
            }
            cfg.threads = static_cast<std::size_t>(v);
        } else {
            std::cerr << "error: unknown argument '" << arg << "'\n";
            usage(argv[0]);
            return false;
        }
    }
    return true;
}

Server* g_server = nullptr;  // set before run(), cleared after

void on_shutdown_signal(int /*sig*/) {
    if (g_server) g_server->stop();  // async-signal-safe
}

}  // namespace

int main(int argc, char** argv) {
    Config cfg;
    if (!parse_args(argc, argv, cfg)) {
        return 2;
    }
    if (cfg.threads == 0) {
        unsigned hc = std::thread::hardware_concurrency();
        cfg.threads = (hc == 0) ? 4 : hc;
    }

    // Validate the docroot and canonicalize it for the security checks.
    struct stat st{};
    if (stat(cfg.docroot.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        std::cerr << "error: docroot '" << cfg.docroot
                  << "' does not exist or is not a directory\n";
        return 2;
    }
    char* rp = realpath(cfg.docroot.c_str(), nullptr);
    if (rp == nullptr) {
        std::cerr << "error: cannot resolve docroot '" << cfg.docroot << "'\n";
        return 2;
    }
    std::string docroot_real(rp);
    free(rp);

    // A dropped client must never kill the server.
    std::signal(SIGPIPE, SIG_IGN);

    struct sigaction sa{};
    sa.sa_handler = on_shutdown_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    Server server(cfg.port, docroot_real, cfg.threads);
    g_server = &server;

    std::cout << "httpd serving " << docroot_real << " on port " << cfg.port
              << " with " << cfg.threads << " worker threads" << std::endl;

    int rc = server.run();

    g_server = nullptr;
    std::cout << "httpd shut down cleanly" << std::endl;
    return rc;
}
