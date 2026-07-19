#include "ipc.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

static void close_fd_if_valid(int *fd) {
    if (fd && *fd >= 0) {
        close(*fd);
        *fd = INVALID_FD;
    }
}

static int is_kept_fd(int fd, const int *keep, int keep_count) {
    if (fd < 0) return 0;
    for (int i = 0; i < keep_count; i++) {
        if (keep[i] == fd) return 1;
    }
    return 0;
}

int set_fd_cloexec(int fd) {
    int flags = fcntl(fd, F_GETFD, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

int set_fd_nonblocking(int fd, int nonblocking) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    if (nonblocking) flags |= O_NONBLOCK;
    else flags &= ~O_NONBLOCK;
    return fcntl(fd, F_SETFL, flags);
}

void ipc_init(AllIPC *ipc) {
    for (int t = 0; t < TEAM_COUNT; t++) {
        for (int i = 0; i < MAX_MEMBERS - 1; i++) {
            ipc->teams[t].forward[i][0] = INVALID_FD;
            ipc->teams[t].forward[i][1] = INVALID_FD;
            ipc->teams[t].backward[i][0] = INVALID_FD;
            ipc->teams[t].backward[i][1] = INVALID_FD;
        }
        ipc->teams[t].ack[0] = INVALID_FD;
        ipc->teams[t].ack[1] = INVALID_FD;
    }
    ipc->event_pipe[0] = INVALID_FD;
    ipc->event_pipe[1] = INVALID_FD;
}

static int make_pipe_pair(int p[2]) {
    if (pipe(p) == -1) return -1;
    set_fd_cloexec(p[0]);
    set_fd_cloexec(p[1]);
    return 0;
}

int ipc_create_all(AllIPC *ipc, const Config *cfg) {
    ipc_init(ipc);
    if (make_pipe_pair(ipc->event_pipe) != 0) return -1;

    for (int t = 0; t < TEAM_COUNT; t++) {
        for (int i = 0; i < cfg->members_per_team - 1; i++) {
            if (make_pipe_pair(ipc->teams[t].forward[i]) != 0) return -1;
            if (make_pipe_pair(ipc->teams[t].backward[i]) != 0) return -1;
        }
        if (make_pipe_pair(ipc->teams[t].ack) != 0) return -1;
    }
    return 0;
}

void ipc_close_all(AllIPC *ipc, const Config *cfg) {
    for (int t = 0; t < TEAM_COUNT; t++) {
        for (int i = 0; i < cfg->members_per_team - 1; i++) {
            close_fd_if_valid(&ipc->teams[t].forward[i][0]);
            close_fd_if_valid(&ipc->teams[t].forward[i][1]);
            close_fd_if_valid(&ipc->teams[t].backward[i][0]);
            close_fd_if_valid(&ipc->teams[t].backward[i][1]);
        }
        close_fd_if_valid(&ipc->teams[t].ack[0]);
        close_fd_if_valid(&ipc->teams[t].ack[1]);
    }
    close_fd_if_valid(&ipc->event_pipe[0]);
    close_fd_if_valid(&ipc->event_pipe[1]);
}

void ipc_close_master_unused(AllIPC *ipc, const Config *cfg) {
    for (int t = 0; t < TEAM_COUNT; t++) {
        for (int i = 0; i < cfg->members_per_team - 1; i++) {
            close_fd_if_valid(&ipc->teams[t].forward[i][0]);
            close_fd_if_valid(&ipc->teams[t].forward[i][1]);
            close_fd_if_valid(&ipc->teams[t].backward[i][0]);
            close_fd_if_valid(&ipc->teams[t].backward[i][1]);
        }
        close_fd_if_valid(&ipc->teams[t].ack[0]);
        close_fd_if_valid(&ipc->teams[t].ack[1]);
    }
    close_fd_if_valid(&ipc->event_pipe[1]);
}

static void close_all_not_kept(AllIPC *ipc, const Config *cfg, const int *keep, int keep_count) {
    for (int t = 0; t < TEAM_COUNT; t++) {
        for (int i = 0; i < cfg->members_per_team - 1; i++) {
            if (!is_kept_fd(ipc->teams[t].forward[i][0], keep, keep_count)) close_fd_if_valid(&ipc->teams[t].forward[i][0]);
            if (!is_kept_fd(ipc->teams[t].forward[i][1], keep, keep_count)) close_fd_if_valid(&ipc->teams[t].forward[i][1]);
            if (!is_kept_fd(ipc->teams[t].backward[i][0], keep, keep_count)) close_fd_if_valid(&ipc->teams[t].backward[i][0]);
            if (!is_kept_fd(ipc->teams[t].backward[i][1], keep, keep_count)) close_fd_if_valid(&ipc->teams[t].backward[i][1]);
        }
        if (!is_kept_fd(ipc->teams[t].ack[0], keep, keep_count)) close_fd_if_valid(&ipc->teams[t].ack[0]);
        if (!is_kept_fd(ipc->teams[t].ack[1], keep, keep_count)) close_fd_if_valid(&ipc->teams[t].ack[1]);
    }
    if (!is_kept_fd(ipc->event_pipe[0], keep, keep_count)) close_fd_if_valid(&ipc->event_pipe[0]);
    if (!is_kept_fd(ipc->event_pipe[1], keep, keep_count)) close_fd_if_valid(&ipc->event_pipe[1]);
}

void ipc_close_child_unused(AllIPC *ipc, const Config *cfg, int team_id, int member_id) {
    int keep[16];
    int n = 0;
    TeamIPC *team = &ipc->teams[team_id];
    int last = cfg->members_per_team - 1;

    keep[n++] = ipc->event_pipe[1];

    if (member_id == 0) {
        keep[n++] = team->forward[0][1];
        keep[n++] = team->backward[0][0];
        keep[n++] = team->ack[0];
    } else if (member_id == last) {
        keep[n++] = team->forward[last - 1][0];
        keep[n++] = team->backward[last - 1][1];
        keep[n++] = team->ack[1];
    } else {
        keep[n++] = team->forward[member_id - 1][0];
        keep[n++] = team->forward[member_id][1];
        keep[n++] = team->backward[member_id][0];
        keep[n++] = team->backward[member_id - 1][1];
    }

    close_all_not_kept(ipc, cfg, keep, n);
}

ssize_t write_full(int fd, const void *buf, size_t count) {
    const char *p = (const char *)buf;
    size_t left = count;
    while (left > 0) {
        ssize_t n = write(fd, p, left);
        if (n == -1) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        p += n;
        left -= (size_t)n;
    }
    return (ssize_t)count;
}

ssize_t read_full(int fd, void *buf, size_t count) {
    char *p = (char *)buf;
    size_t left = count;
    while (left > 0) {
        ssize_t n = read(fd, p, left);
        if (n == -1) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return (ssize_t)(count - left);
            return -1;
        }
        if (n == 0) {
            return (ssize_t)(count - left);
        }
        p += n;
        left -= (size_t)n;
    }
    return (ssize_t)count;
}

