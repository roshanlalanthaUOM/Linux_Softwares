/*
 * xchess - a complete chess game for Linux in a single C file.
 *
 * Dependencies: libX11 and libm only (both are on essentially every Linux
 * desktop, and both are on the AppImage excludelist, so nothing needs to be
 * bundled). Pieces are drawn as vector shapes, so there are no asset files.
 *
 * Build:  gcc -O2 -o xchess chess.c -lX11 -lm
 *
 * Controls:
 *   left click   select a piece / move it
 *   n            new game
 *   u            take back a move
 *   f            flip board and swap sides with the engine
 *   a            toggle the engine on/off (two-player mode)
 *   1-5          engine strength (search depth)
 *   q or Esc     quit
 */

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* Board representation                                               */
/* ------------------------------------------------------------------ */
/* Square 0 = a1, 7 = h1, 56 = a8, 63 = h8.
 * Pieces: positive = white, negative = black.                        */

enum { EMPTY = 0, PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING };

#define WK_CASTLE 1
#define WQ_CASTLE 2
#define BK_CASTLE 4
#define BQ_CASTLE 8

typedef struct {
    int b[64];
    int side;    /* 1 = white to move, -1 = black to move */
    int castle;  /* bit mask of the four castling rights  */
    int ep;      /* en-passant target square, or -1       */
    int half;    /* halfmove clock for the 50-move rule   */
} Pos;

#define F_CAP    1
#define F_EP     2
#define F_CASTLE 4
#define F_DPP    8   /* double pawn push */
#define F_PROMO  16

typedef struct {
    unsigned char from, to, promo, flags;
} Move;

typedef struct {
    int cap, castle, ep, half;
} Undo;

static int fileof(int s) { return s & 7; }
static int rankof(int s) { return s >> 3; }
static int sgn(int v)    { return v > 0 ? 1 : (v < 0 ? -1 : 0); }
static int piece(int v)  { return v < 0 ? -v : v; }
static int okfr(int f, int r) { return f >= 0 && f < 8 && r >= 0 && r < 8; }

static const int DIR8[8][2] = {
    { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 },      /* rook directions   */
    { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 }     /* bishop directions */
};
static const int NDIR[8][2] = {
    { 1, 2 }, { 2, 1 }, { 2, -1 }, { 1, -2 },
    { -1, -2 }, { -2, -1 }, { -2, 1 }, { -1, 2 }
};

static int king_square(const Pos *p, int side);

/* Loads a FEN string. Returns 0 if it looks malformed. */
static int set_fen(Pos *p, const char *s)
{
    int sq = 56, v;
    char c;

    memset(p->b, 0, sizeof(p->b));
    p->ep = -1;
    p->half = 0;
    p->castle = 0;
    p->side = 1;

    while (*s && *s != ' ') {
        c = *s++;
        if (c == '/') {
            sq -= 16;
        } else if (c >= '1' && c <= '8') {
            sq += c - '0';
        } else {
            int white = (c >= 'A' && c <= 'Z');
            char l = white ? (char)(c + 32) : c;
            switch (l) {
            case 'p': v = PAWN;   break;
            case 'n': v = KNIGHT; break;
            case 'b': v = BISHOP; break;
            case 'r': v = ROOK;   break;
            case 'q': v = QUEEN;  break;
            case 'k': v = KING;   break;
            default:  return 0;
            }
            if (sq < 0 || sq > 63)
                return 0;
            p->b[sq++] = white ? v : -v;
        }
    }
    if (*s == ' ')
        s++;
    if (*s == 'b')
        p->side = -1;
    while (*s && *s != ' ')
        s++;
    if (*s == ' ')
        s++;
    while (*s && *s != ' ') {
        if (*s == 'K') p->castle |= WK_CASTLE;
        if (*s == 'Q') p->castle |= WQ_CASTLE;
        if (*s == 'k') p->castle |= BK_CASTLE;
        if (*s == 'q') p->castle |= BQ_CASTLE;
        s++;
    }
    if (*s == ' ')
        s++;
    if (*s && *s != '-' && s[1])
        p->ep = (s[1] - '1') * 8 + (s[0] - 'a');
    return king_square(p, 1) >= 0 && king_square(p, -1) >= 0;
}

static void set_start(Pos *p)
{
    static const int back[8] = { ROOK, KNIGHT, BISHOP, QUEEN,
                                 KING, BISHOP, KNIGHT, ROOK };
    int i;
    memset(p->b, 0, sizeof(p->b));
    for (i = 0; i < 8; i++) {
        p->b[i]      =  back[i];
        p->b[8 + i]  =  PAWN;
        p->b[48 + i] = -PAWN;
        p->b[56 + i] = -back[i];
    }
    p->side = 1;
    p->castle = WK_CASTLE | WQ_CASTLE | BK_CASTLE | BQ_CASTLE;
    p->ep = -1;
    p->half = 0;
}

/* ------------------------------------------------------------------ */
/* Attack detection                                                   */
/* ------------------------------------------------------------------ */

