#include "team.h"
#include "furniture.h"
#include "signals.h"
#include "util.h"

#include <errno.h>
#include <poll.h>
#include <unistd.h>

static void fill_event_config_fields(EventMsg *ev, const ChildArgs *args) {
    ev->furniture_count = args->cfg.furniture_count;
    ev->member_count = args->cfg.members_per_team;
}

static int child_event(const ChildArgs *args, EventType type, int round_id, const char *text) {
    EventMsg ev = make_event(type, args->team_id, args->member_id, round_id, text);
    fill_event_config_fields(&ev, args);
    return write_event_msg(args->ipc->event_pipe[1], &ev);
}

static int child_event_full(const ChildArgs *args, EventType type, int round_id,
                            int piece_id, int serial_no, int accepted_count,
                            int expected_serial, const char *text) {
    EventMsg ev = make_event(type, args->team_id, args->member_id, round_id, text);
    ev.piece_id = piece_id;
    ev.serial_no = serial_no;
    ev.accepted_count = accepted_count;
    ev.expected_serial = expected_serial;
    fill_event_config_fields(&ev, args);
    return write_event_msg(args->ipc->event_pipe[1], &ev);
}

/*
 * The UI is synchronized at the SOURCE, not at every middle member.
 * The real furniture travels through pipes first. Only when the source receives
 * an ACK or a returned piece does it know the final result of that attempt.
 * At that moment it sends ONE visual transaction event. The visualizer then
 * animates the complete path and updates the progress bar at the exact end of
 * that visual transaction.
 */
static int child_visual_result(const ChildArgs *args, int round_id,
                               int piece_id, int serial_no,
                               int accepted_count, MessageDirection result_direction) {
    char text[MAX_TEXT];
    const char *result = result_direction == DIR_FORWARD ? "accepted" : "rejected";
    safe_snprintf(text, sizeof(text), "visual transaction %s: serial %d piece %d progress %d/%d",
                  result, serial_no, piece_id, accepted_count, args->cfg.furniture_count);

    EventMsg ev = make_event(EVENT_VISUAL_RESULT, args->team_id, args->member_id, round_id, text);
    ev.piece_id = piece_id;
    ev.serial_no = serial_no;
    ev.accepted_count = accepted_count;
    ev.expected_serial = accepted_count + 1;
    ev.direction = result_direction;
    ev.from_member = 0;
    ev.to_member = args->cfg.members_per_team - 1;
    ev.touches = 0;
    fill_event_config_fields(&ev, args);
    return write_event_msg(args->ipc->event_pipe[1], &ev);
}

static int wait_for_round_start(void) {
    while (!g_terminate_requested && !g_start_round_requested) {
        sleep_ms_interruptible(50);
    }
    if (g_terminate_requested) return 0;
    g_start_round_requested = 0;
    g_round_active = 1;
    return 1;
}

static void drain_member_inputs(AllIPC *ipc, const Config *cfg, int team_id, int member_id) {
    TeamIPC *team = &ipc->teams[team_id];
    int last = cfg->members_per_team - 1;
    if (member_id == 0) {
        drain_fd(team->backward[0][0]);
        drain_fd(team->ack[0]);
    } else if (member_id == last) {
        drain_fd(team->forward[last - 1][0]);
    } else {
        drain_fd(team->forward[member_id - 1][0]);
        drain_fd(team->backward[member_id][0]);
    }
}

static unsigned int child_seed(const Config *cfg, int team_id, int member_id) {
    unsigned int base = cfg->random_seed;
    if (base == 0) base = (unsigned int)(now_ms() ^ (long)getpid());
    return base ^ (unsigned int)(team_id * 1000003u) ^
           (unsigned int)(member_id * 9176u) ^ (unsigned int)getpid();
}

