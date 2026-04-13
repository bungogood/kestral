/*
 * ubgi_engine.c -- Minimal UBGI engine for kestrel C evaluator.
 *
 * Supports:
 *   ubgi, isready, newgame, position gnubgid <id>, dice <d1> <d2>,
 *   go role chequer, quit
 *
 * Model is optional:
 *   --model /path/to/model.bin
 * If no model is provided, engine picks the first legal move.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bg_engine.h"
#include "nn_eval.h"

#define MAX_LINE 512
#define MAX_MOVE_TEXT 128

typedef struct {
    BoardState state;
    int has_position;
    int d1;
    int d2;
    int has_dice;
    int use_model;
    NNModel model;
} EngineCtx;

static void reply_line(const char *s) {
    puts(s);
    fflush(stdout);
}

static int starts_with(const char *s, const char *prefix) {
    size_t n = strlen(prefix);
    return strncmp(s, prefix, n) == 0;
}

static void trim_trailing(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || isspace((unsigned char)s[n - 1]))) {
        s[n - 1] = '\0';
        n--;
    }
}

static int b64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Decode 14-char GNUbg position id into 10-byte key. */
static int decode_gnuid14(const char *id, unsigned char key[10]) {
    int vals[14];
    int i;
    unsigned int bits = 0;
    int bit_count = 0;
    int out = 0;

    if ((int)strlen(id) != 14) return -1;

    for (i = 0; i < 14; i++) {
        int v = b64_value(id[i]);
        if (v < 0) return -1;
        vals[i] = v;
    }

    memset(key, 0, 10);
    for (i = 0; i < 14; i++) {
        bits = (bits << 6) | (unsigned int)vals[i];
        bit_count += 6;
        while (bit_count >= 8 && out < 10) {
            bit_count -= 8;
            key[out++] = (unsigned char)((bits >> bit_count) & 0xFFu);
        }
    }

    return (out == 10) ? 0 : -1;
}

static int key_bit(const unsigned char key[10], int bit_index) {
    return (key[bit_index / 8] >> (bit_index % 8)) & 1;
}

/*
 * Decode GNUbg position id into our BoardState in "on-roll is WHITE" form.
 * This matches UBGI usage where gnubgid is normalized to side-to-roll.
 */
static int board_from_gnuid(BoardState *state, const char *id) {
    unsigned char key[10];
    int pips[26];
    int bit_index = 0;
    int point;
    int x_pieces = 0;
    int o_pieces = 0;

    if (decode_gnuid14(id, key) != 0) return -1;

    memset(pips, 0, sizeof(pips));

    /* Opponent (negative), points 24..1, then opponent bar (0). */
    for (point = 24; point >= 1; point--) {
        while (key_bit(key, bit_index) == 1) {
            pips[point] -= 1;
            o_pieces++;
            bit_index++;
        }
        bit_index++;
    }
    while (key_bit(key, bit_index) == 1) {
        pips[0] -= 1;
        o_pieces++;
        bit_index++;
    }
    bit_index++;

    /* On-roll player (positive), points 1..24, then own bar (25). */
    for (point = 1; point <= 24; point++) {
        while (key_bit(key, bit_index) == 1) {
            pips[point] += 1;
            x_pieces++;
            bit_index++;
        }
        bit_index++;
    }
    while (bit_index < 80 && key_bit(key, bit_index) == 1) {
        pips[25] += 1;
        x_pieces++;
        bit_index++;
    }

    memset(state, 0, sizeof(*state));
    for (point = 1; point <= 24; point++) {
        state->points[point - 1] = pips[point];
    }

    state->bar[WHITE] = pips[25] > 0 ? pips[25] : 0;
    state->bar[BLACK] = pips[0] < 0 ? -pips[0] : 0;

    state->off[WHITE] = NUM_CHECKERS - x_pieces;
    state->off[BLACK] = NUM_CHECKERS - o_pieces;
    state->turn = WHITE;

    if (state->off[WHITE] < 0 || state->off[BLACK] < 0) return -1;
    return 0;
}

static void play_to_text(const Play *play, char *out, size_t out_size) {
    size_t used = 0;
    int i;

    if (out_size == 0) return;
    out[0] = '\0';

    for (i = 0; i < play->num_moves; i++) {
        const Move *m = &play->moves[i];
        char src[8];
        char dst[8];
        int written;

        if (m->src == BAR_SENTINEL) {
            strcpy(src, "bar");
        } else {
            snprintf(src, sizeof(src), "%d", m->src + 1);
        }

        if (m->dst == OFF_SENTINEL) {
            strcpy(dst, "off");
        } else {
            snprintf(dst, sizeof(dst), "%d", m->dst + 1);
        }

        written = snprintf(out + used, out_size - used,
                           "%s%s/%s", (i == 0) ? "" : " ", src, dst);
        if (written < 0) return;
        if ((size_t)written >= out_size - used) {
            out[out_size - 1] = '\0';
            return;
        }
        used += (size_t)written;
    }
}