static int is_attacked(const Pos *p, int sq, int by)
{
    int f = fileof(sq), r = rankof(sq), i, d, nf, nr, t;

    /* pawns: a pawn of colour "by" sitting one rank "behind" sq */
    nr = r - by;
    if (nr >= 0 && nr < 8) {
        for (d = -1; d <= 1; d += 2) {
            nf = f + d;
            if (nf >= 0 && nf < 8 && p->b[nr * 8 + nf] == PAWN * by)
                return 1;
        }
    }
    /* knights */
    for (i = 0; i < 8; i++) {
        nf = f + NDIR[i][0];
        nr = r + NDIR[i][1];
        if (okfr(nf, nr) && p->b[nr * 8 + nf] == KNIGHT * by)
            return 1;
    }
    /* king */
    for (i = 0; i < 8; i++) {
        nf = f + DIR8[i][0];
        nr = r + DIR8[i][1];
        if (okfr(nf, nr) && p->b[nr * 8 + nf] == KING * by)
            return 1;
    }
    /* sliding pieces */
    for (i = 0; i < 8; i++) {
        nf = f;
        nr = r;
        for (;;) {
            nf += DIR8[i][0];
            nr += DIR8[i][1];
            if (!okfr(nf, nr))
                break;
            t = p->b[nr * 8 + nf];
            if (t) {
                if (sgn(t) == by) {
                    int pt = piece(t);
                    if (pt == QUEEN)
                        return 1;
                    if (i < 4 && pt == ROOK)
                        return 1;
                    if (i >= 4 && pt == BISHOP)
                        return 1;
                }
                break;
            }
        }
    }
    return 0;
}

static int king_square(const Pos *p, int side)
{
    int i;
    for (i = 0; i < 64; i++)
        if (p->b[i] == KING * side)
            return i;
    return -1;
}

static int in_check(const Pos *p, int side)
{
    int k = king_square(p, side);
    return k >= 0 && is_attacked(p, k, -side);
}

/* ------------------------------------------------------------------ */
/* Move generation                                                    */
/* ------------------------------------------------------------------ */

static void add(Move *list, int *n, int from, int to, int promo, int flags)
{
    Move *m = &list[(*n)++];
    m->from = (unsigned char)from;
    m->to = (unsigned char)to;
    m->promo = (unsigned char)promo;
    m->flags = (unsigned char)flags;
}

static void add_pawn(Move *list, int *n, int from, int to, int flags, int side)
{
    int last = side > 0 ? 7 : 0;
    if (rankof(to) == last) {
        int pr;
        for (pr = QUEEN; pr >= KNIGHT; pr--)
            add(list, n, from, to, pr, flags | F_PROMO);
    } else {
        add(list, n, from, to, 0, flags);
    }
}

/* Generates pseudo-legal moves; legality is checked by make/unmake. */
static int gen_moves(const Pos *p, Move *list)
{
    int n = 0, s, i, f, r, nf, nr, t, pc, side = p->side;

    for (s = 0; s < 64; s++) {
        pc = p->b[s];
        if (!pc || sgn(pc) != side)
            continue;
        f = fileof(s);
        r = rankof(s);

        switch (piece(pc)) {
        case PAWN: {
            int one = s + 8 * side;
            if (one >= 0 && one < 64 && !p->b[one]) {
                add_pawn(list, &n, s, one, 0, side);
                if (r == (side > 0 ? 1 : 6) && !p->b[s + 16 * side])
                    add(list, &n, s, s + 16 * side, 0, F_DPP);
            }
            for (i = -1; i <= 1; i += 2) {
                nf = f + i;
                nr = r + side;
                if (!okfr(nf, nr))
                    continue;
                t = nr * 8 + nf;
                if (p->b[t] && sgn(p->b[t]) != side)
                    add_pawn(list, &n, s, t, F_CAP, side);
                else if (!p->b[t] && t == p->ep)
                    add(list, &n, s, t, 0, F_CAP | F_EP);
            }
            break;
        }
        case KNIGHT:
            for (i = 0; i < 8; i++) {
                nf = f + NDIR[i][0];
                nr = r + NDIR[i][1];
                if (!okfr(nf, nr))
                    continue;
                t = nr * 8 + nf;
                if (!p->b[t])
                    add(list, &n, s, t, 0, 0);
                else if (sgn(p->b[t]) != side)
                    add(list, &n, s, t, 0, F_CAP);
            }
            break;
        case KING:
            for (i = 0; i < 8; i++) {
                nf = f + DIR8[i][0];
                nr = r + DIR8[i][1];
                if (!okfr(nf, nr))
                    continue;
                t = nr * 8 + nf;
                if (!p->b[t])
                    add(list, &n, s, t, 0, 0);
                else if (sgn(p->b[t]) != side)
                    add(list, &n, s, t, 0, F_CAP);
            }
            {
                int home = side > 0 ? 0 : 56;
                int kr = side > 0 ? WK_CASTLE : BK_CASTLE;
                int qr = side > 0 ? WQ_CASTLE : BQ_CASTLE;
                if (s == home + 4 && !is_attacked(p, s, -side)) {
                    if ((p->castle & kr) &&
                        !p->b[home + 5] && !p->b[home + 6] &&
                        p->b[home + 7] == ROOK * side &&
                        !is_attacked(p, home + 5, -side) &&
                        !is_attacked(p, home + 6, -side))
                        add(list, &n, s, home + 6, 0, F_CASTLE);
                    if ((p->castle & qr) &&
                        !p->b[home + 1] && !p->b[home + 2] && !p->b[home + 3] &&
                        p->b[home] == ROOK * side &&
                        !is_attacked(p, home + 3, -side) &&
                        !is_attacked(p, home + 2, -side))
                        add(list, &n, s, home + 2, 0, F_CASTLE);
                }
            }
            break;
        default: {
            int d0 = (piece(pc) == BISHOP) ? 4 : 0;
            int d1 = (piece(pc) == ROOK) ? 4 : 8;
            for (i = d0; i < d1; i++) {
                nf = f;
                nr = r;
                for (;;) {
                    nf += DIR8[i][0];
                    nr += DIR8[i][1];
                    if (!okfr(nf, nr))
                        break;
                    t = nr * 8 + nf;
                    if (!p->b[t]) {
                        add(list, &n, s, t, 0, 0);
                    } else {
                        if (sgn(p->b[t]) != side)
                            add(list, &n, s, t, 0, F_CAP);
                        break;
                    }
                }
            }
            break;
        }
        }
    }
    return n;
}

