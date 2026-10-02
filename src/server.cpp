#include "server.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "http.hpp"

namespace {

struct FdGuard {
    explicit FdGuard(int fd) : fd(fd) {}
    ~FdGuard() {
        if (fd >= 0) ::close(fd);
    }
    FdGuard(const FdGuard&) = delete;
    FdGuard& operator=(const FdGuard&) = delete;
    int fd;
};

std::string peer_address(int fd) {
    sockaddr_in addr{};
    socklen_t len = sizeof addr;
    if (getpeername(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        return "?";
    }
    char buf[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof buf) == nullptr) {
        return "?";
    }
    return std::string(buf);
}

}  // namespace

Server::Server(int port, std::string docroot_real, std::size_t num_threads)
    : port_(port), docroot_(std::move(docroot_real)), pool_(num_threads), listen_fd_(-1) {
    pipe_fds_[0] = -1;
    pipe_fds_[1] = -1;
    if (pipe(pipe_fds_) != 0) {
        std::cerr << "error: pipe() failed: " << std::strerror(errno) << "\n";
    }
}

Server::~Server() {
    if (pipe_fds_[0] >= 0) ::close(pipe_fds_[0]);
    if (pipe_fds_[1] >= 0) ::close(pipe_fds_[1]);
}

void Server::stop() {
    stop_.store(true);
    // Wake the select() in run(). write() is async-signal-safe.
    // Best effort: if it fails, the stop_ flag alone still ends the loop.
    const char b = 'x';
    if (pipe_fds_[1] >= 0 && ::write(pipe_fds_[1], &b, 1) < 0) {
        // nothing more we can safely do in a signal handler
    }
}

int Server::run() {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        std::cerr << "error: socket() failed: " << std::strerror(errno) << "\n";
        return 1;
    }
    FdGuard listen_guard(listen_fd_);

    int opt = 1;
    if (::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt) != 0) {
        std::cerr << "error: setsockopt() failed: " << std::strerror(errno) << "\n";
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port_));
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        std::cerr << "error: bind() to port " << port_
                  << " failed: " << std::strerror(errno) << "\n";
        return 1;
    }
    if (::listen(listen_fd_, 128) != 0) {
        std::cerr << "error: listen() failed: " << std::strerror(errno) << "\n";
        return 1;
    }

    // Accept loop. select() waits on the listening socket and the self-pipe
    // so a signal can wake it without any async-signal-safety hazards.
    while (!stop_.load()) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listen_fd_, &rfds);
        int maxfd = listen_fd_;
        bool have_pipe = pipe_fds_[0] >= 0;
        if (have_pipe) {
            FD_SET(pipe_fds_[0], &rfds);
            if (pipe_fds_[0] > maxfd) maxfd = pipe_fds_[0];
        }

        int ready = ::select(maxfd + 1, &rfds, nullptr, nullptr, nullptr);
        if (ready < 0) {
            if (errno == EINTR) continue;
            std::cerr << "error: select() failed: " << std::strerror(errno) << "\n";
            break;
        }
        if (have_pipe && FD_ISSET(pipe_fds_[0], &rfds)) {
            break;  // stop() was called
        }
        if (FD_ISSET(listen_fd_, &rfds)) {
            int conn = ::accept(listen_fd_, nullptr, nullptr);
            if (conn < 0) {
                if (errno == EINTR) continue;
                std::cerr << "warning: accept() failed: " << std::strerror(errno) << "\n";
                continue;
            }
            pool_.enqueue([this, conn] { handle_connection(conn); });
        }
    }

    // Graceful shutdown: the listening socket closes here (FdGuard),
    // the pool drains queued connections, then every worker joins.
    pool_.shutdown();
    return 0;
}

void Server::handle_connection(int fd) {
    FdGuard guard(fd);
    auto start = std::chrono::steady_clock::now();

    std::string peer = peer_address(fd);
    std::string method = "-";
    std::string target = "-";
    HttpResponse res;
    bool head_only = false;

    std::string head;
    int err = 0;
    if (!read_request_head(fd, head, err)) {
        res = make_error_response(err);
    } else {
        HttpRequest req;
        if (!parse_request(head, req, err)) {
            res = make_error_response(err);
        } else {
            method = req.method;
            target = req.target;
            head_only = (req.method == "HEAD");
            res = handle_request(req, docroot_);
        }
    }

    std::string payload = serialize_response(res, head_only);
    (void)send_all(fd, payload.data(), payload.size());

    long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count();
    log_access(peer, method, target, res.status, payload.size(), ms);
}
