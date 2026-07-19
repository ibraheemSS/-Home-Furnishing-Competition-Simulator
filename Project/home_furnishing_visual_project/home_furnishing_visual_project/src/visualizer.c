/*
 * OpenGL visualizer for the home furnishing competition.
 *
 * Synchronization design:
 * - The backend sends EVENT_VISUAL_RESULT only after the source receives ACK or
 *   the rejected product returns to the source.
 * - The visualizer animates that whole completed transaction.
 * - The progress bar is updated only after the accepted transaction visually
 *   reaches the sink.
 * - The master waits for a visual round-done ACK before starting the next round.
 */
#include "ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __APPLE__
#include <GLUT/glut.h>
#else
#include <GL/glut.h>
#endif

#define WINDOW_W 1180
#define WINDOW_H 720
#define QUEUE_CAP 1024
#define BASE_SEGMENT_MS 700.0f
#define CELEBRATION_MS 2200L

typedef struct TeamTheme {
    float lane_r, lane_g, lane_b;
    float lane_edge_r, lane_edge_g, lane_edge_b;
    float main_r, main_g, main_b;
    float accent_r, accent_g, accent_b;
    float pale_r, pale_g, pale_b;
} TeamTheme;

typedef struct VisualTx {
    int team_id;
    int round_id;
    int piece_id;
    int serial_no;
    int direction;       /* DIR_FORWARD = accepted, DIR_BACKWARD = rejected */
    int accepted_count;  /* valid for accepted transactions */
    int expected_serial;
    int member_count;
    int touches;
} VisualTx;

typedef struct TxQueue {
    VisualTx data[QUEUE_CAP];
    int head;
    int tail;
    int count;
} TxQueue;

typedef struct ProductAnim {
    int active;
    VisualTx tx;
    int segment_index;
    int total_segments;
    int from_member;
    int to_member;
    int current_direction;
    float start_ms;
    float duration_ms;
} ProductAnim;

static int g_fifo_fd = -1;
static int g_ack_fd = -1;
static int g_progress[TEAM_COUNT] = {0, 0};
static int g_furniture_count = 1;
static int g_wins[TEAM_COUNT] = {0, 0};
static int g_round = 0;
static int g_members = 5;
static int g_round_finished = 0;
static int g_round_done_sent = 0;
static int g_competition_finished = 0;
static int g_winner_team = -1;
static int g_paused = 0;
static float g_pause_started_ms = 0.0f;
static float g_round_start_ms = 0.0f;
static long g_celebrate_until = 0;
static int g_celebrate_team = -1;
static int g_last_serial[TEAM_COUNT] = {-1, -1};
static int g_last_piece[TEAM_COUNT] = {-1, -1};
static int g_stat_accepted[TEAM_COUNT] = {0, 0};
static int g_stat_rejected[TEAM_COUNT] = {0, 0};
static long g_accept_flash_until[TEAM_COUNT] = {0, 0};
static long g_reject_flash_until[TEAM_COUNT] = {0, 0};
static long g_reject_notice_until[TEAM_COUNT] = {0, 0};
static char g_reject_notice[TEAM_COUNT][96] = {{0}, {0}};
static char g_phase[256] = "Waiting for simulation events...";
static char g_final_message[256] = "";

static TxQueue g_queues[TEAM_COUNT];
static ProductAnim g_anim[TEAM_COUNT];

static const TeamTheme g_theme[TEAM_COUNT] = {
    {0.97f, 0.98f, 0.99f, 0.08f, 0.30f, 0.68f, 0.03f, 0.22f, 0.55f, 0.02f, 0.40f, 0.82f, 0.90f, 0.94f, 1.00f},
    {0.97f, 0.98f, 0.97f, 0.02f, 0.45f, 0.25f, 0.02f, 0.38f, 0.22f, 0.04f, 0.58f, 0.34f, 0.90f, 0.97f, 0.92f}
};

static float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static float visual_now_ms(void) {
    return (float)glutGet(GLUT_ELAPSED_TIME);
}

static void set_rgb(float r, float g, float b) {
    glColor3f(r, g, b);
}

static void set_rgba(float r, float g, float b, float a) {
    glColor4f(r, g, b, a);
}

static int display_team_id(int team_id) {
    return team_id + 1;
}

static const TeamTheme *team_theme(int team_id) {
    return &g_theme[team_id == 1 ? 1 : 0];
}

static void append_text(char *dst, size_t size, size_t *pos, const char *text) {
    while (*text && *pos + 1 < size) {
        dst[*pos] = *text;
        (*pos)++;
        text++;
    }
    if (size > 0) dst[*pos] = '\0';
}

static void copy_display_text(char *dst, size_t size, const char *src) {
    size_t pos = 0;
    if (size == 0) return;
    dst[0] = '\0';

    while (*src && pos + 1 < size) {
        if (strncmp(src, "Team 0", 6) == 0) {
            append_text(dst, size, &pos, "Team 1");
            src += 6;
        } else if (strncmp(src, "Team 1", 6) == 0) {
            append_text(dst, size, &pos, "Team 2");
            src += 6;
        } else if (strncmp(src, "team 0", 6) == 0) {
            append_text(dst, size, &pos, "team 1");
            src += 6;
        } else if (strncmp(src, "team 1", 6) == 0) {
            append_text(dst, size, &pos, "team 2");
            src += 6;
        } else if (strncmp(src, "TEAM 0", 6) == 0) {
            append_text(dst, size, &pos, "TEAM 1");
            src += 6;
        } else if (strncmp(src, "TEAM 1", 6) == 0) {
            append_text(dst, size, &pos, "TEAM 2");
            src += 6;
        } else {
            dst[pos++] = *src++;
            dst[pos] = '\0';
        }
    }
}

static int text_width_small(const char *text) {
    int w = 0;
    for (const char *p = text; *p; p++) w += glutBitmapWidth(GLUT_BITMAP_HELVETICA_12, *p);
    return w;
}

static void draw_text_small(float x, float y, const char *text) {
    glRasterPos2f(x, y);
    for (const char *p = text; *p; p++) glutBitmapCharacter(GLUT_BITMAP_HELVETICA_12, *p);
}

