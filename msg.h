#pragma once

enum {
    LOG_ROLE_PUB,
    LOG_ROLE_SUB,
};

typedef struct initLogRequest {
    int len;
    int pid: 28;
    int role: 4;
    char tag[];
} initLogRequest;

typedef struct initLogResponse {
    int status;
} initLogResponse;

typedef struct logMessage {
    int len;
    unsigned sec;
    unsigned us;
    int tid: 28;
    int level: 4;
    char data[];
} msgLog;
