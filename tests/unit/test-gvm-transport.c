/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "migration/gvm-transport.h"
#include <arpa/inet.h>
#include <pthread.h>

typedef struct Fragment {
    int fd;
    const uint8_t *data;
    size_t size;
} Fragment;

static void *fragments(void *opaque)
{
    Fragment *f = opaque;
    for (size_t i = 0; i < f->size; i++) {
        g_assert_cmpint(send(f->fd, f->data + i, 1, MSG_NOSIGNAL), ==, 1);
        usleep(100);
    }
    return NULL;
}

static void test_fragmented(void)
{
    int fd[2];
    uint8_t wire[] = { 0, 0, 0, 4, 1, 2, 3, 4 };
    uint8_t result[4];
    pthread_t thread;
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, fd), ==, 0);
    Fragment f = { fd[0], wire, sizeof(wire) };
    g_assert_cmpint(pthread_create(&thread, NULL, fragments, &f), ==, 0);
    g_assert_cmpint(gvm_tcp_recv_frame(fd[1], result, sizeof(result), 1000), ==, 4);
    g_assert_cmpmem(result, 4, wire + 4, 4);
    pthread_join(thread, NULL);
    close(fd[0]); close(fd[1]);
}

static void test_two_frames(void)
{
    int fd[2];
    uint8_t input[17], output[32];
    memset(input, 0x9b, sizeof(input));
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, fd), ==, 0);
    for (int i = 0; i < 2; i++) {
        g_assert_cmpint(gvm_tcp_send_frame(fd[0], input, sizeof(input), 1000), ==, 0);
    }
    for (int i = 0; i < 2; i++) {
        g_assert_cmpint(gvm_tcp_recv_frame(fd[1], output, sizeof(output), 1000), ==, 17);
        g_assert_cmpmem(input, 17, output, 17);
    }
    close(fd[0]); close(fd[1]);
}

static void test_invalid(bool oversized)
{
    int fd[2];
    uint32_t wire = htonl(oversized ? 4097 : 0);
    uint8_t output[4096];
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, fd), ==, 0);
    g_assert_cmpint(send(fd[0], &wire, sizeof(wire), 0), ==, sizeof(wire));
    g_assert_cmpint(gvm_tcp_recv_frame(fd[1], output, sizeof(output), 1000),
                    ==, -EMSGSIZE);
    close(fd[0]); close(fd[1]);
}
static void test_oversized(void) { test_invalid(true); }
static void test_empty(void) { test_invalid(false); }

static void test_disconnect(void)
{
    int fd[2];
    uint8_t partial[] = { 0, 0, 0, 4, 1, 2 }, result[4];
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, fd), ==, 0);
    g_assert_cmpint(send(fd[0], partial, sizeof(partial), 0), ==, sizeof(partial));
    close(fd[0]);
    g_assert_cmpint(gvm_tcp_recv_frame(fd[1], result, sizeof(result), 1000),
                    ==, -ECONNRESET);
    close(fd[1]);
}

static void test_timeout(void)
{
    int fd[2];
    uint8_t partial[] = { 0, 0 }, result[4];
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, fd), ==, 0);
    g_assert_cmpint(send(fd[0], partial, sizeof(partial), 0), ==, sizeof(partial));
    g_assert_cmpint(gvm_tcp_recv_frame(fd[1], result, sizeof(result), 30),
                    ==, -ETIMEDOUT);
    close(fd[0]); close(fd[1]);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/gvm-tcp/fragmented", test_fragmented);
    g_test_add_func("/gvm-tcp/two-frames", test_two_frames);
    g_test_add_func("/gvm-tcp/oversized", test_oversized);
    g_test_add_func("/gvm-tcp/empty", test_empty);
    g_test_add_func("/gvm-tcp/disconnect", test_disconnect);
    g_test_add_func("/gvm-tcp/timeout", test_timeout);
    return g_test_run();
}