static void draw_text_small_centered(float cx, float y, const char *text) {
    draw_text_small(cx - (float)text_width_small(text) / 2.0f, y, text);
}

static void draw_text(float x, float y, const char *text) {
    glRasterPos2f(x, y);
    for (const char *p = text; *p; p++) glutBitmapCharacter(GLUT_BITMAP_HELVETICA_18, *p);
}

static void draw_text_large(float x, float y, const char *text) {
    glRasterPos2f(x, y);
    for (const char *p = text; *p; p++) glutBitmapCharacter(GLUT_BITMAP_TIMES_ROMAN_24, *p);
}

static void draw_rect(float x, float y, float w, float h) {
    glBegin(GL_QUADS);
    glVertex2f(x, y);
    glVertex2f(x + w, y);
    glVertex2f(x + w, y + h);
    glVertex2f(x, y + h);
    glEnd();
}

static void draw_rect_gradient_vertical(float x, float y, float w, float h,
                                        float r1, float g1, float b1,
                                        float r2, float g2, float b2) {
    glBegin(GL_QUADS);
    glColor3f(r1, g1, b1);
    glVertex2f(x, y);
    glVertex2f(x + w, y);
    glColor3f(r2, g2, b2);
    glVertex2f(x + w, y + h);
    glVertex2f(x, y + h);
    glEnd();
}

static void draw_rect_outline(float x, float y, float w, float h) {
    glBegin(GL_LINE_LOOP);
    glVertex2f(x, y);
    glVertex2f(x + w, y);
    glVertex2f(x + w, y + h);
    glVertex2f(x, y + h);
    glEnd();
}

static void draw_circle(float cx, float cy, float r, int segments) {
    glBegin(GL_TRIANGLE_FAN);
    glVertex2f(cx, cy);
    for (int i = 0; i <= segments; i++) {
        float a = (float)i * 6.28318530718f / (float)segments;
        glVertex2f(cx + cosf(a) * r, cy + sinf(a) * r);
    }
    glEnd();
}

static void draw_arrow_line(float x1, float y1, float x2, float y2,
                            float r, float g, float b, float width) {
    set_rgb(r, g, b);
    glLineWidth(width);
    glBegin(GL_LINES);
    glVertex2f(x1, y1);
    glVertex2f(x2, y2);
    glEnd();
    glLineWidth(1.0f);

    float dir = (x2 >= x1) ? 1.0f : -1.0f;
    float arrow = 13.0f;
    glBegin(GL_TRIANGLES);
    glVertex2f(x2, y2);
    glVertex2f(x2 - dir * arrow, y2 + 7.5f);
    glVertex2f(x2 - dir * arrow, y2 - 7.5f);
    glEnd();
}

static float layout_left(void) {
    return (g_members > 12) ? 120.0f : 150.0f;
}

static float layout_right(void) {
    return (g_members > 12) ? 1040.0f : 1005.0f;
}

static float member_spacing(void) {
    int n = g_members < 2 ? 2 : g_members;
    return (layout_right() - layout_left()) / (float)(n - 1);
}

static float member_x(int member_id) {
    int n = g_members < 2 ? 2 : g_members;
    member_id = clampi(member_id, 0, n - 1);
    return layout_left() + member_spacing() * (float)member_id;
}

static float team_y(int team_id) {
    return team_id == 0 ? 465.0f : 245.0f;
}

static float box_w(void) {
    return clampf(member_spacing() * 0.58f, 24.0f, 78.0f);
}

static float box_h(void) {
    return g_members > 14 ? 58.0f : 78.0f;
}

static int queue_push(int team_id, const VisualTx *tx) {
    if (team_id < 0 || team_id >= TEAM_COUNT) return -1;
    TxQueue *q = &g_queues[team_id];
    if (q->count >= QUEUE_CAP) {
        q->head = (q->head + 1) % QUEUE_CAP;
        q->count--;
    }
    q->data[q->tail] = *tx;
    q->tail = (q->tail + 1) % QUEUE_CAP;
    q->count++;
    return 0;
}

static int queue_pop(int team_id, VisualTx *tx) {
    if (team_id < 0 || team_id >= TEAM_COUNT) return -1;
    TxQueue *q = &g_queues[team_id];
    if (q->count <= 0) return -1;
    *tx = q->data[q->head];
    q->head = (q->head + 1) % QUEUE_CAP;
    q->count--;
    return 0;
}

static void clear_team_motion(int team_id) {
    if (team_id < 0 || team_id >= TEAM_COUNT) return;
    memset(&g_queues[team_id], 0, sizeof(g_queues[team_id]));
    memset(&g_anim[team_id], 0, sizeof(g_anim[team_id]));
}

static void reset_visual_round(int round_id) {
    g_progress[0] = 0;
    g_progress[1] = 0;
    g_stat_accepted[0] = g_stat_accepted[1] = 0;
    g_stat_rejected[0] = g_stat_rejected[1] = 0;
    g_last_serial[0] = g_last_serial[1] = -1;
    g_last_piece[0] = g_last_piece[1] = -1;
    g_accept_flash_until[0] = g_accept_flash_until[1] = 0;
    g_reject_flash_until[0] = g_reject_flash_until[1] = 0;
    g_reject_notice_until[0] = g_reject_notice_until[1] = 0;
    g_reject_notice[0][0] = g_reject_notice[1][0] = '\0';
    clear_team_motion(0);
    clear_team_motion(1);
    g_round = round_id;
    g_round_finished = 0;
    g_round_done_sent = 0;
    g_winner_team = -1;
    g_celebrate_until = 0;
    g_celebrate_team = -1;
    g_round_start_ms = visual_now_ms();
    g_competition_finished = 0;
    g_final_message[0] = '\0';
}

static void show_rejection_notice(int team_id, const VisualTx *tx) {
    if (team_id < 0 || team_id >= TEAM_COUNT) return;
    snprintf(g_reject_notice[team_id], sizeof(g_reject_notice[team_id]),
             "Rejected: Expected %d, Got %d", tx->expected_serial, tx->serial_no);
    g_reject_notice_until[team_id] = (long)visual_now_ms() + 1600L;
}

static void start_celebration(int team_id) {
    g_celebrate_team = team_id;
    g_celebrate_until = (long)visual_now_ms() + CELEBRATION_MS;
}

