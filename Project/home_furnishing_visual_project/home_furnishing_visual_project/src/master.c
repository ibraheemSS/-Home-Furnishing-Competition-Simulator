#include "master.h"
#include "ipc.h"
#include "signals.h"
#include "team.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>


/* Paths registered for cleanup at exit. Set when FIFOs are created. */
static char g_cleanup_main_fifo[MAX_PATH] = {0};
static char g_cleanup_ack_fifo[MAX_PATH]  = {0};

/* Called automatically at program exit (via atexit) — safety net to
 * make sure FIFO files don't linger in /tmp/ even if something goes
 * wrong before the normal cleanup at the end of run_master. */
static void cleanup_fifos_at_exit(void) {
    if (g_cleanup_main_fifo[0]) {
        unlink(g_cleanup_main_fifo);
        g_cleanup_main_fifo[0] = '\0';
    }
    if (g_cleanup_ack_fifo[0]) {
        unlink(g_cleanup_ack_fifo);
        g_cleanup_ack_fifo[0] = '\0';
    }
}




static void init_pids(pid_t pids[TEAM_COUNT][MAX_MEMBERS]) {
    for (int t = 0; t < TEAM_COUNT; t++) {
        for (int m = 0; m < MAX_MEMBERS; m++) pids[t][m] = -1;
    }
}

static void signal_children(pid_t pids[TEAM_COUNT][MAX_MEMBERS], const Config *cfg, int sig) {
    for (int t = 0; t < TEAM_COUNT; t++) {
        for (int m = 0; m < cfg->members_per_team; m++) {
            if (pids[t][m] > 0) kill(pids[t][m], sig);
        }
    }
}

static void wait_children(pid_t pids[TEAM_COUNT][MAX_MEMBERS], const Config *cfg) {
    for (int t = 0; t < TEAM_COUNT; t++) {
        for (int m = 0; m < cfg->members_per_team; m++) {
            if (pids[t][m] > 0) {
                int status = 0;
                while (waitpid(pids[t][m], &status, 0) == -1 && errno == EINTR) {
                    continue;
                }
                pids[t][m] = -1;
            }
        }
    }
}

static int open_visual_fifo(const Config *cfg) {
    if (!cfg->graphics_enabled) return -1;

    if (mkfifo(cfg->graphics_fifo, 0666) == -1 && errno != EEXIST) {
        fprintf(stderr, "Could not create FIFO '%s': %s\n", cfg->graphics_fifo, strerror(errno));
        return -1;
    }

    /* Remember this FIFO path so atexit handler removes it if we crash */
    snprintf(g_cleanup_main_fifo, sizeof(g_cleanup_main_fifo),
             "%s", cfg->graphics_fifo);

    int fd = open(cfg->graphics_fifo, O_RDWR | O_NONBLOCK);
    if (fd == -1) {
        fprintf(stderr, "Could not open FIFO '%s': %s\n", cfg->graphics_fifo, strerror(errno));
        return -1;
    }
    set_fd_cloexec(fd);
    return fd;
}

static void make_visual_ack_fifo_path(const Config *cfg, char *buf, size_t size) {
    snprintf(buf, size, "%s.ack", cfg->graphics_fifo);
}

static int open_visual_ack_fifo(const char *ack_fifo) {
    if (mkfifo(ack_fifo, 0666) == -1 && errno != EEXIST) {
        fprintf(stderr, "Could not create visual ACK FIFO '%s': %s\n", ack_fifo, strerror(errno));
        return -1;
    }

    snprintf(g_cleanup_ack_fifo, sizeof(g_cleanup_ack_fifo), "%s", ack_fifo);

    int fd = open(ack_fifo, O_RDWR | O_NONBLOCK);
    if (fd == -1) {
        fprintf(stderr, "Could not open visual ACK FIFO '%s': %s\n", ack_fifo, strerror(errno));
        return -1;
    }
    set_fd_cloexec(fd);
    return fd;
}