static void run_source(const ChildArgs *args) {
    TeamIPC *team = &args->ipc->teams[args->team_id];
    int out_fd = team->forward[0][1];
    int return_fd = team->backward[0][0];
    int ack_fd = team->ack[0];
    int round_id = 0;
    unsigned int seed = child_seed(&args->cfg, args->team_id, args->member_id);

    child_event(args, EVENT_CHILD_READY, 0, "source ready");

    while (wait_for_round_start()) {
        round_id++;
        drain_member_inputs(args->ipc, &args->cfg, args->team_id, args->member_id);

        int n = args->cfg.furniture_count;
        int *serials = calloc((size_t)n, sizeof(int));
        unsigned char *delivered = calloc((size_t)n, sizeof(unsigned char));
        unsigned char *blocked = calloc((size_t)n, sizeof(unsigned char));
        if (!serials || !delivered || !blocked) {
            child_event(args, EVENT_ERROR, round_id, "source memory allocation failed");
            free(serials); free(delivered); free(blocked);
            break;
        }

        char error[MAX_TEXT];
        if (generate_serials(&args->cfg, round_id, serials, error, sizeof(error)) != 0) {
            child_event(args, EVENT_ERROR, round_id, error);
            free(serials); free(delivered); free(blocked);
            break;
        }
        reset_status_arrays(n, delivered, blocked);

        char text[MAX_TEXT];
        safe_snprintf(text, sizeof(text), "round %d started at source", round_id);
        child_event(args, EVENT_LOG, round_id, text);

        int accepted_by_source = 0;
        int moves_done = 0;
        while (g_round_active && !g_terminate_requested) {
            if (accepted_by_source >= n) {
                sleep_ms_interruptible(50);
                continue;
            }
            if (!any_available_piece(n, delivered, blocked)) {
                clear_blocks(n, blocked);
                child_event(args, EVENT_LOG, round_id,
                            "all remaining pieces were blocked; blocks cleared as safety fallback");
            }

            int piece = choose_available_piece(n, delivered, blocked, &seed);
            if (piece < 0) {
                sleep_ms_interruptible(20);
                continue;
            }

            int delay = tired_delay_ms(&args->cfg, &seed, moves_done++);
            sleep_ms_interruptible(delay);
            if (!g_round_active || g_terminate_requested) break;

            FurnitureMsg msg = make_furniture_msg(args->team_id, round_id, piece, serials[piece], DIR_FORWARD);
            msg.touches++;
            msg.last_pid = getpid();
            if (write_furniture_msg(out_fd, &msg) != 0) {
                child_event(args, EVENT_ERROR, round_id, "source failed to write furniture to forward pipe");
                break;
            }

            int waiting_for_result = 1;
            while (waiting_for_result && g_round_active && !g_terminate_requested) {
                struct pollfd pfds[2];
                pfds[0].fd = ack_fd;
                pfds[0].events = POLLIN;
                pfds[0].revents = 0;
                pfds[1].fd = return_fd;
                pfds[1].events = POLLIN;
                pfds[1].revents = 0;

                int pr = poll(pfds, 2, 100);
                if (pr == -1) {
                    if (errno == EINTR) continue;
                    child_event(args, EVENT_ERROR, round_id, "source poll failed");
                    waiting_for_result = 0;
                    break;
                }
                if (pr == 0) continue;

                if (pfds[0].revents & POLLIN) {
                    FurnitureMsg ack;
                    if (read_furniture_msg(ack_fd, &ack) == 0 &&
                        ack.round_id == round_id && ack.kind == MSG_ACK) {
                        if (ack.piece_id >= 0 && ack.piece_id < n && !delivered[ack.piece_id]) {
                            delivered[ack.piece_id] = 1;
                            accepted_by_source++;
                        }

                        child_visual_result(args, round_id, ack.piece_id, ack.serial_no,
                                            accepted_by_source, DIR_FORWARD);

                        safe_snprintf(text, sizeof(text), "accepted serial %d (piece %d), progress %d/%d",
                                      ack.serial_no, ack.piece_id, accepted_by_source, n);
                        child_event_full(args, EVENT_ACCEPTED, round_id, ack.piece_id, ack.serial_no,
                                         accepted_by_source, accepted_by_source + 1, text);

                        if (accepted_by_source >= n) {
                            safe_snprintf(text, sizeof(text), "team %d won round %d", args->team_id, round_id);
                            child_event_full(args, EVENT_ROUND_WIN, round_id, ack.piece_id, ack.serial_no,
                                             accepted_by_source, accepted_by_source + 1, text);
                            g_round_active = 0;
                        }

                        clear_blocks(n, blocked);
                        waiting_for_result = 0;
                    }
                }

                if (waiting_for_result && (pfds[1].revents & POLLIN)) {
                    FurnitureMsg returned;
                    if (read_furniture_msg(return_fd, &returned) == 0 &&
                        returned.round_id == round_id && returned.kind == MSG_FURNITURE) {
                        if (returned.piece_id >= 0 && returned.piece_id < n && !delivered[returned.piece_id]) {
                            blocked[returned.piece_id] = 1;
                        }

                        child_visual_result(args, round_id, returned.piece_id, returned.serial_no,
                                            accepted_by_source, DIR_BACKWARD);

                        safe_snprintf(text, sizeof(text), "rejected serial %d (piece %d) returned to source",
                                      returned.serial_no, returned.piece_id);
                        child_event_full(args, EVENT_REJECTED, round_id, returned.piece_id, returned.serial_no,
                                         accepted_by_source, 0, text);

                        waiting_for_result = 0;
                    }
                }
            }
        }

        free(serials);
        free(delivered);
        free(blocked);
        child_event(args, EVENT_LOG, round_id, "source stopped round");
    }

    child_event(args, EVENT_CHILD_EXIT, round_id, "source exiting");
}

