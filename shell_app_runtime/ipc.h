#ifndef SHELL_APP_RUNTIME_IPC_H
#define SHELL_APP_RUNTIME_IPC_H

#include "protocol.h"

int ipc_send_msg(int sock, const runtime_msg_t *msg);
int ipc_recv_msg(int sock, runtime_msg_t *msg);

int ipc_send_msg_with_fd(int sock, const runtime_msg_t *msg, int fd_to_send);
int ipc_recv_msg_with_fd(int sock, runtime_msg_t *msg, int *received_fd);

#endif