static void make_move(Pos *p, Move m, Undo *u)
{
    int from = m.from, to = m.to, side = p->side, pc = p->b[from];

    u->castle = p->castle;
    u->ep = p->ep;
    u->half = p->half;

    if (m.flags & F_EP) {
        int cs = to - 8 * side;
        u->cap = p->b[cs];
        p->b[cs] = 0;
    } else {
        u->cap = p->b[to];
    }

    p->b[to] = (m.flags & F_PROMO) ? m.promo * side : pc;
    p->b[from] = 0;

    if (m.flags & F_CASTLE) {
        if (to > from) {                 /* king side  */
            p->b[from + 1] = p->b[from + 3];
            p->b[from + 3] = 0;
        } else {                         /* queen side */
            p->b[from - 1] = p->b[from - 4];
            p->b[from - 4] = 0;
        }
    }

    if (piece(pc) == KING)
        p->castle &= (side > 0) ? ~(WK_CASTLE | WQ_CASTLE)
                                : ~(BK_CASTLE | BQ_CASTLE);
    if (from == 0  || to == 0)  p->castle &= ~WQ_CASTLE;
    if (from == 7  || to == 7)  p->castle &= ~WK_CASTLE;
    if (from == 56 || to == 56) p->castle &= ~BQ_CASTLE;
    if (from == 63 || to == 63) p->castle &= ~BK_CASTLE;

    p->ep = (m.flags & F_DPP) ? from + 8 * side : -1;
    p->half = (piece(pc) == PAWN || u->cap) ? 0 : p->half + 1;
    p->side = -side;
}

static void unmake_move(Pos *p, Move m, Undo *u)
{
    int from = m.from, to = m.to, side, pc;

    p->side = -p->side;
    side = p->side;
    pc = (m.flags & F_PROMO) ? PAWN * side : p->b[to];

    p->b[from] = pc;
    if (m.flags & F_EP) {
        p->b[to] = 0;
        p->b[to - 8 * side] = u->cap;
    } else {
        p->b[to] = u->cap;
    }

    if (m.flags & F_CASTLE) {
        if (to > from) {
            p->b[from + 3] = p->b[from + 1];
            p->b[from + 1] = 0;
        } else {
            p->b[from - 4] = p->b[from - 1];
            p->b[from - 1] = 0;
        }
    }

    p->castle = u->castle;
    p->ep = u->ep;
    p->half = u->half;
}

static int gen_legal(Pos *p, Move *out)
{
    Move ms[256];
    Undo u;
    int n = gen_moves(p, ms), i, c = 0;
    for (i = 0; i < n; i++) {
        make_move(p, ms[i], &u);
        if (!in_check(p, -p->side))
            out[c++] = ms[i];
        unmake_move(p, ms[i], &u);
    }
    return c;
}

/* ------------------------------------------------------------------ */
/* Evaluation                                                         */
/* ------------------------------------------------------------------ */

static const int VAL[7] = { 0, 100, 320, 330, 500, 900, 0 };

/* Tables are written from a8 (top-left) to h1, the way a board is drawn. */
static const int PST_P[64] = {
     0,  0,  0,  0,  0,  0,  0,  0,
    50, 50, 50, 50, 50, 50, 50, 50,
    10, 10, 20, 30, 30, 20, 10, 10,
     5,  5, 10, 25, 25, 10,  5,  5,
     0,  0,  0, 20, 20,  0,  0,  0,
     5, -5,-10,  0,  0,-10, -5,  5,
     5, 10, 10,-20,-20, 10, 10,  5,
     0,  0,  0,  0,  0,  0,  0,  0
};
static const int PST_N[64] = {
   -50,-40,-30,-30,-30,-30,-40,-50,
   -40,-20,  0,  0,  0,  0,-20,-40,
   -30,  0, 10, 15, 15, 10,  0,-30,
   -30,  5, 15, 20, 20, 15,  5,-30,
   -30,  0, 15, 20, 20, 15,  0,-30,
   -30,  5, 10, 15, 15, 10,  5,-30,
   -40,-20,  0,  5,  5,  0,-20,-40,
   -50,-40,-30,-30,-30,-30,-40,-50
};
static const int PST_B[64] = {
   -20,-10,-10,-10,-10,-10,-10,-20,
   -10,  0,  0,  0,  0,  0,  0,-10,
   -10,  0,  5, 10, 10,  5,  0,-10,
   -10,  5,  5, 10, 10,  5,  5,-10,
   -10,  0, 10, 10, 10, 10,  0,-10,
   -10, 10, 10, 10, 10, 10, 10,-10,
   -10,  5,  0,  0,  0,  0,  5,-10,
   -20,-10,-10,-10,-10,-10,-10,-20
};
static const int PST_R[64] = {
     0,  0,  0,  0,  0,  0,  0,  0,
     5, 10, 10, 10, 10, 10, 10,  5,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
     0,  0,  0,  5,  5,  0,  0,  0
};
static const int PST_Q[64] = {
   -20,-10,-10, -5, -5,-10,-10,-20,
   -10,  0,  0,  0,  0,  0,  0,-10,
   -10,  0,  5,  5,  5,  5,  0,-10,
    -5,  0,  5,  5,  5,  5,  0, -5,
     0,  0,  5,  5,  5,  5,  0, -5,
   -10,  5,  5,  5,  5,  5,  0,-10,
   -10,  0,  5,  0,  0,  0,  0,-10,
   -20,-10,-10, -5, -5,-10,-10,-20
};
static const int PST_K[64] = {
   -30,-40,-40,-50,-50,-40,-40,-30,
   -30,-40,-40,-50,-50,-40,-40,-30,
   -30,-40,-40,-50,-50,-40,-40,-30,
   -30,-40,-40,-50,-50,-40,-40,-30,
   -20,-30,-30,-40,-40,-30,-30,-20,
   -10,-20,-20,-20,-20,-20,-20,-10,
    20, 20,  0,  0,  0, 20, 20, 20,
    20, 30, 10,  0,  0, 10, 30, 20
};
static const int PST_KE[64] = {   /* king in the endgame */
   -50,-40,-30,-20,-20,-30,-40,-50,
   -30,-20,-10,  0,  0,-10,-20,-30,
   -30,-10, 20, 30, 30, 20,-10,-30,
   -30,-10, 30, 40, 40, 30,-10,-30,
   -30,-10, 30, 40, 40, 30,-10,-30,
   -30,-10, 20, 30, 30, 20,-10,-30,
   -30,-30,  0,  0,  0,  0,-30,-30,
   -50,-30,-30,-30,-30,-30,-30,-50
};