static void toggle_pause(void) {
    float now = visual_now_ms();
    if (!g_paused) {
        g_paused = 1;
        g_pause_started_ms = now;
    } else {
        float paused_for = now - g_pause_started_ms;
        long paused_for_long = (long)paused_for;
        for (int t = 0; t < TEAM_COUNT; t++) {
            if (g_anim[t].active) g_anim[t].start_ms += paused_for;
            if (g_accept_flash_until[t] > 0) g_accept_flash_until[t] += paused_for_long;
            if (g_reject_flash_until[t] > 0) g_reject_flash_until[t] += paused_for_long;
            if (g_reject_notice_until[t] > 0) g_reject_notice_until[t] += paused_for_long;
        }
        if (g_celebrate_until > 0) g_celebrate_until += paused_for_long;
        g_paused = 0;
    }
}

static int visualizer_has_pending_work(void) {
    for (int t = 0; t < TEAM_COUNT; t++) {
        if (g_anim[t].active) return 1;
        if (g_queues[t].count > 0) return 1;
    }
    return 0;
}

static int tx_total_segments(const VisualTx *tx) {
    int last = tx->member_count - 1;
    if (last < 1) last = 1;
    return tx->direction == DIR_BACKWARD ? last * 2 : last;
}

static void setup_segment(ProductAnim *a) {
    int last = a->tx.member_count - 1;
    if (last < 1) last = 1;

    if (a->tx.direction == DIR_FORWARD) {
        a->from_member = a->segment_index;
        a->to_member = a->segment_index + 1;
    } else if (a->segment_index < last) {
        a->from_member = a->segment_index;
        a->to_member = a->segment_index + 1;
    } else {
        int back_index = a->segment_index - last;
        a->from_member = last - back_index;
        a->to_member = last - back_index - 1;
    }

    a->current_direction = (a->to_member > a->from_member) ? DIR_FORWARD : DIR_BACKWARD;
    a->start_ms = visual_now_ms();

    /* Use a stable, human-visible speed. Do not speed up because of backlog;
       otherwise cartons become too fast and the progress bar feels unsynchronized. */
    a->duration_ms = clampf(BASE_SEGMENT_MS + (float)a->tx.touches * 10.0f, 550.0f, 950.0f);
}

static void start_next_anim_if_needed(int team_id) {
    if (team_id < 0 || team_id >= TEAM_COUNT) return;
    if (g_anim[team_id].active) return;

    VisualTx tx;
    if (queue_pop(team_id, &tx) != 0) return;

    if (tx.member_count < 2) tx.member_count = g_members;
    if (tx.member_count < 2) tx.member_count = 2;

    g_anim[team_id].active = 1;
    g_anim[team_id].tx = tx;
    g_anim[team_id].segment_index = 0;
    g_anim[team_id].total_segments = tx_total_segments(&tx);
    setup_segment(&g_anim[team_id]);
}

static void finish_transaction(int team_id, const VisualTx *tx) {
    g_last_serial[team_id] = tx->serial_no;
    g_last_piece[team_id] = tx->piece_id;

    
    if (tx->direction == DIR_FORWARD) {
        g_stat_accepted[team_id]++;
    } else if (tx->direction == DIR_BACKWARD) {
        g_stat_rejected[team_id]++;
    }

    if (tx->direction == DIR_FORWARD) {
        g_progress[team_id] = clampi(tx->accepted_count, 0, g_furniture_count);
        g_accept_flash_until[team_id] = (long)visual_now_ms() + 650L;

       
        if (!g_round_finished &&
            g_furniture_count > 0 &&
            g_progress[team_id] >= g_furniture_count) {

            g_round_finished = 1;
            g_winner_team = team_id;

            
            int loser = 1 - team_id;
            memset(&g_queues[loser], 0, sizeof(g_queues[loser]));
            memset(&g_anim[loser], 0, sizeof(g_anim[loser]));

            snprintf(g_phase, sizeof(g_phase),
                     "Round %d winner: Team %d!", g_round, display_team_id(team_id));
            start_celebration(team_id);
        }
    } else {
        g_reject_flash_until[team_id] = (long)visual_now_ms() + 650L;
    }
}

static void send_round_done_if_ready(void) {
    if (!g_round_finished || g_round_done_sent || visualizer_has_pending_work()) return;
    g_round_done_sent = 1;

    if (g_ack_fd >= 0) {
        EventMsg ack;
        memset(&ack, 0, sizeof(ack));
        ack.magic = EVENT_MSG_MAGIC;
        ack.type = EVENT_LOG;
        ack.team_id = -1;
        ack.member_id = -1;
        ack.round_id = g_round;
        ack.furniture_count = g_furniture_count;
        ack.member_count = g_members;
        ack.wins[0] = g_wins[0];
        ack.wins[1] = g_wins[1];
        snprintf(ack.text, sizeof(ack.text), "VISUAL_ROUND_DONE round=%d winner=%d", g_round, g_winner_team);
        (void)write(g_ack_fd, &ack, sizeof(ack));
    }
}

static void update_animations(void) {
    if (g_paused) return;

    float now = visual_now_ms();
    for (int t = 0; t < TEAM_COUNT; t++) {
        if (g_anim[t].active) {
            float age = now - g_anim[t].start_ms;
            if (age >= g_anim[t].duration_ms) {
                g_anim[t].segment_index++;
                if (g_anim[t].segment_index >= g_anim[t].total_segments) {
                    VisualTx done = g_anim[t].tx;
                    g_anim[t].active = 0;
                    finish_transaction(t, &done);
                } else {
                    int last = g_anim[t].tx.member_count - 1;
                    if (last < 1) last = 1;
                    if (g_anim[t].tx.direction == DIR_BACKWARD &&
                        g_anim[t].segment_index == last) {
                        show_rejection_notice(t, &g_anim[t].tx);
                    }
                    setup_segment(&g_anim[t]);
                }
            }
        }
        start_next_anim_if_needed(t);
    }
    send_round_done_if_ready();
}