static void run_middle(const ChildArgs *args) {
    TeamIPC *team = &args->ipc->teams[args->team_id];
    int m = args->member_id;
    int forward_in = team->forward[m - 1][0];
    int forward_out = team->forward[m][1];
    int backward_in = team->backward[m][0];
    int backward_out = team->backward[m - 1][1];
    int round_id = 0;
    int moves_done = 0;
    unsigned int seed = child_seed(&args->cfg, args->team_id, args->member_id);

    child_event(args, EVENT_CHILD_READY, 0, "middle ready");

    while (wait_for_round_start()) {
        round_id++;
        drain_member_inputs(args->ipc, &args->cfg, args->team_id, args->member_id);
        moves_done = 0;
        child_event(args, EVENT_LOG, round_id, "middle started round");

        while (g_round_active && !g_terminate_requested) {
            struct pollfd pfds[2];
            pfds[0].fd = forward_in;
            pfds[0].events = POLLIN;
            pfds[0].revents = 0;
            pfds[1].fd = backward_in;
            pfds[1].events = POLLIN;
            pfds[1].revents = 0;

            int pr = poll(pfds, 2, 100);
            if (pr == -1) {
                if (errno == EINTR) continue;
                child_event(args, EVENT_ERROR, round_id, "middle poll failed");
                break;
            }
            if (pr == 0) continue;

            if (pfds[0].revents & POLLIN) {
                FurnitureMsg msg;
                if (read_furniture_msg(forward_in, &msg) == 0) {
                    if (msg.round_id != round_id || msg.kind != MSG_FURNITURE) continue;
                    int delay = tired_delay_ms(&args->cfg, &seed, moves_done++);
                    sleep_ms_interruptible(delay);
                    if (!g_round_active || g_terminate_requested) continue;
                    msg.direction = DIR_FORWARD;
                    msg.touches++;
                    msg.last_pid = getpid();
                    if (write_furniture_msg(forward_out, &msg) != 0) {
                        child_event(args, EVENT_ERROR, round_id, "middle failed to forward furniture");
                        break;
                    }
                }
            }

            if (pfds[1].revents & POLLIN) {
                FurnitureMsg msg;
                if (read_furniture_msg(backward_in, &msg) == 0) {
                    if (msg.round_id != round_id || msg.kind != MSG_FURNITURE) continue;
                    int delay = tired_delay_ms(&args->cfg, &seed, moves_done++);
                    sleep_ms_interruptible(delay);
                    if (!g_round_active || g_terminate_requested) continue;
                    msg.direction = DIR_BACKWARD;
                    msg.touches++;
                    msg.last_pid = getpid();
                    if (write_furniture_msg(backward_out, &msg) != 0) {
                        child_event(args, EVENT_ERROR, round_id, "middle failed to return furniture");
                        break;
                    }
                }
            }
        }

        child_event(args, EVENT_LOG, round_id, "middle stopped round");
    }

    child_event(args, EVENT_CHILD_EXIT, round_id, "middle exiting");
}