/* Returns the score from white's point of view, in centipawns. */
static int eval(const Pos *p)
{
    int s, pc, pt, sc = 0, mat = 0, idx, white;
    const int *tab;

    for (s = 0; s < 64; s++) {
        pc = p->b[s];
        if (pc && piece(pc) != KING && piece(pc) != PAWN)
            mat += VAL[piece(pc)];
    }

    for (s = 0; s < 64; s++) {
        pc = p->b[s];
        if (!pc)
            continue;
        pt = piece(pc);
        white = pc > 0;
        /* map the square onto the human-readable table above */
        idx = white ? (7 - rankof(s)) * 8 + fileof(s)
                    : rankof(s) * 8 + fileof(s);
        switch (pt) {
        case PAWN:   tab = PST_P;  break;
        case KNIGHT: tab = PST_N;  break;
        case BISHOP: tab = PST_B;  break;
        case ROOK:   tab = PST_R;  break;
        case QUEEN:  tab = PST_Q;  break;
        default:     tab = (mat < 1800) ? PST_KE : PST_K; break;
        }
        sc += sgn(pc) * (VAL[pt] + tab[idx]);
    }
    return sc;
}

/* ------------------------------------------------------------------ */
/* Search: alpha-beta with quiescence                                 */
/* ------------------------------------------------------------------ */

#define INF   1000000
#define MATE  100000

static long g_nodes;

static int mscore(const Pos *p, Move m)
{
    int s = 0;
    if (m.flags & F_CAP) {
        int victim = (m.flags & F_EP) ? PAWN : piece(p->b[m.to]);
        s = 1000 + VAL[victim] * 8 - VAL[piece(p->b[m.from])];
    }
    if (m.flags & F_PROMO)
        s += 900 + VAL[m.promo];
    return s;
}

static void order(const Pos *p, Move *ms, int n)
{
    int sc[256], i, j;
    for (i = 0; i < n; i++)
        sc[i] = mscore(p, ms[i]);
    for (i = 1; i < n; i++) {       /* insertion sort, descending */
        Move mv = ms[i];
        int k = sc[i];
        for (j = i - 1; j >= 0 && sc[j] < k; j--) {
            sc[j + 1] = sc[j];
            ms[j + 1] = ms[j];
        }
        sc[j + 1] = k;
        ms[j + 1] = mv;
    }
}

static int qsearch(Pos *p, int alpha, int beta, int depth)
{
    Move ms[256];
    Undo u;
    int n, i, stand, s;

    g_nodes++;
    stand = eval(p) * p->side;
    if (stand >= beta)
        return beta;
    if (stand > alpha)
        alpha = stand;
    if (depth <= 0)
        return alpha;

    n = gen_moves(p, ms);
    order(p, ms, n);
    for (i = 0; i < n; i++) {
        if (!(ms[i].flags & (F_CAP | F_PROMO)))
            continue;
        make_move(p, ms[i], &u);
        if (in_check(p, -p->side)) {
            unmake_move(p, ms[i], &u);
            continue;
        }
        s = -qsearch(p, -beta, -alpha, depth - 1);
        unmake_move(p, ms[i], &u);
        if (s >= beta)
            return beta;
        if (s > alpha)
            alpha = s;
    }
    return alpha;
}

static int search(Pos *p, int depth, int alpha, int beta, int ply)
{
    Move ms[256];
    Undo u;
    int n, i, s, legal = 0;

    if (depth <= 0)
        return qsearch(p, alpha, beta, 6);

    g_nodes++;
    if (p->half >= 100)
        return 0;

    n = gen_moves(p, ms);
    order(p, ms, n);
    for (i = 0; i < n; i++) {
        make_move(p, ms[i], &u);
        if (in_check(p, -p->side)) {
            unmake_move(p, ms[i], &u);
            continue;
        }
        legal++;
        s = -search(p, depth - 1, -beta, -alpha, ply + 1);
        unmake_move(p, ms[i], &u);
        if (s >= beta)
            return beta;
        if (s > alpha)
            alpha = s;
    }
    if (!legal)
        return in_check(p, p->side) ? -MATE + ply : 0;
    return alpha;
}