static int segment_active(int team_id, int from_member, int to_member) {
    if (team_id < 0 || team_id >= TEAM_COUNT) return 0;
    return g_anim[team_id].active &&
           g_anim[team_id].from_member == from_member &&
           g_anim[team_id].to_member == to_member;
}

static int member_active(int team_id, int member_id) {
    if (team_id < 0 || team_id >= TEAM_COUNT) return 0;
    return g_anim[team_id].active &&
           (g_anim[team_id].from_member == member_id ||
            g_anim[team_id].to_member == member_id);
}

static void draw_man_icon(float cx, float cy, float scale) {
    set_rgb(0.10f, 0.10f, 0.13f);
    draw_circle(cx, cy + 18.0f * scale, 7.5f * scale, 22);

    glLineWidth(2.5f);
    glBegin(GL_LINES);
    glVertex2f(cx, cy + 10.0f * scale); glVertex2f(cx, cy - 11.0f * scale);
    glVertex2f(cx - 14.0f * scale, cy + 2.0f * scale); glVertex2f(cx + 14.0f * scale, cy + 2.0f * scale);
    glVertex2f(cx, cy - 11.0f * scale); glVertex2f(cx - 12.0f * scale, cy - 25.0f * scale);
    glVertex2f(cx, cy - 11.0f * scale); glVertex2f(cx + 12.0f * scale, cy - 25.0f * scale);
    glEnd();
    glLineWidth(1.0f);
}

static void draw_member_box(int team_id, int member_id) {
    float cx = member_x(member_id);
    float cy = team_y(team_id);
    float w = box_w();
    float h = box_h();
    float x = cx - w / 2.0f;
    float y = cy - h / 2.0f;
    int active = member_active(team_id, member_id);
    const TeamTheme *theme = team_theme(team_id);

    if (active) {
        float pulse = 0.65f + 0.25f * sinf(visual_now_ms() * 0.010f);
        set_rgba(theme->accent_r, theme->accent_g, theme->accent_b, 0.26f * pulse);
        draw_rect(x - 7.0f, y - 7.0f, w + 14.0f, h + 14.0f);
        set_rgba(theme->main_r, theme->main_g, theme->main_b, 0.20f * pulse);
        draw_rect(x - 12.0f, y - 12.0f, w + 24.0f, h + 24.0f);
    }

    set_rgb(0.70f, 0.75f, 0.82f);
    draw_rect(x + 4.0f, y - 4.0f, w, h);

    if (member_id == 0) {
        draw_rect_gradient_vertical(x, y, w, h, 1.00f, 0.88f, 0.58f, 1.00f, 0.96f, 0.76f);
    } else if (member_id == g_members - 1) {
        draw_rect_gradient_vertical(x, y, w, h, 0.70f, 0.90f, 0.72f, 0.86f, 0.98f, 0.84f);
    } else {
        draw_rect_gradient_vertical(x, y, w, h,
                                    theme->pale_r - 0.04f, theme->pale_g - 0.04f, theme->pale_b - 0.04f,
                                    theme->pale_r, theme->pale_g, theme->pale_b);
    }

    if (active) set_rgb(theme->main_r, theme->main_g, theme->main_b);
    else set_rgb(0.12f, 0.18f, 0.26f);
    glLineWidth(active ? 3.6f : 2.0f);
    draw_rect_outline(x, y, w, h);
    glLineWidth(1.0f);

    float icon_scale = clampf(w / 70.0f, 0.55f, 1.0f);
    draw_man_icon(cx, cy + 3.0f, icon_scale);

    char label[32];
    if (member_id == 0) snprintf(label, sizeof(label), "Source");
    else if (member_id == g_members - 1) snprintf(label, sizeof(label), "Sink");
    else snprintf(label, sizeof(label), "M%d", member_id);

    set_rgb(0.05f, 0.05f, 0.08f);
    draw_text_small(x + 5.0f, y + h - 14.0f, label);
}

static void draw_source_pile(int team_id) {
    float cy = team_y(team_id);
    float base_x = 52.0f;
    set_rgb(0.08f, 0.11f, 0.16f);
    draw_text_small(base_x - 10.0f, cy + 66.0f, "Furniture pile");

    int remaining = g_furniture_count - g_progress[team_id];
    if (remaining < 0) remaining = 0;
    int boxes = remaining > 9 ? 9 : remaining;

    for (int i = 0; i < boxes; i++) {
        float x = base_x + (float)(i % 3) * 23.0f;
        float y = cy - 44.0f + (float)(i / 3) * 21.0f;
        draw_rect_gradient_vertical(x, y, 20.0f, 18.0f, 0.57f, 0.37f, 0.19f, 0.82f, 0.58f, 0.33f);
        set_rgb(0.30f, 0.18f, 0.09f);
        draw_rect_outline(x, y, 20.0f, 18.0f);
    }

    char buf[48];
    snprintf(buf, sizeof(buf), "left: %d", remaining);
    set_rgb(0.08f, 0.11f, 0.16f);
    draw_text_small(base_x - 4.0f, cy - 63.0f, buf);
}

static void draw_house(int team_id) {
    float cy = team_y(team_id);
    float x = 1070.0f;
    float y = cy - 36.0f;

    draw_rect_gradient_vertical(x, y, 72.0f, 58.0f, 0.80f, 0.88f, 0.95f, 0.96f, 0.98f, 1.00f);
    set_rgb(0.58f, 0.15f, 0.12f);
    glBegin(GL_TRIANGLES);
    glVertex2f(x - 8.0f, y + 58.0f);
    glVertex2f(x + 36.0f, y + 94.0f);
    glVertex2f(x + 80.0f, y + 58.0f);
    glEnd();
    set_rgb(0.18f, 0.20f, 0.24f);
    draw_rect_outline(x, y, 72.0f, 58.0f);

    set_rgb(0.42f, 0.24f, 0.13f);
    draw_rect(x + 30.0f, y, 16.0f, 31.0f);
    set_rgb(0.05f, 0.05f, 0.06f);
    draw_text_small(x + 13.0f, y - 16.0f, "House");

    int stack = g_progress[team_id] > 8 ? 8 : g_progress[team_id];
    for (int i = 0; i < stack; i++) {
        float bx = x + 7.0f + (float)(i % 4) * 14.0f;
        float by = y + 6.0f + (float)(i / 4) * 13.0f;
        draw_rect_gradient_vertical(bx, by, 12.0f, 10.0f, 0.56f, 0.36f, 0.18f, 0.78f, 0.54f, 0.30f);
        set_rgb(0.32f, 0.20f, 0.12f);
        draw_rect_outline(bx, by, 12.0f, 10.0f);
    }
}

