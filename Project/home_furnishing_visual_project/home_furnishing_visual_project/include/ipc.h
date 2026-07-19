#ifndef IPC_H
#define IPC_H

#include "config.h"
#include <poll.h>

/*
 * Pipe message used for actual furniture transfer between child processes.
 */
typedef enum MessageKind {
    MSG_NONE = 0,
    MSG_FURNITURE = 1,
    MSG_ACK = 2
} MessageKind;

typedef enum MessageDirection {
    DIR_NONE = 0,
    DIR_FORWARD = 1,
    DIR_BACKWARD = 2
} MessageDirection;

typedef struct FurnitureMsg {
    uint32_t magic;
    int kind;
    int direction;
    int team_id;
    int round_id;
    int piece_id;
    int serial_no;
    int touches;
    pid_t last_pid;
} FurnitureMsg;

/*
 * Event messages are reports from child processes to the master process.
 * The master prints/logs them and optionally mirrors them to the OpenGL FIFO.
 */
typedef enum EventType {
    EVENT_NONE = 0,
    EVENT_LOG = 1,
    EVENT_ACCEPTED = 2,
    EVENT_REJECTED = 3,
    EVENT_ROUND_WIN = 4,
    EVENT_CHILD_READY = 5,
    EVENT_CHILD_EXIT = 6,
    EVENT_ERROR = 7,
    EVENT_MOVED = 8,         /* animation event: product moved from one member to another */
    EVENT_ROUND_START = 9,   /* master event: a new round is starting, reset visual progress */
    EVENT_TIE_BREAK = 10,    /* master event: regular rounds tied, starting extra judgment round */
    EVENT_COMPETITION_END = 11, /* master event: competition finished */
    EVENT_VISUAL_RESULT = 12,   /* source event: one completed attempt for visual animation */
    EVENT_ROUND_RESULT = 13     /* master event: round finished, show score during pause */
} EventType;

typedef struct EventMsg {
    uint32_t magic;
    int type;
    int team_id;
    int member_id;
    int round_id;
    int piece_id;
    int serial_no;
    int accepted_count;
    int furniture_count;
    int expected_serial;
    int wins[TEAM_COUNT];

    /* Visualization metadata. These fields are also useful for debugging. */
    int member_count;
    int direction;
    int from_member;
    int to_member;
    int touches;

    long timestamp_ms;
    char text[MAX_TEXT];
} EventMsg;

typedef struct TeamIPC {
    int forward[MAX_MEMBERS - 1][2];  /* member i writes to member i+1 */
    int backward[MAX_MEMBERS - 1][2]; /* member i+1 writes to member i */
    int ack[2];                       /* sink writes, source reads */
} TeamIPC;

typedef struct AllIPC {
    TeamIPC teams[TEAM_COUNT];
    int event_pipe[2];                /* children write, master reads */
} AllIPC;

void ipc_init(AllIPC *ipc);
int ipc_create_all(AllIPC *ipc, const Config *cfg);
void ipc_close_all(AllIPC *ipc, const Config *cfg);
void ipc_close_master_unused(AllIPC *ipc, const Config *cfg);
void ipc_close_child_unused(AllIPC *ipc, const Config *cfg, int team_id, int member_id);

int set_fd_nonblocking(int fd, int nonblocking);
int set_fd_cloexec(int fd);
ssize_t write_full(int fd, const void *buf, size_t count);
ssize_t read_full(int fd, void *buf, size_t count);
int write_furniture_msg(int fd, const FurnitureMsg *msg);
int read_furniture_msg(int fd, FurnitureMsg *msg);
int write_event_msg(int fd, const EventMsg *msg);
int read_event_msg(int fd, EventMsg *msg);
void drain_fd(int fd);

FurnitureMsg make_furniture_msg(int team_id, int round_id, int piece_id, int serial_no, MessageDirection direction);
FurnitureMsg make_ack_msg(int team_id, int round_id, int piece_id, int serial_no);
EventMsg make_event(EventType type, int team_id, int member_id, int round_id, const char *text);

#endif
