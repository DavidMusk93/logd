#define _GNU_SOURCE

#include <unistd.h>
#include <poll.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <netdb.h>
#include <sys/un.h>

#include "macro.h"
#include "log.h"
#include "pubsub.h"
#include "misc.h"

static const char *logLevel2String(int level) {
#define __case(x) case LOG_LEVEL_##x:return #x
    switch (level) {
        __case(DEBUG);
        __case(INFO);
        __case(WARN);
        __case(ERROR);
        default:
            return "UNKNOWN";
    }
#undef __case
}

int specificRead(int fd, char *buf, int len) {
    int rc, retry;
    struct pollfd pfd = {.fd=fd, .events=POLLIN};
    for (retry = 0; len > 0 && retry < LOGRECVMAXRETRY; ++retry) {
        _poll(rc, poll, &pfd, 1, LOGMSGRECVTO);
        if (rc == 0) continue;
        rc = (int) read(fd, buf, len);
        if (rc == 0) break;
        if (rc == -1 && (errno == EAGAIN || errno == EINTR)) continue;
        buf += rc;
        len -= rc;
    }
    return len == 0;
}

int limitRead(int fd, int hdrlen, char *buf, int buflen) {
    int left = 0, datalen;
    int *lenlink;
    if (!specificRead(fd, buf, hdrlen)) return 0;
    datalen = *(int *) buf;
    lenlink = (int *) buf;
    if (datalen == 0) return 1;
    buf += hdrlen;
    buflen -= hdrlen;
    if (!specificRead(fd, buf, datalen < buflen ? datalen : buflen)) return 0;
    /* Truncate message if 'datalen' greater than 'buflen'-'hdrlen'. */
    if (datalen > buflen) {
        *lenlink = buflen;
        left = datalen - buflen;
        char a[512];
        do {
            if (!specificRead(fd, a, left < sizeof(a) ? left : (int) sizeof(a))) break;
            if (left <= sizeof(a)) left = 0;
            else left -= sizeof(a);
        } while (left);
    }
    return left == 0;
}

enum {
    DAEMON_OK,
    DAEMON_ERROR_SERVER,
    DAEMON_ERROR_ACCEPT,
    DAEMON_ERROR_INIT,
};

#define DAEMON_MAX_EVENTS 1024
#define LOG_FMT "%u.%06u %d#%d %.*s %s %.*s"

_main() {
    signal(SIGPIPE, SIG_IGN);
    int i, n, rc = -1;
    const int subscriberSendBufSize = 16 * 1024 * 1024;
    autoFd(serverfd);
    autoFd(epollfd);
    struct epoll_event events[DAEMON_MAX_EVENTS], ev;
    peerManager pm;
    _attr(aligned(8)) char buf[LOGBUFLEN];
    char msgbuf[LOGMSGLEN];
    msgReqInit *reqinit = (msgReqInit *) buf;
    msgLog *log = (msgLog *) buf;
    pubEntry se;
    const char *logFmt;

    unixStreamServer(rc, serverfd, LOGIPC);
    if (rc == -1) return DAEMON_ERROR_SERVER;
    se.fd = serverfd;
    epollfd = epoll_create1(0);
    memset(&ev, 0, sizeof(ev));
    ev.data.ptr = &se;
    ev.events = EPOLLIN;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, serverfd, &ev);
    pmInit(&pm);

    for (;;) {
        _poll(rc, epoll_wait, epollfd, events, DAEMON_MAX_EVENTS, -1);
        for (i = 0; i < rc; i++) {
            int fd = ((pubEntry *) events[i].data.ptr)->fd;
            if (fd == serverfd) {
                autoFd(peerFd);
                peerFd = accept4(fd, 0, 0, O_NONBLOCK);
                if (peerFd == -1) return DAEMON_ERROR_ACCEPT;
                if (!limitRead(peerFd, sizeof(msgReqInit), buf, LOGBUFLEN)) {
                    log2("Error occurs on peer init");
                    continue;
                }
                if (reqinit->role == LOG_ROLE_PUB) {
                    ev.data.ptr = pmNewPub(&pm, peerFd, reqinit);
                    epoll_ctl(epollfd, EPOLL_CTL_ADD, peerFd, &ev);
                    log1("New publisher #%d", peerFd);
                } else if (reqinit->role == LOG_ROLE_SUB) {
                    pmNewSub(&pm, peerFd);
                    log1("New subscriber #%d", peerFd);
                    setsockopt(peerFd, SOL_SOCKET, SO_SNDBUF, &subscriberSendBufSize, sizeof(subscriberSendBufSize));
                }
                msgResInit resinit = {0};
                write(peerFd, &resinit, sizeof resinit);
                peerFd = -1;
            } else {
                pubEntry *entry = events[i].data.ptr;
                if (events[i].events & (EPOLLERR | EPOLLHUP) || !limitRead(fd, sizeof(msgLog), buf, LOGBUFLEN)) {
                    pmFreePub(&pm, entry);
                    log1("Publisher #%d leave", fd);
                } else {
                    logFmt = log->data[log->len - 1] == '\n' ? LOG_FMT : LOG_FMT "\n";
                    n = sprintf(msgbuf, logFmt,
                                log->sec, log->us,
                                entry->pid, log->tid,
                                entry->len, entry->tag,
                                logLevel2String(log->level),
                                log->len, log->data);
                    pmPost(&pm, msgbuf, n);
                }
            }
        }
    }
    pmFree(&pm);
    return 0;
}