static void run_sink(const ChildArgs *args) {
    TeamIPC *team = &args->ipc->teams[args->team_id];
    int last = args->cfg.members_per_team - 1;
    int in_fd = team->forward[last - 1][0];
    int return_fd = team->backward[last - 1][1];
    int ack_fd = team->ack[1];
    int round_id = 0;
    int moves_done = 0;
    unsigned int seed = child_seed(&args->cfg, args->team_id, args->member_id);

    child_event(args, EVENT_CHILD_READY, 0, "sink ready");

    while (wait_for_round_start()) {
        round_id++;
        drain_member_inputs(args->ipc, &args->cfg, args->team_id, args->member_id);
        moves_done = 0;
        int expected_serial = 1;
        int accepted_count = 0;
        child_event(args, EVENT_LOG, round_id, "sink started round");

        while (g_round_active && !g_terminate_requested) {
            struct pollfd pfd;
            pfd.fd = in_fd;
            pfd.events = POLLIN;
            pfd.revents = 0;
            int pr = poll(&pfd, 1, 100);
            if (pr == -1) {
                if (errno == EINTR) continue;
                child_event(args, EVENT_ERROR, round_id, "sink poll failed");
                break;
            }
            if (pr == 0) continue;
            if (!(pfd.revents & POLLIN)) continue;

            FurnitureMsg msg;
            if (read_furniture_msg(in_fd, &msg) != 0) continue;
            if (msg.round_id != round_id || msg.kind != MSG_FURNITURE) continue;

            int delay = tired_delay_ms(&args->cfg, &seed, moves_done++);
            sleep_ms_interruptible(delay);
            if (!g_round_active || g_terminate_requested) continue;

            if (msg.serial_no == expected_serial) {
                accepted_count++;

                FurnitureMsg ack = make_ack_msg(args->team_id, round_id, msg.piece_id, msg.serial_no);
                if (write_furniture_msg(ack_fd, &ack) != 0) {
                    child_event(args, EVENT_ERROR, round_id, "sink failed to send ack");
                    break;
                }

                expected_serial++;

                if (accepted_count >= args->cfg.furniture_count) {
                    g_round_active = 0;
                    break;
                }
            } else {
                msg.direction = DIR_BACKWARD;
                msg.touches++;
                msg.last_pid = getpid();
                if (write_furniture_msg(return_fd, &msg) != 0) {
                    child_event(args, EVENT_ERROR, round_id, "sink failed to return wrong furniture");
                    break;
                }
            }
        }

        child_event(args, EVENT_LOG, round_id, "sink stopped round");
    }

    child_event(args, EVENT_CHILD_EXIT, round_id, "sink exiting");
}

void run_team_member(const ChildArgs *args) {
    reset_child_signal_state();
    if (install_child_signal_handlers() != 0) {
        perror("install_child_signal_handlers");
        _exit(2);
    }

    ipc_close_child_unused(args->ipc, &args->cfg, args->team_id, args->member_id);

    const char *role = role_name(args->member_id, args->cfg.members_per_team);
    (void)role;

    if (args->member_id == 0) {
        run_source(args);
    } else if (args->member_id == args->cfg.members_per_team - 1) {
        run_sink(args);
    } else {
        run_middle(args);
    }

    _exit(0);
}