static int handle_go(EngineCtx *ctx, const char *cmd) {
    Play plays[MAX_PLAYS];
    static float encoded[MAX_PLAYS * NUM_FEATURES];
    int count;
    int best;
    int i;
    char move_text[MAX_MOVE_TEXT];
    char line[MAX_MOVE_TEXT + 16];

    if (strstr(cmd, "role cube") || strstr(cmd, "role turn")) {
        reply_line("error unsupported_feature role");
        return 0;
    }

    if (!ctx->has_position) {
        reply_line("error missing_context position");
        return 0;
    }
    if (!ctx->has_dice) {
        reply_line("error missing_context dice");
        return 0;
    }

    if (ctx->use_model) {
        count = get_legal_plays_encoded(&ctx->state, ctx->d1, ctx->d2,
                                        plays, MAX_PLAYS, encoded);
    } else {
        count = get_legal_plays(&ctx->state, ctx->d1, ctx->d2, plays, MAX_PLAYS);
    }

    if (count == 0) {
        reply_line("bestmove pass");
        return 0;
    }

    if (count < 0) {
        reply_line("error internal movegen_failed");
        return 0;
    }

    best = 0;
    if (ctx->use_model) {
        float best_v = 2.0f;
        for (i = 0; i < count; i++) {
            float v = nn_forward(&ctx->model, encoded + (i * NUM_FEATURES));
            if (v < best_v) {
                best_v = v;
                best = i;
            }
        }
    }

    play_to_text(&plays[best], move_text, sizeof(move_text));
    snprintf(line, sizeof(line), "bestmove %s", move_text);
    reply_line(line);
    return 0;
}

int main(int argc, char **argv) {
    EngineCtx ctx;
    char line[MAX_LINE];
    int i;

    memset(&ctx, 0, sizeof(ctx));

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--model") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "missing value for --model\n");
                return 2;
            }
            if (nn_load(&ctx.model, argv[i + 1]) != 0) {
                fprintf(stderr, "failed to load model: %s\n", argv[i + 1]);
                return 2;
            }
            ctx.use_model = 1;
            i++;
        } else {
            fprintf(stderr, "unknown arg: %s\n", argv[i]);
            return 2;
        }
    }

    while (fgets(line, sizeof(line), stdin) != NULL) {
        char *cmd = line;
        trim_trailing(cmd);

        while (*cmd && isspace((unsigned char)*cmd)) cmd++;
        if (*cmd == '\0') continue;

        if (strcmp(cmd, "ubgi") == 0) {
            reply_line("id name kestrel-c-ubgi 0.1");
            reply_line("id author kestrel");
            reply_line("option name ModelLoaded type check default false");
            reply_line("ubgiok");
            continue;
        }

        if (strcmp(cmd, "isready") == 0) {
            reply_line("readyok");
            continue;
        }

        if (strcmp(cmd, "newgame") == 0) {
            ctx.has_position = 0;
            ctx.has_dice = 0;
            continue;
        }

        if (strcmp(cmd, "quit") == 0) {
            break;
        }

        if (starts_with(cmd, "setoption name ")) {
            continue;
        }

        if (starts_with(cmd, "position gnubgid ")) {
            char id[32];
            const char *id_src = cmd + strlen("position gnubgid ");
            int n = 0;
            while (id_src[n] && !isspace((unsigned char)id_src[n]) && n < (int)sizeof(id) - 1) {
                id[n] = id_src[n];
                n++;
            }
            id[n] = '\0';

            if (board_from_gnuid(&ctx.state, id) != 0) {
                reply_line("error bad_argument invalid_position");
            } else {
                ctx.has_position = 1;
            }
            continue;
        }

        if (strcmp(cmd, "position xgid") == 0 || starts_with(cmd, "position xgid ")) {
            reply_line("error unsupported_feature position_xgid");
            continue;
        }

        if (starts_with(cmd, "dice ")) {
            int d1, d2;
            if (sscanf(cmd + 5, "%d %d", &d1, &d2) != 2 ||
                d1 < 1 || d1 > 6 || d2 < 1 || d2 > 6) {
                reply_line("error bad_argument dice");
            } else {
                ctx.d1 = d1;
                ctx.d2 = d2;
                ctx.has_dice = 1;
            }
            continue;
        }

        if (strcmp(cmd, "go") == 0 || starts_with(cmd, "go ")) {
            handle_go(&ctx, cmd);
            continue;
        }

        reply_line("error unknown_command");
    }

    if (ctx.use_model) {
        nn_free(&ctx.model);
    }
    return 0;
}
