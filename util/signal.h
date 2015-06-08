#ifndef __UTIL_SIGNAL_H__
#define __UTIL_SIGNAL_H__

#include "core/time.h"

// ---- Exponential Low-Pass Filters ----

struct ema_state {
    double out_1;
    double in_1;
    mbeat_t t_1;
    union {
        double tau; // Normal EMA
        struct {   // Diode EMA
            double tau_rise;
            double tau_fall;
        };
    };
};

// Exponential Moving Average Filter
// Single-pole, linear, IIR filter
// w/ monotonic impulse response
// tau: time constant in beats
void ema_init(struct ema_state * state, double tau);
double ema_update(struct ema_state * state, mbeat_t t, double in);

static inline void ema_set_tau(struct ema_state * state, double tau){
    state->tau = tau;
}

// "Diode" Exponential Moving Average Filter
// Single-pole, non-linear, IIR filter
// Separate  constants for rising & falling singal
// tau_rise/tau_fall: time constants in beats
void dema_init(struct ema_state * state, double tau_rise, double tau_fall);
double dema_update(struct ema_state * state, mbeat_t t, double in);

static inline void dema_set_tau(struct ema_state * state, double tau_rise, double tau_fall){
    state->tau_rise = tau_rise;
    state->tau_fall = tau_fall;
}

// ---- Automatic Gain Control / Compressor ----

struct agc_state {
    // Output range
    double range_high;
    double range_low;

    // Input envelope
    struct ema_state env_high;
    struct ema_state env_low;

    // Input range without amplification
    // These are in the same units as the input, not the output
    double knee_high;
    double knee_low;
};

void agc_init(struct agc_state * state, double range_high, double range_low, double knee_high, double knee_low, double tau);
double agc_update(struct agc_state * state, mbeat_t t, double in);

#endif