static void draw_pipes(int team_id) {
    float cy = team_y(team_id);
    float top_y = cy + 28.0f;
    float bottom_y = cy - 28.0f;
    float w = box_w();
    const TeamTheme *theme = team_theme(team_id);

    for (int i = 0; i < g_members - 1; i++) {
        float x1 = member_x(i) + w / 2.0f + 4.0f;
        float x2 = member_x(i + 1) - w / 2.0f - 4.0f;

        if (segment_active(team_id, i, i + 1)) {
            draw_arrow_line(x1, top_y, x2, top_y, theme->main_r, theme->main_g, theme->main_b, 8.0f);
        } else {
            draw_arrow_line(x1, top_y, x2, top_y, 0.50f, 0.58f, 0.68f, 4.5f);
        }

        if (segment_active(team_id, i + 1, i)) {
            draw_arrow_line(x2, bottom_y, x1, bottom_y, 0.86f, 0.20f, 0.15f, 8.0f);
        } else {
            draw_arrow_line(x2, bottom_y, x1, bottom_y, 0.58f, 0.54f, 0.50f, 4.5f);
        }
    }
}

static void draw_progress_bar(int team_id) {
    float x = 150.0f;
    float y = team_y(team_id) - 108.0f;
    float w = 855.0f;
    float h = 18.0f;
    float ratio = g_furniture_count > 0 ? (float)g_progress[team_id] / (float)g_furniture_count : 0.0f;
    ratio = clampf(ratio, 0.0f, 1.0f);
    int pct = (int)(ratio * 100.0f + 0.5f);
    const TeamTheme *theme = team_theme(team_id);

    set_rgba(0.15f, 0.19f, 0.26f, 0.10f);
    draw_rect(x + 1.5f, y - 1.5f, w, h);
    draw_rect_gradient_vertical(x, y, w, h, 0.86f, 0.89f, 0.93f, 0.96f, 0.97f, 0.99f);
    draw_rect_gradient_vertical(x, y, w * ratio, h,
                                theme->main_r * 0.82f, theme->main_g * 0.82f, theme->main_b * 0.82f,
                                theme->accent_r, theme->accent_g, theme->accent_b);
    if (ratio > 0.02f) {
        set_rgba(1.00f, 1.00f, 1.00f, 0.18f);
        draw_rect(x + 2.0f, y + h - 5.0f, w * ratio - 4.0f, 2.0f);
    }
    set_rgba(0.30f, 0.36f, 0.44f, 0.45f);
    glLineWidth(1.0f);
    draw_rect_outline(x, y, w, h);
    glLineWidth(1.0f);

    char buf[128];
    snprintf(buf, sizeof(buf), "Progress: %d / %d (%d%%)", g_progress[team_id], g_furniture_count, pct);
    set_rgb(0.04f, 0.06f, 0.09f);
    draw_text_small(x + 8.0f, y + 4.0f, buf);
}

static void format_round_time(char *buf, size_t size) {
    float elapsed = g_round_start_ms > 0.0f ? (visual_now_ms() - g_round_start_ms) / 1000.0f : 0.0f;
    int minutes = (int)(elapsed / 60.0f);
    int seconds = (int)elapsed % 60;

    snprintf(buf, size, "%02d:%02d", minutes, seconds);
}

static void draw_stat_badge(float x, float y, float w, const char *label,
                            const char *value, const TeamTheme *theme) {
    set_rgba(0.15f, 0.19f, 0.25f, 0.08f);
    draw_rect(x + 1.5f, y - 1.5f, w, 28.0f);
    set_rgb(0.98f, 0.99f, 1.00f);
    draw_rect(x, y, w, 28.0f);
    set_rgba(0.34f, 0.40f, 0.48f, 0.24f);
    draw_rect_outline(x, y, w, 28.0f);
    set_rgb(theme->main_r, theme->main_g, theme->main_b);
    draw_rect(x, y, 4.0f, 28.0f);
    set_rgb(0.39f, 0.45f, 0.53f);
    draw_text_small(x + 10.0f, y + 16.0f, label);
    set_rgb(0.05f, 0.08f, 0.12f);
    draw_text_small(x + 10.0f, y + 5.0f, value);
}

static void draw_team_stats(int team_id) {
    float x = 150.0f;
    float y = team_y(team_id) - 84.0f;
    float w = 118.0f;
    int accepted = g_stat_accepted[team_id];
    int rejected = g_stat_rejected[team_id];
    int total = accepted + rejected;
    int efficiency = total > 0 ? (int)(((float)accepted / (float)total) * 100.0f + 0.5f) : 0;
    const TeamTheme *theme = team_theme(team_id);
    char value[32];

    snprintf(value, sizeof(value), "%d", accepted);
    draw_stat_badge(x, y, w, "Accepted", value, theme);
    snprintf(value, sizeof(value), "%d", rejected);
    draw_stat_badge(x + 128.0f, y, w, "Rejected", value, theme);
    snprintf(value, sizeof(value), "%d", total);
    draw_stat_badge(x + 256.0f, y, w, "Attempts", value, theme);
    snprintf(value, sizeof(value), "%d%%", efficiency);
    draw_stat_badge(x + 384.0f, y, w, "Efficiency", value, theme);
}

static void draw_pause_overlay(void) {
    if (!g_paused) return;

    set_rgba(0.02f, 0.04f, 0.07f, 0.34f);
    draw_rect(0.0f, 0.0f, WINDOW_W, WINDOW_H);
    set_rgba(0.04f, 0.08f, 0.14f, 0.88f);
    draw_rect(505.0f, 338.0f, 170.0f, 46.0f);
    set_rgb(1.00f, 1.00f, 1.00f);
    draw_text(548.0f, 355.0f, "PAUSED");
}

