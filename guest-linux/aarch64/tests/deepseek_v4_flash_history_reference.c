#define _POSIX_C_SOURCE 200809L
#include "ds4.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Link against an unchanged DS4 CPU build. Every position is evaluated by
 * the full model; the public payload API owns checkpoint serialization. */
static int number(const char *text, int limit) {
    char *end = NULL;
    errno = 0;
    const long value = strtol(text, &end, 10);
    return !errno && end != text && !*end && value >= 0 && value < limit
               ? (int)value : -1;
}

static double seconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return 0;
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

int main(int argc, char **argv) {
    if (argc != 6 || !strcmp(argv[1], "--help")) {
        fprintf(stderr, "usage: %s MODEL POSITIONS INITIAL_TOKEN PAYLOAD TRACE\n",
                argv[0]);
        return argc == 2 && !strcmp(argv[1], "--help") ? 0 : 2;
    }
    const int positions = number(argv[2], 1048576);
    int token = number(argv[3], 129280);
    if (positions <= 0 || token < 0) return 2;

    FILE *payload = fopen(argv[4], "wbx");
    if (!payload) {
        perror("open new payload");
        return 1;
    }
    FILE *trace = fopen(argv[5], "wx");
    if (!trace) {
        perror("open new trace");
        fclose(payload);
        remove(argv[4]);
        return 1;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(trace, NULL, _IOLBF, 0);
    ds4_engine_options options = {0};
    options.model_path = argv[1];
    options.backend = DS4_BACKEND_CPU;
    options.n_threads = 12;
    options.context_size = positions + 1;
    options.mtp_draft_tokens = 1;
    options.mtp_margin = 3.0f;
    ds4_engine *engine = NULL;
    ds4_session *session = NULL;
    float *logits = NULL;
    char error[256] = {0};
    int result = 1;
    const double started = seconds();
    if (ds4_engine_open(&engine, &options) || !engine ||
        ds4_session_create(&session, engine, positions + 1) || !session)
        goto done;
    logits = malloc(129280 * sizeof(*logits));
    if (!logits) goto done;
    for (int position = 0; position < positions; ++position) {
        if (ds4_session_eval(session, token, error, sizeof(error)) ||
            ds4_session_pos(session) != position + 1 ||
            ds4_session_copy_logits(session, logits, 129280) != 129280)
            goto done;
        int next = 0;
        for (int candidate = 0; candidate < 129280; ++candidate) {
            if (!isfinite(logits[candidate])) {
                snprintf(error, sizeof(error), "nonfinite logit at %d", position);
                goto done;
            }
            if (logits[candidate] > logits[next]) next = candidate;
        }
        if (fprintf(trace,
                    "{\"position\":%d,\"input\":%d,\"next\":%d,"
                    "\"elapsed_s\":%.6f}\n",
                    position, token, next, seconds() - started) < 0)
            goto done;
        token = next;
        if (position < 4 || (position + 1) % 32 == 0 ||
            position + 1 == positions)
            printf("real_history positions=%d/%d next=%d elapsed_s=%.3f\n",
                   position + 1, positions, token, seconds() - started);
    }
    if (ds4_session_save_payload(session, payload, error, sizeof(error)))
        goto done;
    result = 0;
done:
    if (fclose(trace)) result = 1;
    if (fclose(payload)) result = 1;
    if (result) {
        fprintf(stderr, "real_history=fail reason=%s\n", error);
        remove(argv[4]);
    } else {
        printf("real_history=pass positions=%d next=%d elapsed_s=%.3f\n",
               positions, token, seconds() - started);
    }
    free(logits);
    if (session) ds4_session_free(session);
    if (engine) ds4_engine_close(engine);
    return result;
}