static pid_t launch_visualizer_child(const Config *cfg, const char *ack_fifo) {
    if (!cfg->graphics_enabled) return -1;

    pid_t pid = fork();
    if (pid == -1) {
        fprintf(stderr, "Could not fork visualizer child: %s\n", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        execlp(cfg->visualizer_path, cfg->visualizer_path, cfg->graphics_fifo, ack_fifo, (char *)NULL);
        fprintf(stderr, "Could not exec visualizer '%s': %s\n", cfg->visualizer_path, strerror(errno));
        _exit(127);
    }

    printf("Visualizer child started with PID %ld using FIFO %s.\n", (long)pid, cfg->graphics_fifo);
    fflush(stdout);
    return pid;
}

static void terminate_visualizer_child(pid_t visualizer_pid) {
    if (visualizer_pid <= 0) return;

    int status = 0;
    pid_t r = waitpid(visualizer_pid, &status, WNOHANG);
    if (r == 0) {
        kill(visualizer_pid, SIGTERM);
        while (waitpid(visualizer_pid, &status, 0) == -1 && errno == EINTR) {
            continue;
        }
    }
}

static void wait_visualizer_child(pid_t visualizer_pid) {
    if (visualizer_pid <= 0) return;

    int status = 0;
    printf("Visualizer is still open. Close the visualizer window or press q in it to finish.\n");
    fflush(stdout);
    while (waitpid(visualizer_pid, &status, 0) == -1 && errno == EINTR) {
        continue;
    }
}

static void forward_to_visualizer(int fifo_fd, const EventMsg *ev, const int wins[TEAM_COUNT]) {
    if (fifo_fd < 0) return;
    EventMsg copy = *ev;
    copy.wins[0] = wins[0];
    copy.wins[1] = wins[1];
    (void)write_full(fifo_fd, &copy, sizeof(copy));
}

static void reset_log_file(const Config *cfg) {
    if (!cfg->log_file[0]) return;
    ensure_directory_for_file(cfg->log_file);
    FILE *fp = fopen(cfg->log_file, "w");
    if (!fp) return;
    fprintf(fp, "# Home furnishing simulation log\n");
    fprintf(fp, "# timestamp_ms type team member round piece serial accepted furniture expected wins members dir from to touches text\n");
    fclose(fp);
}

static void log_event(const Config *cfg, const EventMsg *ev, const int wins[TEAM_COUNT]) {
    if (!cfg->log_file[0]) return;
    EventMsg copy = *ev;
    copy.wins[0] = wins[0];
    copy.wins[1] = wins[1];
    char line[512];
    event_to_log_line(&copy, line, sizeof(line));
    append_log_line(cfg->log_file, line);
}

static void print_event(const Config *cfg, const EventMsg *ev, const int wins[TEAM_COUNT]) {
    (void)wins;
    if (ev->type == EVENT_ACCEPTED) {
        printf("[round %d] team %d accepted serial %d (piece %d) -> %d/%d\n",
               ev->round_id, ev->team_id, ev->serial_no, ev->piece_id,
               ev->accepted_count, ev->furniture_count);
    } else if (ev->type == EVENT_REJECTED && cfg->verbose) {
        printf("[round %d] team %d rejected serial %d (expected %d)\n",
               ev->round_id, ev->team_id, ev->serial_no, ev->expected_serial);
    } else if (ev->type == EVENT_VISUAL_RESULT && cfg->verbose) {
        printf("[round %d] team %d visual result serial %d piece %d %s -> progress %d\n",
               ev->round_id, ev->team_id, ev->serial_no, ev->piece_id,
               ev->direction == DIR_FORWARD ? "accepted" : "rejected", ev->accepted_count);
    } else if (ev->type == EVENT_MOVED && cfg->verbose) {
        printf("[round %d] team %d product serial %d moved %d -> %d\n",
               ev->round_id, ev->team_id, ev->serial_no, ev->from_member, ev->to_member);
    } else if (ev->type == EVENT_ROUND_WIN) {
        printf("[round %d] >>> TEAM %d WON THE ROUND <<<\n", ev->round_id, ev->team_id);
    } else if (ev->type == EVENT_ROUND_START) {
        printf("%s\n", ev->text);
    } else if (ev->type == EVENT_TIE_BREAK) {
        printf("%s\n", ev->text);
    } else if (ev->type == EVENT_ROUND_RESULT) {
        printf("%s\n", ev->text);
    } else if (ev->type == EVENT_COMPETITION_END) {
        printf("%s\n", ev->text);
    } else if (ev->type == EVENT_ERROR) {
        fprintf(stderr, "[child error] team=%d member=%d round=%d: %s\n",
                ev->team_id, ev->member_id, ev->round_id, ev->text);
    } else if (ev->type == EVENT_CHILD_READY && cfg->verbose) {
        printf("child ready: team=%d member=%d role=%s\n", ev->team_id, ev->member_id,
               role_name(ev->member_id, cfg->members_per_team));
    }
    fflush(stdout);
}

static void emit_master_event(const Config *cfg, int fifo_fd, int wins[TEAM_COUNT],
                              EventType type, int round_id, const char *text) {
    EventMsg ev = make_event(type, -1, -1, round_id, text);
    ev.furniture_count = cfg->furniture_count;
    ev.member_count = cfg->members_per_team;
    ev.wins[0] = wins[0];
    ev.wins[1] = wins[1];
    log_event(cfg, &ev, wins);
    forward_to_visualizer(fifo_fd, &ev, wins);
    print_event(cfg, &ev, wins);
}

static int read_and_handle_event(int event_fd, const Config *cfg, int fifo_fd, int wins[TEAM_COUNT],
                                 int current_round_id, EventMsg *out) {
    EventMsg ev;
    if (read_event_msg(event_fd, &ev) != 0) return -1;
    ev.wins[0] = wins[0];
    ev.wins[1] = wins[1];

    if (!(current_round_id > 0 && ev.round_id > 0 && ev.round_id < current_round_id)) {
        log_event(cfg, &ev, wins);
        forward_to_visualizer(fifo_fd, &ev, wins);
        print_event(cfg, &ev, wins);
    }

    if (out) *out = ev;
    return 0;
}

static int wait_for_ready(int event_fd, const Config *cfg, int fifo_fd, int wins[TEAM_COUNT]) {
    int total = TEAM_COUNT * cfg->members_per_team;
    unsigned char seen[TEAM_COUNT][MAX_MEMBERS];
    memset(seen, 0, sizeof(seen));
    int ready = 0;
    long start = now_ms();

    while (ready < total && !g_terminate_requested) {
        struct pollfd pfd;
        pfd.fd = event_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pr = poll(&pfd, 1, 200);
        if (pr == -1) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (pr == 0) {
            if (now_ms() - start > 10000) {
                fprintf(stderr, "Timeout while waiting for children to become ready (%d/%d ready).\n", ready, total);
                return -1;
            }
            continue;
        }
        if (pfd.revents & POLLIN) {
            EventMsg ev;
            if (read_and_handle_event(event_fd, cfg, fifo_fd, wins, 0, &ev) == 0) {
                if (ev.type == EVENT_CHILD_READY && ev.team_id >= 0 && ev.team_id < TEAM_COUNT &&
                    ev.member_id >= 0 && ev.member_id < cfg->members_per_team && !seen[ev.team_id][ev.member_id]) {
                    seen[ev.team_id][ev.member_id] = 1;
                    ready++;
                }
            }
        }
    }
    return ready == total ? 0 : -1;
}

static void drain_old_events(int event_fd, const Config *cfg, int fifo_fd, int wins[TEAM_COUNT], int current_round_id, int max_ms) {
    (void)cfg;
    (void)fifo_fd;
    (void)wins;
    (void)current_round_id;

    /*
     * After a round winner is detected, children may still have a few old
     * movement/progress events already waiting in the event pipe. These old
     * messages must be consumed, but they must not be forwarded to the
     * visualizer, otherwise the UI will show furniture moving after the round
     * has already ended.
     */
    long start = now_ms();
    while (now_ms() - start < max_ms) {
        struct pollfd pfd;
        pfd.fd = event_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int pr = poll(&pfd, 1, 20);
        if (pr <= 0) {
            if (pr == -1 && errno == EINTR) continue;
            break;
        }

        if (pfd.revents & POLLIN) {
            EventMsg ev;
            (void)read_event_msg(event_fd, &ev);
        }
    }
}

static int wait_for_visual_round_done(int ack_fd, int round_id, int timeout_ms) {
    if (ack_fd < 0) return 0;

    /* timeout_ms == 0 means wait forever (until the visualizer acks
     * or the user terminates). */
    int wait_forever = (timeout_ms <= 0);
    long start = now_ms();

    while (wait_forever || (now_ms() - start < timeout_ms)) {
        if (g_terminate_requested) return -1;

        struct pollfd pfd;
        pfd.fd = ack_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int poll_wait;
        if (wait_forever) {
            poll_wait = 250;  /* poll in 250ms slices so we can check g_terminate_requested */
        } else {
            int remaining = timeout_ms - (int)(now_ms() - start);
            if (remaining < 50) remaining = 50;
            if (remaining > 250) remaining = 250;
            poll_wait = remaining;
        }

        int pr = poll(&pfd, 1, poll_wait);
        if (pr == -1) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (pr == 0) continue;

        if (pfd.revents & POLLIN) {
            EventMsg ack;
            ssize_t n = read(ack_fd, &ack, sizeof(ack));
            if (n == (ssize_t)sizeof(ack) && ack.magic == EVENT_MSG_MAGIC && ack.round_id == round_id) {
                return 0;
            }
        }
    }

    fprintf(stderr, "Warning: visualizer did not acknowledge round %d before timeout; continuing.\n", round_id);
    return -1;
}

static int leading_team(const int wins[TEAM_COUNT]) {
    if (wins[0] > wins[1]) return 0;
    if (wins[1] > wins[0]) return 1;
    return -1;
}

static int unreachable_leader(const int wins[TEAM_COUNT], int remaining_regular_rounds) {
    if (wins[0] > wins[1] + remaining_regular_rounds) return 0;
    if (wins[1] > wins[0] + remaining_regular_rounds) return 1;
    return -1;
}

int run_master(const Config *cfg) {
    if (install_master_signal_handlers() != 0) {
        perror("install_master_signal_handlers");
        return 1;
    }

    atexit(cleanup_fifos_at_exit);

    AllIPC ipc;
    if (ipc_create_all(&ipc, cfg) != 0) {
        perror("ipc_create_all");
        return 1;
    }

    reset_log_file(cfg);

    pid_t pids[TEAM_COUNT][MAX_MEMBERS];
    init_pids(pids);

    for (int t = 0; t < TEAM_COUNT; t++) {
        for (int m = 0; m < cfg->members_per_team; m++) {
            pid_t pid = fork();
            if (pid == -1) {
                perror("fork");
                signal_children(pids, cfg, SIGTERM);
                wait_children(pids, cfg);
                ipc_close_all(&ipc, cfg);
                return 1;
            }
            if (pid == 0) {
                ChildArgs args;
                args.cfg = *cfg;
                args.ipc = &ipc;
                args.team_id = t;
                args.member_id = m;
                run_team_member(&args);
            }
            pids[t][m] = pid;
        }
    }

    ipc_close_master_unused(&ipc, cfg);
    int event_fd = ipc.event_pipe[0];
    int fifo_fd = open_visual_fifo(cfg);
    char visual_ack_fifo[MAX_PATH];
    visual_ack_fifo[0] = '\0';
    int visual_ack_fd = -1;
    pid_t visualizer_pid = -1;
    if (fifo_fd >= 0) {
        make_visual_ack_fifo_path(cfg, visual_ack_fifo, sizeof(visual_ack_fifo));
        visual_ack_fd = open_visual_ack_fifo(visual_ack_fifo);
        visualizer_pid = launch_visualizer_child(cfg, visual_ack_fifo);
    }
    int wins[TEAM_COUNT] = {0, 0};

    if (wait_for_ready(event_fd, cfg, fifo_fd, wins) != 0) {
        fprintf(stderr, "Children did not become ready. Terminating.\n");
        signal_children(pids, cfg, SIGTERM);
        wait_children(pids, cfg);
        terminate_visualizer_child(visualizer_pid);
        if (fifo_fd >= 0) close(fifo_fd);
        if (visual_ack_fd >= 0) close(visual_ack_fd);
        close(event_fd);
        return 1;
    }

    printf("\nAll children are ready. Starting competition.\n");
    printf("Scheduled rounds: %d. If the score is tied after them, tie-break rounds will be added.\n\n",
           cfg->rounds_to_play);
    fflush(stdout);

    int round_id = 0;
    int champion = -1;

    while (!g_terminate_requested && champion < 0) {
        int tie_break_round = (round_id >= cfg->rounds_to_play && wins[0] == wins[1]);
        round_id++;

        char text[MAX_TEXT];
        if (tie_break_round) {
            safe_snprintf(text, sizeof(text),
                          "================ TIE-BREAK ROUND %d ================ Score is tied %d-%d; this round will judge the winner.",
                          round_id, wins[0], wins[1]);
            emit_master_event(cfg, fifo_fd, wins, EVENT_TIE_BREAK, round_id, text);
        } else {
            safe_snprintf(text, sizeof(text),
                          "================ ROUND %d/%d ================ Current score: team 0 = %d, team 1 = %d",
                          round_id, cfg->rounds_to_play, wins[0], wins[1]);
            emit_master_event(cfg, fifo_fd, wins, EVENT_ROUND_START, round_id, text);
        }
        fflush(stdout);

        signal_children(pids, cfg, SIGUSR1);
        long round_start = now_ms();
        int winner = -1;
        int timeout_round = 0;

        while (!g_terminate_requested && winner < 0 && !timeout_round) {
            struct pollfd pfd;
            pfd.fd = event_fd;
            pfd.events = POLLIN;
            pfd.revents = 0;
            int pr = poll(&pfd, 1, cfg->master_poll_ms);
            if (pr == -1) {
                if (errno == EINTR) continue;
                perror("master poll");
                g_terminate_requested = 1;
                break;
            }
            if (pr > 0 && (pfd.revents & POLLIN)) {
                EventMsg ev;
                if (read_and_handle_event(event_fd, cfg, fifo_fd, wins, round_id, &ev) == 0) {
                    if (ev.round_id == round_id && ev.type == EVENT_ROUND_WIN && winner < 0) {
                        winner = ev.team_id;
                        /* Do NOT stop children immediately - let the losing team
                         * finish their in-flight piece for a smooth visual ending.
                         * We give a grace period to drain pending animations,
                         * then stop. */
                        if (cfg->graphics_enabled) {
                            long grace_start = now_ms();
                            long grace_period = 1500;  /* 1.5 seconds for in-flight pieces */
                            while (now_ms() - grace_start < grace_period && !g_terminate_requested) {
                                struct pollfd gp;
                                gp.fd = event_fd;
                                gp.events = POLLIN;
                                gp.revents = 0;
                                int gpr = poll(&gp, 1, 50);
                                if (gpr > 0 && (gp.revents & POLLIN)) {
                                    EventMsg gev;
                                    /* keep forwarding events to visualizer during grace */
                                    read_and_handle_event(event_fd, cfg, fifo_fd, wins, round_id, &gev);
                                }
                            }
                        }
                        signal_children(pids, cfg, SIGUSR2);
                    }
                }
            }

            if (cfg->max_round_seconds > 0 && now_ms() - round_start > (long)cfg->max_round_seconds * 1000L) {
                timeout_round = 1;
            }
        }

        signal_children(pids, cfg, SIGUSR2);

        if (winner >= 0 && winner < TEAM_COUNT) {
            wins[winner]++;
            printf("Round %d result: team %d wins. Score: team 0 = %d, team 1 = %d\n\n",
                   round_id, winner, wins[0], wins[1]);
        } else if (timeout_round) {
            printf("Round %d timed out after %d seconds. No score change.\n\n", round_id, cfg->max_round_seconds);
        }
        fflush(stdout);

        drain_old_events(event_fd, cfg, fifo_fd, wins, round_id, cfg->round_pause_ms);

        if (winner >= 0 && fifo_fd >= 0 && visualizer_pid > 0) {
            printf("Waiting for visualizer to finish round %d animation...\n", round_id);
            fflush(stdout);
            /* If max_round_seconds=0 (no timeout), wait indefinitely for the
            * visualizer to finish animating. Otherwise use a timeout. */
            int viz_timeout_ms = (cfg->max_round_seconds > 0)
                                ? (cfg->max_round_seconds * 1000 + 30000)  /* round time + 30s grace */
                                : 0;  /* 0 = wait forever */
            (void)wait_for_visual_round_done(visual_ack_fd, round_id, viz_timeout_ms);
        }

        if (timeout_round && fifo_fd >= 0 && visualizer_pid > 0) {
            printf("Waiting for visualizer to finish round %d animation after timeout...\n", round_id);
            fflush(stdout);
            /* Use a small timeout here since simulation already gave up */
            (void)wait_for_visual_round_done(visual_ack_fd, round_id, 10000);
        }

        if (winner >= 0 && !g_terminate_requested) {
            safe_snprintf(text, sizeof(text),
                          "Round %d finished: Team %d won this round. Current result: Team 0 = %d, Team 1 = %d. Next round starts in 2 seconds.",
                          round_id, winner, wins[0], wins[1]);
            emit_master_event(cfg, fifo_fd, wins, EVENT_ROUND_RESULT, round_id, text);
        } else if (timeout_round && !g_terminate_requested) {
            safe_snprintf(text, sizeof(text),
                          "Round %d finished with timeout. Current result: Team 0 = %d, Team 1 = %d. Next round starts in 2 seconds.",
                          round_id, wins[0], wins[1]);
            emit_master_event(cfg, fifo_fd, wins, EVENT_ROUND_RESULT, round_id, text);
        }

        if (!g_terminate_requested) {
            if (round_id < cfg->rounds_to_play) {
                int remaining = cfg->rounds_to_play - round_id;
                champion = unreachable_leader(wins, remaining);
                if (champion >= 0) {
                    safe_snprintf(text, sizeof(text),
                                  "Competition decided early: team %d leads %d-%d with %d regular round(s) left, so the other team cannot catch up.",
                                  champion, wins[0], wins[1], remaining);
                    emit_master_event(cfg, fifo_fd, wins, EVENT_COMPETITION_END, round_id, text);
                }
            } else {
                champion = leading_team(wins);
                if (champion < 0) {
                    safe_snprintf(text, sizeof(text),
                                  "Tie after %d scheduled round(s): team 0 = %d, team 1 = %d. Starting an extra tie-break round.",
                                  cfg->rounds_to_play, wins[0], wins[1]);
                    emit_master_event(cfg, fifo_fd, wins, EVENT_TIE_BREAK, round_id + 1, text);
                }
            }
        }

        sleep_ms(cfg->round_pause_ms);
    }

    if (champion >= 0) {
        char text[MAX_TEXT];
        safe_snprintf(text, sizeof(text),
                      "COMPETITION WINNER: TEAM %d. Final score: team 0 = %d, team 1 = %d.",
                      champion, wins[0], wins[1]);
        emit_master_event(cfg, fifo_fd, wins, EVENT_COMPETITION_END, round_id, text);

        printf("==========================================\n");
        printf("COMPETITION WINNER: TEAM %d\n", champion);
        printf("Final score: team 0 = %d, team 1 = %d\n", wins[0], wins[1]);
        printf("Rounds played: %d\n", round_id);
        printf("==========================================\n");
    } else {
        printf("Competition interrupted. Final score: team 0 = %d, team 1 = %d\n", wins[0], wins[1]);
    }
    fflush(stdout);

    signal_children(pids, cfg, SIGTERM);
    wait_children(pids, cfg);

    if (champion >= 0) {
        wait_visualizer_child(visualizer_pid);
    } else {
        terminate_visualizer_child(visualizer_pid);
    }

    if (fifo_fd >= 0) close(fifo_fd);
    if (visual_ack_fd >= 0) close(visual_ack_fd);

    if (visual_ack_fifo[0]) unlink(visual_ack_fifo);
    g_cleanup_ack_fifo[0] = '\0';

    if (cfg->graphics_enabled && cfg->graphics_fifo[0]) {
        unlink(cfg->graphics_fifo);
    }
    g_cleanup_main_fifo[0] = '\0';

    close(event_fd);
    return champion >= 0 ? 0 : 130;
}