/* Picks the engine's move. Returns 0 if there is no legal move. */
static int best_move(Pos *p, int depth, Move *out)
{
    Move ms[256];
    Undo u;
    int n = gen_legal(p, ms), i, d, s, best = -INF, count = 0;
    Move bm;

    if (!n)
        return 0;
    order(p, ms, n);
    bm = ms[0];
    g_nodes = 0;

    for (d = 1; d <= depth; d++) {   /* simple iterative deepening */
        best = -INF;
        count = 0;
        for (i = 0; i < n; i++) {
            make_move(p, ms[i], &u);
            s = -search(p, d - 1, -INF, INF, 1);
            unmake_move(p, ms[i], &u);
            if (s > best) {
                best = s;
                bm = ms[i];
                count = 1;
            } else if (s == best && (rand() % ++count) == 0) {
                bm = ms[i];        /* random pick among equal moves */
            }
        }
        /* put the best move first so the next iteration prunes better */
        for (i = 0; i < n; i++) {
            if (ms[i].from == bm.from && ms[i].to == bm.to &&
                ms[i].promo == bm.promo) {
                Move t = ms[0];
                ms[0] = ms[i];
                ms[i] = t;
                break;
            }
        }
    }
    *out = bm;
    return 1;
}

/* ------------------------------------------------------------------ */
/* X11 front end                                                      */
/* ------------------------------------------------------------------ */

#define SQPX   72
#define BOARD  (SQPX * 8)
#define BARH   40
#define WINW   BOARD
#define WINH   (BOARD + BARH)

static Display *dpy;
static Window win;
static Pixmap buf;
static GC gc;
static XFontStruct *font;
static Atom wm_delete;

static unsigned long C_LIGHT, C_DARK, C_SEL, C_LAST, C_CHK, C_WHITE,
                     C_BLACK, C_BAR, C_TEXT, C_DOT;

static Pos pos;
static int flip = 0;            /* 0: white at the bottom */
static int human = 1;           /* side the human plays; 0 = both */
static int ai_on = 1;
static int depth = 4;
static int selected = -1;
static int last_from = -1, last_to = -1;
static int thinking = 0;
static int result = 0;          /* 0 running, 1 mate, 2 stalemate, 3 draw */
static int promo_pending = 0, promo_from, promo_to;
static char status[160];
static const char *start_fen = NULL;

static Move hist_m[1024];
static Undo hist_u[1024];
static int hist_n = 0;

static unsigned long mkcolor(int r, int g, int b)
{
    XColor c;
    c.red   = (unsigned short)(r * 257);
    c.green = (unsigned short)(g * 257);
    c.blue  = (unsigned short)(b * 257);
    c.flags = DoRed | DoGreen | DoBlue;
    if (!XAllocColor(dpy, DefaultColormap(dpy, DefaultScreen(dpy)), &c))
        return BlackPixel(dpy, DefaultScreen(dpy));
    return c.pixel;
}

/* screen column/row for a square, honouring the flip */
static int scol(int sq) { return flip ? 7 - fileof(sq) : fileof(sq); }
static int srow(int sq) { return flip ? rankof(sq) : 7 - rankof(sq); }

static int sq_at(int px, int py)
{
    int c = px / SQPX, r = py / SQPX, f, rk;
    if (px < 0 || py < 0 || c > 7 || r > 7)
        return -1;
    f = flip ? 7 - c : c;
    rk = flip ? r : 7 - r;
    return rk * 8 + f;
}

/* -------- vector piece drawing (normalised 0..1 inside a square) --- */

static void poly(int x, int y, const double *pts, int n,
                 unsigned long fill, unsigned long line)
{
    XPoint xp[24];
    int i;
    if (n > 23)
        n = 23;
    for (i = 0; i < n; i++) {
        xp[i].x = (short)(x + pts[2 * i] * SQPX + 0.5);
        xp[i].y = (short)(y + pts[2 * i + 1] * SQPX + 0.5);
    }
    XSetForeground(dpy, gc, fill);
    XFillPolygon(dpy, buf, gc, xp, n, Complex, CoordModeOrigin);
    xp[n] = xp[0];
    XSetForeground(dpy, gc, line);
    XDrawLines(dpy, buf, gc, xp, n + 1, CoordModeOrigin);
}

static void circ(int x, int y, double cx, double cy, double rad,
                 unsigned long fill, unsigned long line)
{
    int r = (int)(rad * SQPX + 0.5);
    int px = (int)(x + cx * SQPX + 0.5) - r;
    int py = (int)(y + cy * SQPX + 0.5) - r;
    XSetForeground(dpy, gc, fill);
    XFillArc(dpy, buf, gc, px, py, 2 * r, 2 * r, 0, 360 * 64);
    XSetForeground(dpy, gc, line);
    XDrawArc(dpy, buf, gc, px, py, 2 * r, 2 * r, 0, 360 * 64);
}

