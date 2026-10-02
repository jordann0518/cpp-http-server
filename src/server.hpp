#pragma once

#include <atomic>
#include <string>

#include "thread_pool.hpp"

// A multithreaded static file server.
//
// One thread runs the accept loop; accepted connections are handed to a
// fixed-size ThreadPool. stop() is async-signal-safe: it wakes the accept
// loop through a self-pipe. run() then stops accepting, lets the pool
// drain its queue, joins every worker, and returns.
class Server {
public:
    Server(int port, std::string docroot_real, std::size_t num_threads);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Blocks until stop() is called. Returns 0 on clean shutdown, 1 on
    // fatal setup errors (bad socket/bind/listen).
    int run();

    // Safe to call from a signal handler.
    void stop();

private:
    void handle_connection(int fd);

    int port_;
    std::string docroot_;
    ThreadPool pool_;
    int listen_fd_;
    int pipe_fds_[2];
    std::atomic<bool> stop_{false};
};