static void draw_status_chip(float x, float y, float w, const char *label, const char *value) {
    set_rgba(0.13f, 0.18f, 0.25f, 0.07f);
    draw_rect(x + 1.5f, y - 1.5f, w, 32.0f);
    set_rgb(0.98f, 0.99f, 1.00f);
    draw_rect(x, y, w, 32.0f);
    set_rgba(0.34f, 0.40f, 0.48f, 0.24f);
    draw_rect_outline(x, y, w, 32.0f);
    set_rgb(0.42f, 0.48f, 0.56f);
    draw_text_small(x + 9.0f, y + 18.0f, label);
    set_rgb(0.06f, 0.09f, 0.14f);
    draw_text_small(x + 9.0f, y + 6.0f, value);
}

static void draw_header(void) {
    char buf[128];
    char time_buf[32];

    set_rgba(1.00f, 1.00f, 1.00f, 0.86f);
    draw_rect(24.0f, 628.0f, 1132.0f, 70.0f);
    set_rgba(0.26f, 0.32f, 0.40f, 0.16f);
    draw_rect_outline(24.0f, 628.0f, 1132.0f, 70.0f);

    set_rgb(0.03f, 0.07f, 0.13f);
    draw_text_large(38.0f, 665.0f, "Home Furnishing Competition");
    set_rgb(0.38f, 0.45f, 0.54f);
    draw_text_small(40.0f, 646.0f, "Synchronized pipe visualization");

    snprintf(buf, sizeof(buf), "Round %d", g_round);
    draw_status_chip(405.0f, 650.0f, 92.0f, "Round", buf);

    snprintf(buf, sizeof(buf), "T1 %d  /  T2 %d", g_wins[0], g_wins[1]);
    draw_status_chip(510.0f, 650.0f, 128.0f, "Score", buf);

    snprintf(buf, sizeof(buf), "%d", g_furniture_count);
    draw_status_chip(650.0f, 650.0f, 112.0f, "Furniture", buf);

    snprintf(buf, sizeof(buf), "%d", g_members);
    draw_status_chip(775.0f, 650.0f, 104.0f, "Members", buf);

    format_round_time(time_buf, sizeof(time_buf));
    draw_status_chip(1015.0f, 650.0f, 118.0f, "Round time", time_buf);

    set_rgb(0.50f, 0.14f, 0.10f);
    draw_text_small(405.0f, 632.0f, g_phase);
}

static void draw_celebration(void) {
    long now = (long)visual_now_ms();
    if (g_celebrate_until <= now) return;

    float remaining = (float)(g_celebrate_until - now);
    float alpha = clampf(remaining / (float)CELEBRATION_MS, 0.0f, 1.0f);
    float pulse = 0.55f + 0.45f * sinf(visual_now_ms() * 0.018f);
    int team = (g_celebrate_team >= 0 && g_celebrate_team < TEAM_COUNT) ? g_celebrate_team : 0;
    const TeamTheme *theme = team_theme(team);

    set_rgba(theme->accent_r, theme->accent_g, theme->accent_b, 0.18f + 0.18f * pulse * alpha);
    glLineWidth(7.0f);
    draw_rect_outline(18.0f, 86.0f, 1144.0f, 575.0f);
    glLineWidth(1.0f);

    set_rgba(theme->main_r, theme->main_g, theme->main_b, 0.88f * alpha);
    draw_rect(395.0f, 624.0f, 390.0f, 34.0f);
    set_rgb(1.00f, 1.00f, 1.00f);
    if (g_celebrate_team >= 0) {
        char buf[80];
        snprintf(buf, sizeof(buf), "Team %d wins the round", display_team_id(g_celebrate_team));
        draw_text(492.0f, 632.0f, buf);
    } else {
        draw_text(478.0f, 632.0f, "Competition complete");
    }

    for (int i = 0; i < 24; i++) {
        float t = visual_now_ms() * 0.0015f + (float)i * 0.37f;
        float x = 70.0f + fmodf((float)(i * 47) + t * 120.0f, 1040.0f);
        float y = 628.0f - fmodf(t * (28.0f + (float)(i % 5) * 8.0f), 135.0f);
        if (i % 3 == 0) set_rgba(theme->accent_r, theme->accent_g, theme->accent_b, 0.85f * alpha);
        else if (i % 3 == 1) set_rgba(0.98f, 0.78f, 0.24f, 0.85f * alpha);
        else set_rgba(1.00f, 1.00f, 1.00f, 0.80f * alpha);
        draw_rect(x, y, 7.0f, 11.0f);
    }
}

static void draw_product_symbol(float x, float y, int serial_no, int piece_id, int direction) {
    float w = 74.0f;
    float h = 44.0f;

    set_rgba(0.12f, 0.13f, 0.16f, 0.24f);
    draw_rect(x - w / 2.0f + 4.0f, y - h / 2.0f - 4.0f, w, h);

    if (direction == DIR_BACKWARD) {
        draw_rect_gradient_vertical(x - w / 2.0f, y - h / 2.0f, w, h,
                                    0.83f, 0.24f, 0.18f, 1.00f, 0.58f, 0.48f);
    } else {
        draw_rect_gradient_vertical(x - w / 2.0f, y - h / 2.0f, w, h,
                                    0.68f, 0.42f, 0.20f, 0.96f, 0.72f, 0.42f);
    }

    set_rgb(0.40f, 0.25f, 0.12f);
    draw_rect_outline(x - w / 2.0f, y - h / 2.0f, w, h);
    glBegin(GL_LINES);
    glVertex2f(x - w / 2.0f, y + 1.0f); glVertex2f(x + w / 2.0f, y + 1.0f);
    glEnd();

    char label[40];
    set_rgb(0.06f, 0.05f, 0.04f);
    snprintf(label, sizeof(label), "Serial: %d", serial_no);
    draw_text_small_centered(x, y + 7.0f, label);

    snprintf(label, sizeof(label), "Piece: %d", piece_id);
    draw_text_small_centered(x, y - 11.0f, label);

    float dir = direction == DIR_BACKWARD ? -1.0f : 1.0f;
    set_rgb(0.10f, 0.10f, 0.12f);
    glBegin(GL_TRIANGLES);
    glVertex2f(x + dir * 34.0f, y + 18.0f);
    glVertex2f(x + dir * 25.0f, y + 22.0f);
    glVertex2f(x + dir * 25.0f, y + 14.0f);
    glEnd();
}