int write_furniture_msg(int fd, const FurnitureMsg *msg) {
    FurnitureMsg tmp = *msg;
    tmp.magic = FURNITURE_MSG_MAGIC;
    return write_full(fd, &tmp, sizeof(tmp)) == (ssize_t)sizeof(tmp) ? 0 : -1;
}

int read_furniture_msg(int fd, FurnitureMsg *msg) {
    ssize_t n = read_full(fd, msg, sizeof(*msg));
    if (n != (ssize_t)sizeof(*msg)) return -1;
    if (msg->magic != FURNITURE_MSG_MAGIC) return -1;
    return 0;
}

int write_event_msg(int fd, const EventMsg *msg) {
    EventMsg tmp = *msg;
    tmp.magic = EVENT_MSG_MAGIC;
    tmp.timestamp_ms = now_ms();
    return write_full(fd, &tmp, sizeof(tmp)) == (ssize_t)sizeof(tmp) ? 0 : -1;
}

int read_event_msg(int fd, EventMsg *msg) {
    ssize_t n = read_full(fd, msg, sizeof(*msg));
    if (n != (ssize_t)sizeof(*msg)) return -1;
    if (msg->magic != EVENT_MSG_MAGIC) return -1;
    return 0;
}

void drain_fd(int fd) {
    if (fd < 0) return;
    int old_flags = fcntl(fd, F_GETFL, 0);
    if (old_flags == -1) return;
    fcntl(fd, F_SETFL, old_flags | O_NONBLOCK);
    char tmp[512];
    while (1) {
        ssize_t n = read(fd, tmp, sizeof(tmp));
        if (n > 0) continue;
        if (n == -1 && errno == EINTR) continue;
        break;
    }
    fcntl(fd, F_SETFL, old_flags);
}

FurnitureMsg make_furniture_msg(int team_id, int round_id, int piece_id, int serial_no, MessageDirection direction) {
    FurnitureMsg msg;
    memset(&msg, 0, sizeof(msg));
    msg.magic = FURNITURE_MSG_MAGIC;
    msg.kind = MSG_FURNITURE;
    msg.direction = direction;
    msg.team_id = team_id;
    msg.round_id = round_id;
    msg.piece_id = piece_id;
    msg.serial_no = serial_no;
    msg.touches = 0;
    msg.last_pid = getpid();
    return msg;
}

FurnitureMsg make_ack_msg(int team_id, int round_id, int piece_id, int serial_no) {
    FurnitureMsg msg;
    memset(&msg, 0, sizeof(msg));
    msg.magic = FURNITURE_MSG_MAGIC;
    msg.kind = MSG_ACK;
    msg.direction = DIR_NONE;
    msg.team_id = team_id;
    msg.round_id = round_id;
    msg.piece_id = piece_id;
    msg.serial_no = serial_no;
    msg.last_pid = getpid();
    return msg;
}

EventMsg make_event(EventType type, int team_id, int member_id, int round_id, const char *text) {
    EventMsg ev;
    memset(&ev, 0, sizeof(ev));
    ev.magic = EVENT_MSG_MAGIC;
    ev.type = type;
    ev.team_id = team_id;
    ev.member_id = member_id;
    ev.round_id = round_id;
    ev.piece_id = -1;
    ev.serial_no = -1;
    ev.accepted_count = 0;
    ev.furniture_count = 0;
    ev.expected_serial = 0;
    ev.wins[0] = 0;
    ev.wins[1] = 0;
    ev.member_count = 0;
    ev.direction = DIR_NONE;
    ev.from_member = member_id;
    ev.to_member = member_id;
    ev.touches = 0;
    ev.timestamp_ms = now_ms();
    if (text) snprintf(ev.text, sizeof(ev.text), "%s", text);
    return ev;
}
