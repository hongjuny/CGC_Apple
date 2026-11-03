// Band-limited step synthesis (the Blip_Buffer technique): every change of an
// output level is recorded at its exact time as a delta, spread over a few
// samples with a windowed-sinc impulse; the output is the running sum. Square
// waves from the speaker and the AY PSGs then come out without the aliasing
// that point- or box-sampling them at 44.1 kHz produces.
#include <math.h>
#include <string.h>
#include "a2internal.h"

#define BLIP_PHASES 64
#define CUTOFF 0.45     // fraction of the sample rate (~19.8 kHz at 44.1 kHz)

static float kernel[BLIP_PHASES][BLIP_TAPS];
static bool kernel_ready;

static void build_kernel(void)
{
    for (int p = 0; p < BLIP_PHASES; p++) {
        double sum = 0, frac = (double)p / BLIP_PHASES;
        for (int j = 0; j < BLIP_TAPS; j++) {
            double t = j - (BLIP_TAPS / 2 - 1) - frac;           // tap time relative to the event
            double x = 2.0 * CUTOFF * t;
            double sinc = fabs(x) < 1e-9 ? 1.0 : sin(M_PI * x) / (M_PI * x);
            double w = (t + BLIP_TAPS / 2.0) / BLIP_TAPS;        // Blackman window over the taps
            double win = 0.42 - 0.5 * cos(2 * M_PI * w) + 0.08 * cos(4 * M_PI * w);
            kernel[p][j] = (float)(sinc * win);
            sum += kernel[p][j];
        }
        for (int j = 0; j < BLIP_TAPS; j++)
            kernel[p][j] = (float)(kernel[p][j] / sum);           // each step adds exactly `delta`
    }
    kernel_ready = true;
}

void blip_clear(blip *b)
{
    if (!kernel_ready)
        build_kernel();
    memset(b->buf, 0, sizeof b->buf);
    b->integrator = 0;
}

// Adds a level change of `delta` at sample time `t` (relative to buf[0]).
void blip_add(blip *b, double t, float delta)
{
    if (t < BLIP_TAPS / 2 - 1)
        t = BLIP_TAPS / 2 - 1;                 // already-finalised samples can't change
    int i = (int)t;
    if (i + BLIP_TAPS / 2 >= BLIP_SIZE)
        return;                                // buffer not being drained; drop
    int p = (int)((t - i) * BLIP_PHASES);
    const float *k = kernel[p];
    float *out = b->buf + i - (BLIP_TAPS / 2 - 1);
    for (int j = 0; j < BLIP_TAPS; j++)
        out[j] += delta * k[j];
}

// Removes n finished samples into out.
void blip_read(blip *b, int n, float *out)
{
    for (int i = 0; i < n; i++) {
        b->integrator += b->buf[i];
        out[i] = (float)b->integrator;
    }
    memmove(b->buf, b->buf + n, (BLIP_SIZE + BLIP_TAPS - n) * sizeof b->buf[0]);
    memset(b->buf + BLIP_SIZE + BLIP_TAPS - n, 0, n * sizeof b->buf[0]);
}