static void draw_rejection_notice(int team_id) {
    long now = (long)visual_now_ms();
    if (team_id < 0 || team_id >= TEAM_COUNT) return;
    if (g_reject_notice_until[team_id] <= now || !g_reject_notice[team_id][0]) return;

    float remaining = (float)(g_reject_notice_until[team_id] - now);
    float alpha = clampf(remaining / 1600.0f, 0.0f, 1.0f);
    float cx = member_x(g_members - 1);
    float cy = team_y(team_id);
    float w = 230.0f;
    float h = 30.0f;
    float x = clampf(cx - w / 2.0f, 780.0f, WINDOW_W - w - 28.0f);
    float y = cy + 49.0f;

    set_rgba(0.86f, 0.10f, 0.08f, 0.90f * alpha);
    draw_rect(x + 3.0f, y - 3.0f, w, h);
    set_rgba(1.00f, 0.93f, 0.90f, 0.96f * alpha);
    draw_rect(x, y, w, h);
    set_rgba(0.62f, 0.06f, 0.05f, alpha);
    draw_rect_outline(x, y, w, h);
    set_rgba(0.35f, 0.03f, 0.03f, alpha);
    draw_text_small_centered(x + w / 2.0f, y + 10.0f, g_reject_notice[team_id]);
}

static void draw_active_products(void) {
    float now = visual_now_ms();
    for (int t = 0; t < TEAM_COUNT; t++) {
        if (!g_anim[t].active) continue;

        ProductAnim *a = &g_anim[t];
        float age = now - a->start_ms;
        float p = clampf(age / a->duration_ms, 0.0f, 1.0f);
        p = p * p * (3.0f - 2.0f * p);

        float x1 = member_x(a->from_member);
        float x2 = member_x(a->to_member);
        float x = x1 + (x2 - x1) * p;
        float y = team_y(t) + (a->current_direction == DIR_FORWARD ? 28.0f : -28.0f);
        y += sinf(p * 3.14159265359f) * 6.0f;

        draw_product_symbol(x, y, a->tx.serial_no, a->tx.piece_id, a->current_direction);
    }
}

static void draw_team_lane(int team_id) {
    float cy = team_y(team_id);
    long now = (long)visual_now_ms();
    const TeamTheme *theme = team_theme(team_id);

    draw_rect_gradient_vertical(25.0f, cy - 126.0f, 1130.0f, 190.0f,
                                0.96f, 0.97f, 0.98f,
                                theme->lane_r, theme->lane_g, theme->lane_b);
    set_rgba(theme->main_r, theme->main_g, theme->main_b, 0.72f);
    draw_rect(25.0f, cy + 58.0f, 1130.0f, 4.0f);
    set_rgba(0.16f, 0.20f, 0.27f, 0.14f);
    draw_rect(25.0f, cy - 126.0f, 1130.0f, 1.0f);

    if (g_accept_flash_until[team_id] > now) {
        set_rgba(0.35f, 0.95f, 0.48f, 0.75f);
        draw_rect(25.0f, cy - 126.0f, 1130.0f, 8.0f);
    }
    if (g_reject_flash_until[team_id] > now) {
        set_rgba(1.00f, 0.30f, 0.24f, 0.70f);
        draw_rect(25.0f, cy - 118.0f, 1130.0f, 8.0f);
    }

    char title[128];
    snprintf(title, sizeof(title), "Team %d  |  wins: %d", display_team_id(team_id), g_wins[team_id]);
    set_rgb(theme->main_r, theme->main_g, theme->main_b);
    draw_text(40.0f, cy + 78.0f, title);

    draw_source_pile(team_id);
    draw_house(team_id);
    draw_pipes(team_id);

    for (int m = 0; m < g_members; m++) draw_member_box(team_id, m);

    draw_progress_bar(team_id);
    draw_team_stats(team_id);
    draw_rejection_notice(team_id);

    if (g_last_serial[team_id] >= 0) {
        char buf[96];
        snprintf(buf, sizeof(buf), "Last shown: serial %d / piece %d", g_last_serial[team_id], g_last_piece[team_id]);
        set_rgb(0.10f, 0.10f, 0.12f);
        draw_text_small(820.0f, cy + 78.0f, buf);
    }
}

