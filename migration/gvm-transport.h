/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef GVM_TRANSPORT_H
#define GVM_TRANSPORT_H
/* Selected once by -local-cpu method=tcp|rdma, shared by all GVM channels. */
extern bool gvm_transport_tcp;
/* Set only for explicit cluster shutdown/exit, never on a network error. */
extern bool gvm_transport_stopping;
const char *gvm_transport_method(void);
int gvm_tcp_listen(const char *host, int port);
int gvm_tcp_connect(const char *host, int port);
int gvm_tcp_accept(int listener);
int gvm_tcp_send_frame(int fd, const void *data, size_t length, unsigned timeout);
int gvm_tcp_recv_frame(int fd, void *data, size_t capacity, unsigned timeout);
#endif