static const double P_BASE[] = { .26,.85, .74,.85, .68,.76, .32,.76 };
static const double P_PAWN_BODY[] = { .40,.76, .60,.76, .565,.50, .435,.50 };
static const double P_ROOK_BODY[] = { .33,.76, .67,.76, .63,.36, .37,.36 };
static const double P_ROOK_TOP[] = {
    .27,.36, .73,.36, .73,.20, .635,.20, .635,.27, .555,.27,
    .555,.20, .445,.20, .445,.27, .365,.27, .365,.20, .27,.20
};
static const double P_KNIGHT[] = {
    .26,.85, .76,.85, .75,.70, .71,.56, .66,.44, .72,.33, .66,.22,
    .53,.15, .42,.20, .30,.29, .22,.42, .27,.49, .34,.42, .40,.46,
    .35,.58, .30,.72, .26,.79
};
static const double P_BISHOP_BODY[] = { .36,.76, .64,.76, .615,.60, .385,.60 };
static const double P_BISHOP_TOP[] = {
    .385,.60, .615,.60, .60,.45, .50,.25, .40,.45
};
static const double P_QK_BODY[] = { .32,.76, .68,.76, .645,.47, .355,.47 };
static const double P_QUEEN_TOP[] = {
    .29,.47, .71,.47, .76,.21, .625,.35, .50,.17, .375,.35, .24,.21
};
static const double P_KING_TOP[] = {
    .29,.47, .71,.47, .685,.30, .58,.38, .50,.29, .42,.38, .315,.30
};
static const double P_CROSS_V[] = { .465,.05, .535,.05, .535,.27, .465,.27 };
static const double P_CROSS_H[] = { .39,.12, .61,.12, .61,.185, .39,.185 };

static void draw_piece(int x, int y, int pc)
{
    unsigned long fill = pc > 0 ? C_WHITE : C_BLACK;
    unsigned long line = pc > 0 ? C_BLACK : C_WHITE;

    switch (piece(pc)) {
    case PAWN:
        poly(x, y, P_BASE, 4, fill, line);
        poly(x, y, P_PAWN_BODY, 4, fill, line);
        circ(x, y, .50, .40, .135, fill, line);
        break;
    case ROOK:
        poly(x, y, P_BASE, 4, fill, line);
        poly(x, y, P_ROOK_BODY, 4, fill, line);
        poly(x, y, P_ROOK_TOP, 12, fill, line);
        break;
    case KNIGHT:
        poly(x, y, P_KNIGHT, 17, fill, line);
        circ(x, y, .40, .32, .025, line, line);
        break;
    case BISHOP:
        poly(x, y, P_BASE, 4, fill, line);
        poly(x, y, P_BISHOP_BODY, 4, fill, line);
        poly(x, y, P_BISHOP_TOP, 5, fill, line);
        circ(x, y, .50, .22, .05, fill, line);
        XSetForeground(dpy, gc, line);
        XDrawLine(dpy, buf, gc,
                  (int)(x + .555 * SQPX), (int)(y + .38 * SQPX),
                  (int)(x + .47 * SQPX),  (int)(y + .52 * SQPX));
        break;
    case QUEEN:
        poly(x, y, P_BASE, 4, fill, line);
        poly(x, y, P_QK_BODY, 4, fill, line);
        poly(x, y, P_QUEEN_TOP, 7, fill, line);
        circ(x, y, .24, .19, .045, fill, line);
        circ(x, y, .375, .33, .045, fill, line);
        circ(x, y, .50, .15, .05, fill, line);
        circ(x, y, .625, .33, .045, fill, line);
        circ(x, y, .76, .19, .045, fill, line);
        break;
    case KING:
        poly(x, y, P_BASE, 4, fill, line);
        poly(x, y, P_QK_BODY, 4, fill, line);
        poly(x, y, P_KING_TOP, 7, fill, line);
        poly(x, y, P_CROSS_V, 4, fill, line);
        poly(x, y, P_CROSS_H, 4, fill, line);
        break;
    default:
        break;
    }
}

static void text_at(int x, int y, const char *s, unsigned long col)
{
    if (!font)
        return;
    XSetForeground(dpy, gc, col);
    XDrawString(dpy, buf, gc, x, y, s, (int)strlen(s));
}

static void fill_rect(int x, int y, int w, int h, unsigned long col)
{
    XSetForeground(dpy, gc, col);
    XFillRectangle(dpy, buf, gc, x, y, (unsigned)w, (unsigned)h);
}

static void update_status(void)
{
    const char *who = pos.side > 0 ? "White" : "Black";
    if (thinking)
        snprintf(status, sizeof(status), "Thinking... (depth %d)", depth);
    else if (result == 1)
        snprintf(status, sizeof(status), "Checkmate - %s wins.  n: new game",
                 pos.side > 0 ? "Black" : "White");
    else if (result == 2)
        snprintf(status, sizeof(status), "Stalemate - draw.  n: new game");
    else if (result == 3)
        snprintf(status, sizeof(status), "Draw.  n: new game");
    else if (promo_pending)
        snprintf(status, sizeof(status), "Choose a promotion piece");
    else
        snprintf(status, sizeof(status),
                 "%s to move%s | engine %s d%d | n u f a 1-5 q",
                 who, in_check(&pos, pos.side) ? " (check)" : "",
                 ai_on ? "on" : "off", depth);
}

