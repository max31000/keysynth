/* keysynth DSP plugin C ABI (docs/PLUGINS.md, ARCHITECTURE §10).
 *
 * A plugin DLL exports one function:
 *
 *     KS_PLUGIN_EXPORT const ks_plugin_descriptor* ks_get_plugin(void);
 *
 * returning a pointer to a static descriptor. No C++ types cross the boundary. Every struct starts with
 * `struct_size` (= sizeof of the struct the plugin was compiled against) so the ABI can grow by appending
 * fields; ABI v1 hosts require the full v1 structs and reject smaller ones.
 *
 * Threads:
 *   loader thread  : ks_get_plugin (once per load; the descriptor must stay valid while the DLL is loaded)
 *   control thread : create, destroy, prepare, reset (while not live), load_state (before prepare), save_state
 *   audio thread   : process, set_param (block start, only when the value changed), get_param, reset (panic,
 *                    NaN recovery), tail_samples
 * save_state may be called on the control thread WHILE process runs on the audio thread for the same instance
 * (hot reload carries state over): only read state that is safe to read concurrently (atomics, or a snapshot
 * the audio thread publishes). Never block the audio thread for it.
 * Real-time rules for audio-thread functions: no allocation, locks, IO, logging or exceptions; bounded cost.
 * The host calls every function inside an SEH guard: a fault mutes the instance for good (until reloaded).
 *
 * Buffers: process() gets stereo[0] = left, stereo[1] = right, nframes <= max_block (from prepare).
 *   Instruments: buffers are zeroed on entry; add/write the output.
 *   Effects:     in-place stereo (read input, write output in the same buffers).
 * Events are sorted by sample_offset (0 <= sample_offset < nframes). Plain units for every param value.
 */
#ifndef KEYSYNTH_PLUGIN_ABI_H
#define KEYSYNTH_PLUGIN_ABI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KS_PLUGIN_ABI_VERSION 1u

#if defined(_WIN32)
#define KS_PLUGIN_EXPORT __declspec(dllexport)
#else
#define KS_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

typedef enum ks_plugin_kind { KS_KIND_INSTRUMENT = 0, KS_KIND_EFFECT = 1 } ks_plugin_kind;

typedef enum ks_param_scale {
    KS_SCALE_LINEAR = 0,
    KS_SCALE_LOG = 1,
    KS_SCALE_INT = 2,
    KS_SCALE_ENUM = 3,
    KS_SCALE_BOOL = 4
} ks_param_scale;

enum {
    KS_PARAM_READ_ONLY = 1,      /* written by the plugin (get_param), shown as a readout */
    KS_PARAM_HIDDEN = 2,
    KS_PARAM_NON_AUTOMATABLE = 4
};

typedef struct ks_param_spec {
    uint32_t struct_size;        /* sizeof(ks_param_spec) */
    const char* id;              /* stable snake_case [a-z][a-z0-9_]*, unique in the plugin (preset format) */
    const char* name;            /* display name */
    const char* group;           /* "" = none */
    const char* unit;            /* "Hz", "dB", "s", "%", ... or "" */
    float min, max, def;         /* plain units */
    uint32_t scale;              /* ks_param_scale */
    float skew_centre;           /* log params: value at knob centre (0 = geometric mean) */
    uint32_t flags;              /* KS_PARAM_* */
    const char* const* choices;  /* enum labels (value = index), may be NULL */
    uint32_t num_choices;
} ks_param_spec;

/* Same layout as the engine's MidiEvent (static_assert'ed by the host). */
typedef enum ks_event_type {
    KS_EV_NOTE_ON = 0,
    KS_EV_NOTE_OFF = 1,
    KS_EV_CONTROL_CHANGE = 2,
    KS_EV_PITCH_BEND = 3,        /* value_f -1..1 */
    KS_EV_CHANNEL_PRESSURE = 4,  /* value_f 0..1 */
    KS_EV_POLY_PRESSURE = 5,     /* data1 = note, value_f 0..1 */
    KS_EV_PROGRAM_CHANGE = 6,
    KS_EV_ALL_NOTES_OFF = 7,     /* release everything */
    KS_EV_ALL_SOUND_OFF = 8      /* hard stop */
} ks_event_type;

typedef struct ks_event {
    uint32_t sample_offset;
    uint8_t type;                /* ks_event_type */
    uint8_t channel;             /* 1..16 (0 = internal) */
    uint8_t data1;               /* note or cc number */
    uint8_t value7;              /* velocity / cc value */
    float value_f;               /* normalized value (velocity/127, cc/127, bend -1..1) */
    int32_t note_id;             /* -1 = derive from note */
} ks_event;

typedef void* ks_instance;

typedef struct ks_plugin_descriptor {
    uint32_t struct_size;        /* sizeof(ks_plugin_descriptor) */
    uint32_t abi_version;        /* KS_PLUGIN_ABI_VERSION */
    const char* id;              /* must equal the plugin directory name (plugins/<id>/) */
    const char* name;            /* display name */
    const char* category;        /* "Distortion", "Synth", ... */
    uint32_t kind;               /* ks_plugin_kind */
    const ks_param_spec* params;
    uint32_t num_params;

    ks_instance (*create)(void);                             /* NULL on failure */
    void (*destroy)(ks_instance);
    void (*prepare)(ks_instance, double sample_rate, int32_t max_block); /* allocate here */
    void (*reset)(ks_instance);                              /* kill voices / tails (RT-safe) */
    void (*process)(ks_instance, float** stereo, int32_t nframes, const ks_event* events, int32_t nevents);
    void (*set_param)(ks_instance, uint32_t index, float value);
    float (*get_param)(ks_instance, uint32_t index);         /* optional (READ_ONLY params), may be NULL */
    int32_t (*tail_samples)(ks_instance);                    /* optional, may be NULL */
    int32_t (*latency_samples)(ks_instance);                 /* optional, may be NULL; keep 0 (§4.7) */
    /* Non-param state blob. save_state returns the number of bytes needed; it writes only when capacity is
     * large enough. load_state returns 0 on success. Both optional (NULL = stateless). */
    int32_t (*save_state)(ks_instance, void* buffer, int32_t capacity);
    int32_t (*load_state)(ks_instance, const void* data, int32_t size);
} ks_plugin_descriptor;

typedef const ks_plugin_descriptor* (*ks_get_plugin_fn)(void);

#ifdef __cplusplus
}
#endif

#endif /* KEYSYNTH_PLUGIN_ABI_H */
