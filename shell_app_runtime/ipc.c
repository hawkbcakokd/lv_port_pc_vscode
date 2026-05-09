#include "ipc.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int ipc_send_msg(int sock, const runtime_msg_t *msg)
{
    ssize_t n = send(sock, msg, sizeof(*msg), 0);
    return n == (ssize_t)sizeof(*msg) ? 0 : -1;
}

int ipc_recv_msg(int sock, runtime_msg_t *msg)
{
    ssize_t n = recv(sock, msg, sizeof(*msg), MSG_WAITALL);
    if (n == 0) return 1;
    return n == (ssize_t)sizeof(*msg) ? 0 : -1;
}

int ipc_send_msg_with_fd(int sock, const runtime_msg_t *msg, int fd_to_send)
{
    struct msghdr msgh;
    struct iovec iov;
    char buf[CMSG_SPACE(sizeof(int))];
    memset(&msgh, 0, sizeof(msgh));
    memset(buf, 0, sizeof(buf));

    iov.iov_base = (void *)msg;
    iov.iov_len = sizeof(*msg);
    msgh.msg_iov = &iov;
    msgh.msg_iovlen = 1;
    msgh.msg_control = buf;
    msgh.msg_controllen = sizeof(buf);

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msgh);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd_to_send, sizeof(int));

    ssize_t n = sendmsg(sock, &msgh, 0);
    return n == (ssize_t)sizeof(*msg) ? 0 : -1;
}

int ipc_recv_msg_with_fd(int sock, runtime_msg_t *msg, int *received_fd)
{
    struct msghdr msgh;
    struct iovec iov;
    char buf[CMSG_SPACE(sizeof(int))];
    memset(&msgh, 0, sizeof(msgh));
    memset(buf, 0, sizeof(buf));
    *received_fd = -1;

    iov.iov_base = msg;
    iov.iov_len = sizeof(*msg);
    msgh.msg_iov = &iov;
    msgh.msg_iovlen = 1;
    msgh.msg_control = buf;
    msgh.msg_controllen = sizeof(buf);

    ssize_t n = recvmsg(sock, &msgh, 0);
    if (n == 0) return 1;
    if (n != (ssize_t)sizeof(*msg)) return -1;

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msgh);
    if (cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
        memcpy(received_fd, CMSG_DATA(cmsg), sizeof(int));
    }
    return 0;
}

