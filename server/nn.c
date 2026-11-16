#include <math.h>
#include <stdio.h>
#include <string.h>

#include <sys/neutrino.h>
#include <sys/syspage.h>

#include "nn.h"
#include "nn_weights.h"

/* ------------------------------------------------------------------ */
/* Timing. Local rather than borrowed from engine.c so that nn.c can be
 * tested on its own, and because engine.c's cost model is about to be
 * seeded FROM these measurements rather than the other way round.       */
/* ------------------------------------------------------------------ */

static uint64_t cycles_per_us = 1;

static uint64_t now_us(void)
{
    return (uint64_t)ClockCycles() / cycles_per_us;
}

/* ------------------------------------------------------------------ */
/* Forward pass                                                        */
/* ------------------------------------------------------------------ */

/*
 * One patch through one network.
 *
 * Plain scalar float32: no SIMD, no blocking, no threads. That is a
 * deliberate choice rather than laziness. This code's job is to be a
 * believable, portable, chunkable cost -- it builds unchanged for x86_64
 * and aarch64le with qcc and has no dependencies at all, which matters
 * more here than throughput. A real deployment would hand this to a
 * vendor runtime; the point Triage is making is about *when* inference
 * runs, not how fast the kernels are.
 */
static void forward(const nn_net_t *net, const float *in,
                    float *out, int *out_len)
{
    float  buf_a[NN_MAX_UNITS];
    float  buf_b[NN_MAX_UNITS];
    const float *src = in;
    float       *dst = buf_a;
    int          i, l;

    for (l = 0; l < net->n_layers; l++) {
        const nn_layer_t *ly = &net->layers[l];
        int               last = (l == net->n_layers - 1);

        for (i = 0; i < ly->out; i++) {
            const float *w = ly->w + (size_t)i;
            float        acc = ly->b[i];
            int          j;

            /* Weights are stored [in][out], so stride by ly->out. Row-major
             * over the input keeps the input in cache across outputs. */
            for (j = 0; j < ly->in; j++)
                acc += src[j] * w[(size_t)j * (size_t)ly->out];

            /* ReLU everywhere but the output layer. */
            dst[i] = (!last && acc < 0.0f) ? 0.0f : acc;
        }

        src = dst;
        dst = (dst == buf_a) ? buf_b : buf_a;
        *out_len = ly->out;
    }

    memcpy(out, src, sizeof(float) * (size_t)*out_len);
}

/*
 * Softmax, then the best DIGIT and how confident we are in it.
 *
 * Two things here were wrong in the first version and are worth recording,
 * because together they made the detector useless while looking fine:
 *
 * 1. The exponential was a 4-term Taylor series, to avoid linking libm.
 *    On (x - max) that is often very negative -- at x = -10 the series
 *    gives +291 instead of 0.000045 -- so the "confidence" was garbage and
 *    the patch that won the frame was effectively arbitrary. expf() it is;
 *    ten exponentials against 3296 multiply-accumulates is not the cost
 *    worth optimising.
 *
 * 2. It returned the best class over ALL classes, background included. A
 *    sliding window over a frame is mostly empty, so the answer was
 *    whichever piece of background scored highest -- which is why every
 *    frame came back class=5 whatever digit was actually in it.
 *
 * So this returns the most probable NON-background class and its
 *  probability. A patch the network thinks is empty scores 0 and loses,
 * which is what lets the one patch containing something win the frame.
 */
static void classify(const float *logits, int n, int *cls, unsigned *score)
{
    float mx = logits[0];
    float sum = 0.0f, p[NN_CLASSES];
    float best_p = -1.0f;
    int   best = -1, i;

    for (i = 1; i < n; i++)
        if (logits[i] > mx)
            mx = logits[i];

    for (i = 0; i < n; i++) {
        p[i] = expf(logits[i] - mx);
        sum += p[i];
    }
    if (sum <= 0.0f)
        sum = 1.0f;

    for (i = 0; i < n; i++) {
        if (i == NN_BG_CLASS)
            continue;                 /* "nothing here" is not a detection */
        if (p[i] > best_p) {
            best_p = p[i];
            best   = i;
        }
    }

    *cls = best;
    /*
     * Confidence is the probability of that digit, which is implicitly
     * discounted by however much probability went to background -- so an
     * empty patch reports a low number without any thresholding of ours.
     */
    *score = (best >= 0) ? (unsigned)(best_p / sum * 1000.0f) : 0u;
    if (*score > 1000u)
        *score = 1000u;
}

/* ------------------------------------------------------------------ */
/* Calibration                                                         */
/* ------------------------------------------------------------------ */

static uint64_t us_per_patch[TRIAGE_TIER__COUNT];

static const nn_net_t *net_for(triage_tier_t tier)
{
    if (tier <= TRIAGE_TIER_NONE || tier >= TRIAGE_TIER__COUNT)
        return NULL;
    return &nn_nets[tier];
}

unsigned nn_patches(triage_tier_t tier)
{
    const nn_net_t *net = net_for(tier);
    unsigned        n;

    if (net == NULL || net->stride <= 0)
        return 0;
    n = (unsigned)((NN_FRAME - NN_PATCH) / net->stride) + 1u;
    return n * n;
}

unsigned nn_accuracy(triage_tier_t tier)
{
    const nn_net_t *net = net_for(tier);

    return net ? net->accuracy : 0u;
}

