/**
 * @file main.c
 * @brief Minimal ubus test-service for HGW-Doctor integration tests.
 *
 * Registers "TestService" on ubus with:
 *   SetMode {"mode": "Idle"|"CPUStress"|"MemStress"}
 *   _get    {} → {"UptimeSeconds": N}
 *   _set    {"UptimeSeconds": N}
 *
 * Every 5 s the internal uptime counter increments and the new value is
 * written back via ubus call TestService _set (Fix 4).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>

#include <libubus.h>
#include <libubox/blobmsg_json.h>
#include <libubox/uloop.h>

typedef enum { MODE_IDLE = 0, MODE_CPU_STRESS, MODE_MEM_STRESS } ServiceMode;

static volatile ServiceMode  s_mode    = MODE_IDLE;
static uint32_t              s_uptime  = 0;
static void                 *s_mem_blk = NULL;
static struct ubus_context  *s_ctx     = NULL;
static struct ubus_object    s_obj;
static struct uloop_timeout  s_tick;

/* ── CPU stress ───────────────────────────────────────────────────── */
static void *cpu_stress_fn(void *arg) {
    (void)arg;
    volatile unsigned long x = 0;
    while (s_mode == MODE_CPU_STRESS)
        x++;
    return NULL;
}

/* ── SetMode handler ──────────────────────────────────────────────── */
enum { SM_MODE, __SM_MAX };
static const struct blobmsg_policy sm_policy[] = {
    [SM_MODE] = { "mode", BLOBMSG_TYPE_STRING },
};

static int handle_set_mode(struct ubus_context *ctx, struct ubus_object *obj,
                           struct ubus_request_data *req,
                           const char *method, struct blob_attr *msg) {
    (void)obj; (void)method;
    struct blob_attr *tb[__SM_MAX];
    blobmsg_parse(sm_policy, __SM_MAX, tb, blob_data(msg), blob_len(msg));
    const char *m = tb[SM_MODE] ? blobmsg_get_string(tb[SM_MODE]) : "Idle";

    if (strcmp(m, "CPUStress") == 0) {
        s_mode = MODE_CPU_STRESS;
        int ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
        for (int i = 0; i < ncpu; i++) {
            pthread_t t;
            pthread_create(&t, NULL, cpu_stress_fn, NULL);
            pthread_detach(t);
        }
    } else if (strcmp(m, "MemStress") == 0) {
        s_mode = MODE_MEM_STRESS;
        free(s_mem_blk);
        s_mem_blk = malloc(600UL * 1024 * 1024);
        if (s_mem_blk)
            memset(s_mem_blk, 0xAA, 600UL * 1024 * 1024);
    } else {
        s_mode = MODE_IDLE;
        free(s_mem_blk);
        s_mem_blk = NULL;
    }

    struct blob_buf b = {};
    blob_buf_init(&b, 0);
    ubus_send_reply(ctx, req, b.head);
    blob_buf_free(&b);
    return 0;
}

/* ── _get handler ─────────────────────────────────────────────────── */
static int handle_get(struct ubus_context *ctx, struct ubus_object *obj,
                      struct ubus_request_data *req,
                      const char *method, struct blob_attr *msg) {
    (void)obj; (void)method; (void)msg;
    struct blob_buf b = {};
    blob_buf_init(&b, 0);
    blobmsg_add_u32(&b, "UptimeSeconds", s_uptime);
    ubus_send_reply(ctx, req, b.head);
    blob_buf_free(&b);
    return 0;
}

/* ── _set handler ─────────────────────────────────────────────────── */
enum { SS_UPTIME, __SS_MAX };
static const struct blobmsg_policy ss_policy[] = {
    [SS_UPTIME] = { "UptimeSeconds", BLOBMSG_TYPE_INT32 },
};

static int handle_set(struct ubus_context *ctx, struct ubus_object *obj,
                      struct ubus_request_data *req,
                      const char *method, struct blob_attr *msg) {
    (void)obj; (void)method;
    struct blob_attr *tb[__SS_MAX];
    blobmsg_parse(ss_policy, __SS_MAX, tb, blob_data(msg), blob_len(msg));
    if (tb[SS_UPTIME])
        s_uptime = (uint32_t)blobmsg_get_u32(tb[SS_UPTIME]);
    struct blob_buf b = {};
    blob_buf_init(&b, 0);
    ubus_send_reply(ctx, req, b.head);
    blob_buf_free(&b);
    return 0;
}

/* ── ubus object ──────────────────────────────────────────────────── */
static const struct ubus_method ts_methods[] = {
    UBUS_METHOD("SetMode", handle_set_mode, sm_policy),
    UBUS_METHOD_NOARG("_get", handle_get),
    UBUS_METHOD("_set",    handle_set, ss_policy),
};
static struct ubus_object_type ts_type =
    UBUS_OBJECT_TYPE("TestService", ts_methods);

/* ── Uptime tick (every 5 s) ──────────────────────────────────────── */
static void uptime_tick(struct uloop_timeout *t) {
    s_uptime += 5;

    struct blob_buf b = {};
    blob_buf_init(&b, 0);
    blobmsg_add_u32(&b, "UptimeSeconds", s_uptime);
    ubus_invoke(s_ctx, s_obj.id, "_set", b.head, NULL, NULL, 500);
    blob_buf_free(&b);

    uloop_timeout_set(t, 5000);
}

/* ── main ─────────────────────────────────────────────────────────── */
int main(void) {
    signal(SIGTERM, (void (*)(int))uloop_end);
    signal(SIGINT,  (void (*)(int))uloop_end);

    uloop_init();

    s_ctx = ubus_connect(NULL);
    if (!s_ctx) {
        fprintf(stderr, "test-service: ubus connect failed\n");
        return 1;
    }
    ubus_add_uloop(s_ctx);

    s_obj.name      = "TestService";
    s_obj.type      = &ts_type;
    s_obj.methods   = ts_methods;
    s_obj.n_methods = ARRAY_SIZE(ts_methods);

    if (ubus_add_object(s_ctx, &s_obj) != 0) {
        fprintf(stderr, "test-service: ubus_add_object failed\n");
        ubus_free(s_ctx);
        return 1;
    }

    fprintf(stdout, "test-service: ready (pid=%d)\n", getpid());
    fflush(stdout);

    s_tick.cb = uptime_tick;
    uloop_timeout_set(&s_tick, 5000);

    uloop_run();

    free(s_mem_blk);
    ubus_free(s_ctx);
    uloop_done();
    return 0;
}
