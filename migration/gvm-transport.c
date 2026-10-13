/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "gvm-transport.h"
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <poll.h>

bool gvm_transport_tcp;

const char *gvm_transport_method(void)
{
    return gvm_transport_tcp ? "tcp" : "rdma";
}

static int64_t tcp_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int tcp_wait(int fd, short events, int64_t deadline)
{
    for (;;) {
        int64_t remaining = deadline - tcp_ms();
        struct pollfd p = { .fd = fd, .events = events };
        if (remaining <= 0) {
            return -ETIMEDOUT;
        }
        int ret = poll(&p, 1, MIN(remaining, INT_MAX));
        if (ret < 0 && errno == EINTR) {
            continue;
        }
        if (ret <= 0) {
            return ret ? -errno : -ETIMEDOUT;
        }
        if (p.revents & events) {
            return 0;
        }
        return -ECONNRESET;
    }
}

static int tcp_exact(int fd, void *buffer, size_t length, bool write,
                     int64_t deadline)
{
    uint8_t *p = buffer;
    while (length) {
        int ret = tcp_wait(fd, write ? POLLOUT : POLLIN, deadline);
        if (ret < 0) {
            return ret;
        }
        ssize_t n = write ? send(fd, p, length, MSG_NOSIGNAL | MSG_DONTWAIT) :
                            recv(fd, p, length, MSG_DONTWAIT);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
            continue;
        }
        if (n <= 0) {
            return n ? -errno : -ECONNRESET;
        }
        p += n;
        length -= n;
    }
    return 0;
}

static int tcp_options(int fd)
{
    int one = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) ||
        fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
        int ret = -errno;
        close(fd);
        return ret;
    }
    return fd;
}

int gvm_tcp_listen(const char *host, int port)
{
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(port) };
    int one = 1, fd;
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        return -EINVAL;
    }
    fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        return -errno;
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) || listen(fd, 16)) {
        int ret = -errno;
        close(fd);
        return ret;
    }
    return fd;
}

int gvm_tcp_connect(const char *host, int port)
{
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(port) };
    int64_t deadline = tcp_ms() + 30000;
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        return -EINVAL;
    }
    while (tcp_ms() < deadline) {
        int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        int err = 0;
        socklen_t len = sizeof(err);
        if (fd < 0) {
            return -errno;
        }
        int ret = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
        if (!ret) {
            return tcp_options(fd);
        }
        if (errno == EINPROGRESS &&
            !tcp_wait(fd, POLLOUT, MIN(deadline, tcp_ms() + 1000)) &&
            !getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) && !err) {
            return tcp_options(fd);
        }
        close(fd);
        usleep(100000);
    }
    return -ETIMEDOUT;
}

int gvm_tcp_accept(int listener)
{
    int64_t deadline = tcp_ms() + 30000;
    for (;;) {
        int ret = tcp_wait(listener, POLLIN, deadline);
        if (ret < 0) {
            return ret;
        }
        int fd = accept4(listener, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd >= 0) {
            return tcp_options(fd);
        }
        if (errno != EINTR && errno != EAGAIN) {
            return -errno;
        }
    }
}

int gvm_tcp_send_frame(int fd, const void *data, size_t length, unsigned timeout)
{
    uint32_t wire = htonl(length);
    int64_t deadline = tcp_ms() + timeout;
    if (length > UINT32_MAX) {
        return -EMSGSIZE;
    }
    int ret = tcp_exact(fd, &wire, sizeof(wire), true, deadline);
    if (!ret) {
        ret = tcp_exact(fd, (void *)data, length, true, deadline);
    }
    return ret;
}

int gvm_tcp_recv_frame(int fd, void *data, size_t capacity, unsigned timeout)
{
    uint32_t wire;
    int64_t deadline = tcp_ms() + timeout;
    int ret = tcp_exact(fd, &wire, sizeof(wire), false, deadline);
    if (ret < 0) {
        return ret;
    }
    unsigned length = ntohl(wire);
    if (!length || length > capacity) {
        return -EMSGSIZE;
    }
    ret = tcp_exact(fd, data, length, false, deadline);
    return ret < 0 ? ret : length;
}