static void redraw(void)
{
    int r, c, sq, x, y, pc, i;
    Move legal[256];
    int nl = 0;

    if (selected >= 0)
        nl = gen_legal(&pos, legal);

    for (r = 0; r < 8; r++) {
        for (c = 0; c < 8; c++) {
            int f = flip ? 7 - c : c;
            int rk = flip ? r : 7 - r;
            unsigned long col;
            sq = rk * 8 + f;
            x = c * SQPX;
            y = r * SQPX;
            col = ((f + rk) & 1) ? C_LIGHT : C_DARK;
            if (sq == last_from || sq == last_to)
                col = C_LAST;
            if (sq == selected)
                col = C_SEL;
            fill_rect(x, y, SQPX, SQPX, col);

            pc = pos.b[sq];
            if (piece(pc) == KING && in_check(&pos, sgn(pc)))
                fill_rect(x, y, SQPX, SQPX, C_CHK);

            if (font) {
                char lab[3];
                if (r == 7) {
                    lab[0] = (char)('a' + f);
                    lab[1] = 0;
                    text_at(x + SQPX - 12, y + SQPX - 5, lab,
                            ((f + rk) & 1) ? C_DARK : C_LIGHT);
                }
                if (c == 0) {
                    lab[0] = (char)('1' + rk);
                    lab[1] = 0;
                    text_at(x + 4, y + 14, lab,
                            ((f + rk) & 1) ? C_DARK : C_LIGHT);
                }
            }
            if (pc)
                draw_piece(x, y, pc);
        }
    }

    /* move hints for the selected piece */
    for (i = 0; i < nl; i++) {
        if (legal[i].from != selected)
            continue;
        x = scol(legal[i].to) * SQPX;
        y = srow(legal[i].to) * SQPX;
        if (pos.b[legal[i].to] || (legal[i].flags & F_EP)) {
            XSetForeground(dpy, gc, C_DOT);
            XSetLineAttributes(dpy, gc, 4, LineSolid, CapButt, JoinMiter);
            XDrawArc(dpy, buf, gc, x + 4, y + 4, SQPX - 8, SQPX - 8,
                     0, 360 * 64);
            XSetLineAttributes(dpy, gc, 1, LineSolid, CapButt, JoinMiter);
        } else {
            int rr = SQPX / 6;
            XSetForeground(dpy, gc, C_DOT);
            XFillArc(dpy, buf, gc, x + SQPX / 2 - rr, y + SQPX / 2 - rr,
                     2 * rr, 2 * rr, 0, 360 * 64);
        }
    }

    /* promotion chooser */
    if (promo_pending) {
        static const int order4[4] = { QUEEN, ROOK, BISHOP, KNIGHT };
        int col0 = scol(promo_to), row0 = srow(promo_to);
        int dir = (row0 == 0) ? 1 : -1;
        for (i = 0; i < 4; i++) {
            int rr = row0 + dir * i;
            x = col0 * SQPX;
            y = rr * SQPX;
            fill_rect(x, y, SQPX, SQPX, C_SEL);
            XSetForeground(dpy, gc, C_BLACK);
            XDrawRectangle(dpy, buf, gc, x, y, SQPX - 1, SQPX - 1);
            draw_piece(x, y, order4[i] * sgn(pos.b[promo_from]));
        }
    }

    fill_rect(0, BOARD, WINW, BARH, C_BAR);
    update_status();
    text_at(10, BOARD + 25, status, C_TEXT);

    XCopyArea(dpy, buf, win, gc, 0, 0, WINW, WINH, 0, 0);
    XFlush(dpy);
}

static void check_result(void)
{
    Move ms[256];
    int n = gen_legal(&pos, ms);
    if (!n)
        result = in_check(&pos, pos.side) ? 1 : 2;
    else if (pos.half >= 100)
        result = 3;
    else
        result = 0;
}

static void apply(Move m)
{
    make_move(&pos, m, &hist_u[hist_n]);
    hist_m[hist_n] = m;
    if (hist_n < 1023)
        hist_n++;
    last_from = m.from;
    last_to = m.to;
    selected = -1;
    check_result();
}

static void engine_move(void)
{
    Move m;
    if (result || !ai_on || !human || pos.side == human)
        return;
    thinking = 1;
    redraw();
    if (best_move(&pos, depth, &m))
        apply(m);
    thinking = 0;
    redraw();
}

static void new_game(void)
{
    if (!start_fen || !set_fen(&pos, start_fen))
        set_start(&pos);
    hist_n = 0;
    selected = -1;
    last_from = last_to = -1;
    promo_pending = 0;
    result = 0;
    redraw();
    engine_move();
}

static void take_back(void)
{
    int steps = (ai_on && human) ? 2 : 1;
    while (steps-- > 0 && hist_n > 0) {
        hist_n--;
        unmake_move(&pos, hist_m[hist_n], &hist_u[hist_n]);
    }
    selected = -1;
    promo_pending = 0;
    if (hist_n > 0) {
        last_from = hist_m[hist_n - 1].from;
        last_to = hist_m[hist_n - 1].to;
    } else {
        last_from = last_to = -1;
    }
    check_result();
    redraw();
}

static void click(int px, int py)
{
    Move legal[256];
    int n, i, sq;

    if (promo_pending) {
        static const int order4[4] = { QUEEN, ROOK, BISHOP, KNIGHT };
        int col0 = scol(promo_to), row0 = srow(promo_to);
        int dir = (row0 == 0) ? 1 : -1;
        int c = px / SQPX, r = py / SQPX;
        if (py < BOARD && c == col0) {
            for (i = 0; i < 4; i++) {
                if (r == row0 + dir * i) {
                    n = gen_legal(&pos, legal);
                    for (sq = 0; sq < n; sq++) {
                        if (legal[sq].from == promo_from &&
                            legal[sq].to == promo_to &&
                            legal[sq].promo == order4[i]) {
                            promo_pending = 0;
                            apply(legal[sq]);
                            redraw();
                            engine_move();
                            return;
                        }
                    }
                }
            }
        }
        promo_pending = 0;      /* clicked elsewhere: cancel */
        redraw();
        return;
    }

    if (result || thinking || py >= BOARD)
        return;
    if (ai_on && human && pos.side != human)
        return;

    sq = sq_at(px, py);
    if (sq < 0)
        return;

    n = gen_legal(&pos, legal);

    if (selected >= 0) {
        for (i = 0; i < n; i++) {
            if (legal[i].from == selected && legal[i].to == sq) {
                if (legal[i].flags & F_PROMO) {
                    promo_pending = 1;
                    promo_from = selected;
                    promo_to = sq;
                    redraw();
                    return;
                }
                apply(legal[i]);
                redraw();
                engine_move();
                return;
            }
        }
    }
    selected = (pos.b[sq] && sgn(pos.b[sq]) == pos.side) ? sq : -1;
    redraw();
}