static void consume_events(void) {
    if (g_fifo_fd < 0) return;

    while (1) {
        EventMsg ev;
        ssize_t n = read(g_fifo_fd, &ev, sizeof(ev));
        if (n == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) break;
            break;
        }
        if (n == 0) break;
        if (n != (ssize_t)sizeof(ev)) continue;
        if (ev.magic != EVENT_MSG_MAGIC) continue;

        if (ev.member_count >= 2 && ev.member_count <= MAX_MEMBERS) g_members = ev.member_count;
        if (ev.furniture_count > 0) g_furniture_count = ev.furniture_count;
        g_wins[0] = ev.wins[0];
        g_wins[1] = ev.wins[1];

        if (ev.type == EVENT_ROUND_START || ev.type == EVENT_TIE_BREAK) {
            reset_visual_round(ev.round_id);
            copy_display_text(g_phase, sizeof(g_phase), ev.text);
        } else if (ev.round_id > 0 && ev.round_id < g_round) {
            continue;
        } else if (ev.type == EVENT_VISUAL_RESULT && ev.round_id == g_round && ev.team_id >= 0 && ev.team_id < TEAM_COUNT) {
            /* DO NOT increment stats here - they are updated in finish_transaction()
            * when the carton actually reaches its destination on screen. This keeps
            * the stat panel synchronized with the animation. */

            /* If the round is already decided, ignore events from the LOSING team only.
            * Allow the WINNING team's events through so its progress bar updates. */
            if (g_round_finished && ev.team_id != g_winner_team) {
                continue;
            }

            VisualTx tx;
            memset(&tx, 0, sizeof(tx));
            tx.team_id = ev.team_id;
            tx.round_id = ev.round_id;
            tx.piece_id = ev.piece_id;
            tx.serial_no = ev.serial_no;
            tx.direction = ev.direction;
            tx.accepted_count = ev.accepted_count;
            tx.expected_serial = ev.expected_serial;
            tx.member_count = g_members;
            tx.touches = ev.touches;
            queue_push(ev.team_id, &tx);
            if (!g_paused) start_next_anim_if_needed(ev.team_id);
        } else if (ev.type == EVENT_ROUND_WIN && ev.round_id == g_round && ev.team_id >= 0 && ev.team_id < TEAM_COUNT) {
            /* The simulation says a team has won. We trust this only if the
             * visualizer has not already determined a winner via finish_transaction.
             * Either way, do NOT freeze any team here — the visual winner is
             * determined by which carton REACHES 100% on screen first
             * (handled in finish_transaction). This handler is just a backup. */
            if (!g_round_finished) {
                /* Visualizer hasn't seen the winning carton yet — accept what
                 * the simulation says, but don't kill the OTHER team's animation
                 * because it might still visually arrive first. Just record the
                 * pending winner; finish_transaction will freeze the loser. */
                snprintf(g_phase, sizeof(g_phase),
                         "Round %d in progress... waiting for winner to be visible", g_round);
            }
            /* If g_round_finished is already 1, finish_transaction already set
             * g_winner_team and froze the loser. We do nothing here. */
        } else if (ev.type == EVENT_ROUND_RESULT && ev.round_id == g_round) {
            /* Message shown during the 2-second pause between rounds. */
            copy_display_text(g_phase, sizeof(g_phase), ev.text);
        } else if (ev.type == EVENT_COMPETITION_END) {
            g_competition_finished = 1;
            copy_display_text(g_final_message, sizeof(g_final_message), ev.text);
            copy_display_text(g_phase, sizeof(g_phase), ev.text);
            if (g_wins[0] > g_wins[1]) start_celebration(0);
            else if (g_wins[1] > g_wins[0]) start_celebration(1);
            else start_celebration(-1);
        }
    }
}

static void display(void) {
    update_animations();

    glClear(GL_COLOR_BUFFER_BIT);
    glLoadIdentity();

    draw_rect_gradient_vertical(0.0f, 0.0f, WINDOW_W, WINDOW_H,
                                0.94f, 0.95f, 0.97f,
                                0.99f, 0.99f, 1.00f);

    draw_header();

    draw_team_lane(0);
    draw_team_lane(1);
    draw_active_products();
    draw_celebration();

    if (g_competition_finished && g_final_message[0]) {
        set_rgb(0.08f, 0.38f, 0.13f);
        draw_text(35.0f, 58.0f, "Competition result:");
        draw_text(230.0f, 58.0f, g_final_message);
    } else {
        set_rgb(0.04f, 0.05f, 0.08f);
        draw_text_small(35.0f, 45.0f, "Visualization-only controls: Space pauses/resumes animation.");
    }

    draw_pause_overlay();
    glutSwapBuffers();
}

static void timer_cb(int value) {
    (void)value;
    consume_events();
    update_animations();
    glutPostRedisplay();
    glutTimerFunc(16, timer_cb, 0);
}

static void reshape(int w, int h) {
    glViewport(0, 0, w, h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluOrtho2D(0.0, (double)WINDOW_W, 0.0, (double)WINDOW_H);
    glMatrixMode(GL_MODELVIEW);
}

static void keyboard_cb(unsigned char key, int x, int y) {
    (void)x;
    (void)y;
    if (key == 27 || key == 'q' || key == 'Q') {
        if (g_fifo_fd >= 0) close(g_fifo_fd);
        if (g_ack_fd >= 0) close(g_ack_fd);
        exit(0);
    }
    if (key == 'c' || key == 'C') {
        clear_team_motion(0);
        clear_team_motion(1);
    }
    if (key == ' ') {
        toggle_pause();
    }
}

static int open_master_fifo_for_reading(const char *fifo) {
    int printed_wait_message = 0;

    while (1) {
        int fd = open(fifo, O_RDONLY | O_NONBLOCK);
        if (fd >= 0) return fd;

        if (errno == ENOENT) {
            if (!printed_wait_message) {
                fprintf(stderr, "Waiting for master to create FIFO '%s'...\n", fifo);
                printed_wait_message = 1;
            }
            sleep(1);
            continue;
        }

        fprintf(stderr, "Could not open FIFO '%s': %s\n", fifo, strerror(errno));
        return -1;
    }
}

static int open_master_fifo_for_writing(const char *fifo) {
    int printed_wait_message = 0;

    while (1) {
        int fd = open(fifo, O_WRONLY | O_NONBLOCK);
        if (fd >= 0) return fd;

        if (errno == ENOENT || errno == ENXIO) {
            if (!printed_wait_message) {
                fprintf(stderr, "Waiting for master to open visual ACK FIFO '%s'...\n", fifo);
                printed_wait_message = 1;
            }
            sleep(1);
            continue;
        }

        fprintf(stderr, "Could not open ACK FIFO '%s': %s\n", fifo, strerror(errno));
        return -1;
    }
}

int main(int argc, char **argv) {
    const char *fifo = argc >= 2 ? argv[1] : "/tmp/home_furnishing_fifo";
    const char *ack_fifo = argc >= 3 ? argv[2] : NULL;

    g_fifo_fd = open_master_fifo_for_reading(fifo);
    if (g_fifo_fd == -1) return 1;

    if (ack_fifo) {
        g_ack_fd = open_master_fifo_for_writing(ack_fifo);
        if (g_ack_fd == -1) return 1;
    }

    snprintf(g_phase, sizeof(g_phase), "Connected to FIFO %s. Waiting for simulation events...", fifo);

    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGB);
    glutInitWindowSize(WINDOW_W, WINDOW_H);
    glutCreateWindow("Home Furnishing Competition - Synchronized Pipes and Products");

    glClearColor(0.97f, 0.98f, 1.00f, 1.0f);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glutDisplayFunc(display);
    glutReshapeFunc(reshape);
    glutKeyboardFunc(keyboard_cb);
    glutTimerFunc(16, timer_cb, 0);
    glutMainLoop();

    if (g_fifo_fd >= 0) close(g_fifo_fd);
    if (g_ack_fd >= 0) close(g_ack_fd);
    return 0;
}