uint64_t nn_us_per_patch(triage_tier_t tier)
{
    if (tier <= TRIAGE_TIER_NONE || tier >= TRIAGE_TIER__COUNT)
        return 1;
    return us_per_patch[tier] ? us_per_patch[tier] : 1;
}

unsigned nn_patches_per_chunk(triage_tier_t tier, uint64_t target_us)
{
    uint64_t per = nn_us_per_patch(tier);
    uint64_t n   = target_us / (per ? per : 1);

    if (n < 1)
        n = 1;
    /* A chunk should not be the whole frame, or there is no boundary at
     * which to re-check the deadline. */
    if (n > nn_patches(tier))
        n = nn_patches(tier);
    return (unsigned)(n ? n : 1);
}

unsigned nn_frame_count(void)
{
    return NN_FRAMES;
}

void nn_init(void)
{
    uint64_t      cps = SYSPAGE_ENTRY(qtime)->cycles_per_sec;
    triage_tier_t t;

    cycles_per_us = (cps >= 1000000u) ? cps / 1000000u : 1;
    if (cycles_per_us == 0)
        cycles_per_us = 1;

    /*
     * Measure, do not declare. The tier cost table used to be six numbers
     * chosen by hand; now a tier costs whatever its arithmetic costs on
     * this machine, and the engine's cost model is seeded from that.
     *
     * Fastest of several trials, for the reason the engine documents: an
     * interrupted trial understates throughput and would make every later
     * chunk look expensive.
     */
    for (t = TRIAGE_TIER_SMALL; t < TRIAGE_TIER__COUNT; t++) {
        const nn_net_t *net = net_for(t);
        float           logits[NN_CLASSES];
        int             n_out = 0, trial;
        uint64_t        best = (uint64_t)-1;
        const int       reps = 8;

        if (net == NULL)
            continue;

        forward(net, nn_frames[0].pixels, logits, &n_out);   /* warm up */

        for (trial = 0; trial < 5; trial++) {
            uint64_t t0 = now_us();
            uint64_t dt;
            int      r;

            for (r = 0; r < reps; r++)
                forward(net, nn_frames[0].pixels, logits, &n_out);

            dt = now_us() - t0;
            if (dt > 0 && dt < best)
                best = dt;
        }

        us_per_patch[t] = (best == (uint64_t)-1) ? 1 : (best / reps);
        if (us_per_patch[t] == 0)
            us_per_patch[t] = 1;   /* faster than our clock can see */
    }
}

/* ------------------------------------------------------------------ */
/* A scan in progress, per thread                                      */
/* ------------------------------------------------------------------ */

static __thread const nn_net_t   *s_net;
static __thread const nn_frame_t *s_frame;
static __thread unsigned          s_next;       /* next patch index */
static __thread unsigned          s_total;
static __thread unsigned          s_per_row;
static __thread int               s_best_cls;
static __thread unsigned          s_best_score;
static __thread int               s_best_x, s_best_y;

void nn_scan_begin(triage_tier_t tier, uint32_t frame_sel)
{
    s_net        = net_for(tier);
    s_frame      = &nn_frames[frame_sel % NN_FRAMES];
    s_next       = 0;
    s_total      = nn_patches(tier);
    s_per_row    = s_net && s_net->stride > 0
                       ? (unsigned)((NN_FRAME - NN_PATCH) / s_net->stride) + 1u
                       : 1u;
    s_best_cls   = -1;
    s_best_score = 0;
    s_best_x     = -1;
    s_best_y     = -1;
}

int nn_scan_complete(void)
{
    return s_net == NULL || s_next >= s_total;
}

unsigned nn_scan_step(unsigned n)
{
    float    patch[NN_PATCH_LEN];
    float    logits[NN_CLASSES];
    unsigned did = 0;

    if (s_net == NULL || s_frame == NULL)
        return 0;

    while (did < n && s_next < s_total) {
        unsigned px = (s_next % s_per_row) * (unsigned)s_net->stride;
        unsigned py = (s_next / s_per_row) * (unsigned)s_net->stride;
        int      cls = 0, n_out = 0, row;
        unsigned score = 0;

        /* Lift the window out of the frame. A real detector would work in
         * place; copying keeps the forward pass free of stride arithmetic
         * and costs a few hundred bytes of memcpy per patch. */
        for (row = 0; row < NN_PATCH; row++)
            memcpy(&patch[row * NN_PATCH],
                   &s_frame->pixels[(py + (unsigned)row) * NN_FRAME + px],
                   sizeof(float) * NN_PATCH);

        forward(s_net, patch, logits, &n_out);
        classify(logits, n_out, &cls, &score);

        /* Keep the most confident detection in the frame so far, which is
         * what makes an early stop still useful. */
        if (score > s_best_score) {
            s_best_score = score;
            s_best_cls   = cls;
            s_best_x     = (int)px;
            s_best_y     = (int)py;
        }

        s_next++;
        did++;
    }
    return did;
}

int      nn_scan_best_class(void)   { return s_best_cls; }
unsigned nn_scan_best_score(void)   { return s_best_score; }
int      nn_scan_best_x(void)       { return s_best_x; }
int      nn_scan_best_y(void)       { return s_best_y; }
unsigned nn_scan_done_patches(void) { return s_next; }

int nn_scan_truth_class(void)
{
    return s_frame ? s_frame->label : -1;
}