static void keypress(KeySym k)
{
    switch (k) {
    case XK_n: case XK_N:
        new_game();
        break;
    case XK_u: case XK_U:
        take_back();
        break;
    case XK_f: case XK_F:
        flip = !flip;
        if (human)
            human = -human;
        selected = -1;
        redraw();
        engine_move();
        break;
    case XK_a: case XK_A:
        ai_on = !ai_on;
        if (ai_on && !human)
            human = flip ? -1 : 1;
        redraw();
        engine_move();
        break;
    case XK_1: case XK_2: case XK_3: case XK_4: case XK_5:
        depth = (int)(k - XK_1) + 1;
        redraw();
        break;
    default:
        break;
    }
}

static void load_font(void)
{
    static const char *names[] = {
        "-*-helvetica-medium-r-normal--14-*-*-*-*-*-iso8859-1",
        "-*-dejavu sans-medium-r-normal--14-*-*-*-*-*-*-*",
        "9x15", "fixed", "-*-*-*-*-*-*-14-*-*-*-*-*-*-*", NULL
    };
    int i;
    for (i = 0; names[i]; i++) {
        font = XLoadQueryFont(dpy, names[i]);
        if (font) {
            XSetFont(dpy, gc, font->fid);
            return;
        }
    }
    font = NULL;   /* the game still works, just without labels */
}

int main(int argc, char **argv)
{
    XSizeHints hints;
    XEvent ev;
    int screen, i;

    srand((unsigned)time(NULL));

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--depth") && i + 1 < argc) {
            depth = atoi(argv[++i]);
            if (depth < 1) depth = 1;
            if (depth > 6) depth = 6;
        } else if (!strcmp(argv[i], "--black")) {
            human = -1;
            flip = 1;
        } else if (!strcmp(argv[i], "--two-player")) {
            ai_on = 0;
        } else if (!strcmp(argv[i], "--fen") && i + 1 < argc) {
            start_fen = argv[++i];
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("usage: %s [--depth N] [--black] [--two-player] "
                   "[--fen \"<position>\"]\n", argv[0]);
            return 0;
        }
    }

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "xchess: cannot open display (is X running? "
                        "try DISPLAY=:0)\n");
        return 1;
    }
    screen = DefaultScreen(dpy);

    win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 0, 0,
                              WINW, WINH, 0,
                              BlackPixel(dpy, screen),
                              WhitePixel(dpy, screen));
    XStoreName(dpy, win, "Chess");
    XSelectInput(dpy, win, ExposureMask | ButtonPressMask | KeyPressMask |
                           StructureNotifyMask);

    hints.flags = PMinSize | PMaxSize;
    hints.min_width = hints.max_width = WINW;
    hints.min_height = hints.max_height = WINH;
    XSetWMNormalHints(dpy, win, &hints);

    wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete, 1);

    gc = XCreateGC(dpy, win, 0, NULL);
    buf = XCreatePixmap(dpy, win, WINW, WINH,
                        (unsigned)DefaultDepth(dpy, screen));

    C_LIGHT = mkcolor(238, 238, 210);
    C_DARK  = mkcolor(118, 150,  86);
    C_SEL   = mkcolor(246, 246, 105);
    C_LAST  = mkcolor(205, 210, 106);
    C_CHK   = mkcolor(224, 108,  92);
    C_WHITE = mkcolor(252, 252, 250);
    C_BLACK = mkcolor( 32,  32,  34);
    C_BAR   = mkcolor( 38,  36,  33);
    C_TEXT  = mkcolor(232, 230, 225);
    C_DOT   = mkcolor( 90, 110,  70);

    load_font();
    XMapWindow(dpy, win);

    if (!start_fen || !set_fen(&pos, start_fen)) {
        if (start_fen)
            fprintf(stderr, "xchess: bad FEN, starting a normal game\n");
        set_start(&pos);
    }
    if (ai_on && human && pos.side != human)
        flip = (human == -1);
    check_result();

    for (;;) {
        XNextEvent(dpy, &ev);
        switch (ev.type) {
        case Expose:
            if (ev.xexpose.count == 0)
                redraw();
            break;
        case MapNotify:
            redraw();
            if (human == -1)
                engine_move();
            break;
        case ButtonPress:
            if (ev.xbutton.button == Button1)
                click(ev.xbutton.x, ev.xbutton.y);
            break;
        case KeyPress: {
            KeySym k = XLookupKeysym(&ev.xkey, 0);
            if (k == XK_q || k == XK_Q || k == XK_Escape)
                goto done;
            keypress(k);
            break;
        }
        case ClientMessage:
            if ((Atom)ev.xclient.data.l[0] == wm_delete)
                goto done;
            break;
        default:
            break;
        }
    }
done:
    if (font)
        XFreeFont(dpy, font);
    XFreePixmap(dpy, buf);
    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}
