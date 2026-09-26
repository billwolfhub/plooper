// Plooper: a four-playhead looper with strange reverbs for the Electrosmith
// Daisy Patch.
//
// Record a loop, then play it back with four heads (A-D), each with its own
// level, speed (reverse below zero), start, window length, direction mode,
// and pan. The heads feed a reverb with six modes, including reversed tails
// and pitch-shifted feedback.
//
// Signal flow (per sample):
//
//   IN 1-4 -> levels (INPUT page) -> DC block -> first take / overdub write
//                                             \-> monitor --------------+
//   tape (loop_buffer) -> heads A-D -> pan -> sum ----------------------+-> limiter -> OUT 1/2
//                                             \-> DC block -> reverb --+
//                                                  ^   (+ pitched      \-> limiter -> OUT 3/4
//                                                  |    feedback)
//                                                  +-- shimmer / reverse / freeze
//
// There is one tape and four heads reading it; the tape is rewritten at
// `master_position` (a speed-1 "record head") while overdubbing, and on every
// pass with Decay: Always, which is where fading and aging happen.
//
// Threads: everything that touches audio, the looper state, knob pickup, gates,
// and the encoder's press edges runs in AudioCallback (48-sample blocks, 1 kHz).
// The main loop handles the encoder menu, holds that need timing (latching
// DUB), the display, and saving settings. They talk through `volatile` flags
// (the UI requests, the audio callback acts), so the audio callback can
// interrupt the main loop at any point without tearing state.
//
// Memory: the 60 s stereo tape and the reverb, pitch shifter, pre-delay, and
// reverse buffers live in SDRAM, which isn't zeroed at boot (see main()).

#include <algorithm>
#include <cstdio>
#include <cstring>
#include "daisy_patch.h"
#include "daisysp.h"
#include "util/PersistentStorage.h"

using namespace daisy;
using namespace daisysp;

const int kNumHeads = 4;
const int kMaxLoopSamples = 48000 * 60; // 60 seconds
const int kMinLoopSamples = 480; // 10 ms
const int kMinWindowSamples = 960; // 20 ms
const int kEdgeFadeSamples = 240; // 5 ms fades at window edges and take ends
const int kReverseBufferSamples = 48000 * 16 / 5; // 3.2 s, twice the longest reverse window
const int kPreDelaySamples = 24000; // 500 ms
const uint32_t kLoopPulseMillis = 5;
const uint32_t kLatchMinimumMillis = 4000; // Shortest hold that latches DUB
const uint32_t kSetupTimeoutMillis = 6000; // Idle time before SETUP returns to play
const uint32_t kScreenUpdateMillis = 16;
const uint32_t kWaveformRefreshMillis = 500;
const uint32_t kSettingsSaveDelayMillis = 2000;
// Separate from other firmwares' settings (grainwaves 0x7F0000, edges 0x7E0000)
const uint32_t kSettingsQspiOffset = 0x7D0000;
const uint32_t kSettingsVersion = 0x9100C004;
const float kPickupTolerance = 0.02f; // A knob this close to a stored value picks it up

DaisyPatch patch;
float sample_rate = 48000.f;

// ---------------------------------------------------------------------------
// Buffers (SDRAM, ~25 MB of 64 MB)

float DSY_SDRAM_BSS loop_buffer[2][kMaxLoopSamples];
float DSY_SDRAM_BSS reverse_buffer[2][kReverseBufferSamples];
ReverbSc DSY_SDRAM_BSS reverb;
PitchShifter DSY_SDRAM_BSS shifter;
DelayLine<float, kPreDelaySamples> DSY_SDRAM_BSS pre_delay[2];

// ---------------------------------------------------------------------------
// Settings

// Pages the encoder turns through. The first six are knob pages: each knob
// sets one parameter for one column (IN 1-4, heads A-D, or the FX controls).
enum Page { PAGE_INPUT, PAGE_MIX, PAGE_SPEED, PAGE_START, PAGE_LENGTH, PAGE_FX, PAGE_SETUP, NUM_PAGES };
const int kNumKnobPages = PAGE_SETUP;
const char *page_names[NUM_PAGES] = { "INPUT", "MIX", "SPEED", "START", "LENGTH", "FX", "SETUP" };
const char *input_labels[kNumHeads] = { "IN1", "IN2", "IN3", "IN4" };
const char *fx_labels[kNumHeads] = { "MIX", "DECAY", "TONE", "ODD" };
const char *head_labels[kNumHeads] = { "A", "B", "C", "D" };

enum ReverbMode { REVERB_PLATE, REVERB_SHIMMER, REVERB_SUB, REVERB_BACKWARDS, REVERB_GHOST, REVERB_FREEZE, NUM_REVERB_MODES };
const char *reverb_names[NUM_REVERB_MODES] = { "Plate", "Shimmer", "Sub", "Backwards", "Ghost", "Freeze" };

enum Direction { DIR_FORWARD, DIR_REVERSE, DIR_PINGPONG, DIR_RANDOM, NUM_DIRECTIONS };
const char *direction_names[NUM_DIRECTIONS] = { "Forward", "Reverse", "Pingpong", "Random" };

enum Snap { SNAP_OFF, SNAP_OCTAVES, SNAP_MUSICAL, NUM_SNAPS };
const char *snap_names[NUM_SNAPS] = { "Off", "Octaves", "Musical" };

enum Gate2Mode { GATE2_REVERSE, GATE2_RETRIGGER, GATE2_SCATTER, GATE2_DUB, NUM_GATE2_MODES };
const char *gate2_names[NUM_GATE2_MODES] = { "Reverse", "Retrig", "Scatter", "Dub" };

// Knob page values, stored as raw knob positions 0..1 and converted to real
// units where they're used (SpeedFromKnob, LengthFraction, ...). Not saved.
float page_value[kNumKnobPages][kNumHeads] = {
    { 1.f, 1.f, 0.f, 0.f }, // INPUT: levels of IN 1-4 into the loop and monitor
    { 0.8f, 0.f, 0.f, 0.f }, // MIX: only head A audible at first
    // SPEED: +1, -1, +0.5, +2 (see SpeedFromKnob)
    { 0.5f + 1.f / 3.f, 0.5f - 1.f / 3.f, 0.5f + 1.f / 6.f, 1.f },
    { 0.f, 0.f, 0.f, 0.f }, // START
    { 1.f, 1.f, 1.f, 1.f }, // LENGTH: whole loop
    { 0.3f, 0.6f, 0.6f, 0.5f }, // FX: mix, decay, tone, strange
};

int reverb_mode = REVERB_PLATE;
int direction[kNumHeads] = { DIR_FORWARD, DIR_FORWARD, DIR_FORWARD, DIR_FORWARD };
int pan[kNumHeads] = { -30, 30, -60, 60 }; // -100..100
int snap = SNAP_MUSICAL;
int gate2_mode = GATE2_REVERSE;
int dub_fade = 0; // Percent the loop fades on each pass while it's rewritten
bool decay_always = false; // Tape: fade on every pass in PLAY too, not just DUB
int tape_age = 0; // Tape: 0-100, each rewritten pass gets darker and saturated
int tape_wow = 0; // Tape: 0-100, slow wobble and flutter on the heads
bool stereo_input = false; // false: IN 1-4 mixed to both channels
bool monitor = true; // Pass the input to OUT 1/2
int latch_loops = 2; // Holding a punch this many loops latches DUB (0 = never)

// SETUP page items, in menu order
enum SetupItem {
    SETUP_BACK, SETUP_REVERB, SETUP_DIR_A, SETUP_DIR_B, SETUP_DIR_C, SETUP_DIR_D,
    SETUP_PAN_A, SETUP_PAN_B, SETUP_PAN_C, SETUP_PAN_D, SETUP_SNAP, SETUP_GATE2,
    SETUP_DUB_FADE, SETUP_DECAY, SETUP_AGE, SETUP_WOW, SETUP_LATCH, SETUP_INPUT, SETUP_MONITOR,
    SETUP_CLEAR, NUM_SETUP_ITEMS
};
const char *setup_names[NUM_SETUP_ITEMS] = {
    "< Pages", "Reverb", "Dir A", "Dir B", "Dir C", "Dir D", "Pan A", "Pan B",
    "Pan C", "Pan D", "Snap", "Gate 2", "Dub fade", "Decay", "Age", "Wow", "Latch after", "Input",
    "Monitor", "Clear loop"
};

// SETUP settings saved to QSPI flash. Changing this struct means bumping
// kSettingsVersion, so an older saved layout is ignored instead of misread.
struct Settings {
    uint32_t version;
    int32_t reverb_mode;
    int32_t direction[kNumHeads];
    int32_t pan[kNumHeads];
    int32_t snap;
    int32_t gate2_mode;
    int32_t dub_fade;
    int32_t tape_age;
    int32_t tape_wow;
    bool decay_always;
    bool stereo_input;
    bool monitor;
    int32_t latch_loops;

    // PersistentStorage only writes flash when this reports a change.
    // CurrentSettings() zeroes padding so the byte comparison is reliable.
    bool operator!=(const Settings &other) const {
        return memcmp(this, &other, sizeof(Settings)) != 0;
    }
};
PersistentStorage<Settings> settings_storage(patch.seed.qspi);
bool settings_dirty = false;
uint32_t last_settings_change_millis = 0;

// ---------------------------------------------------------------------------
// Looper state

// EMPTY -> RECORDING (first take) -> PLAYING <-> OVERDUB, via ToggleRecording().
// Momentary punch-ins stay in PLAYING; `dub_amount` does the recording.
enum LoopState { LOOP_EMPTY, LOOP_RECORDING, LOOP_PLAYING, LOOP_OVERDUB };
const char *state_names[] = { "EMPTY", "REC", "PLAY", "DUB" };

volatile int loop_state = LOOP_EMPTY;
volatile float input_meter = 0.f; // Recent input peak, 0..1
float dub_amount = 0.f; // 0..1, ramps so dubbing punches in and out without clicks
volatile bool punching_in = false; // A momentary dub is active, for the display
volatile bool encoder_punch = false; // Encoder held for a punch-in (set in the audio callback)
// What the current encoder press does, decided by the audio callback from the
// looper state at the moment of the press
enum PressAction { PRESS_CLICK, PRESS_RECORD, PRESS_PUNCH, PRESS_LEAVE_DUB };
volatile int press_action = PRESS_CLICK;
volatile float latch_progress = -1.f; // 0..1 while a punch heads toward latching DUB, else < 0
volatile uint32_t last_input_clip_millis = 0;
volatile int loop_length = 0;
volatile int record_position = 0; // Write position during the first take
volatile int master_position = 0; // Speed-1 position, used for overdub and outputs
uint32_t last_loop_start_millis = 0;

// A head plays a window of the tape: it starts at START (fraction of the loop),
// spans LENGTH, and wraps, bounces, or jumps inside it. `position` is relative
// to the window start, in samples, and fractional for varispeed.
struct Head {
    float position = 0.f; // Offset within the head's window
    float bounce = 1.f; // Pingpong direction
};
Head heads[kNumHeads];

// Derived per block, read by the display
volatile float head_window_start[kNumHeads];
volatile float head_window_length[kNumHeads];

// Requests from the UI to the audio callback
volatile bool record_toggle_requested = false;
volatile bool clear_requested = false;
volatile int requested_page = PAGE_MIX;

// Knob pickup, owned by the audio callback (see UpdateKnobs)
int active_page = PAGE_MIX;
bool knob_caught[kNumHeads] = { true, true, true, true };
float last_knob[kNumHeads] = { 0, 0, 0, 0 };
volatile bool knob_caught_display[kNumHeads] = { true, true, true, true };

bool gate1_state = false;
bool gate2_state = false;

// Reverb state
float shimmer_feedback = 0.f; // Last wet output, pitched back into the reverb
float wet_envelope = 0.f; // Wet level, for the shimmer and freeze gain control
float shimmer_highpass_x = 0.f; // One-pole highpass state on the feedback
float shimmer_highpass_y = 0.f;
// The pitched feedback adds energy on every trip round the reverb and would run
// away to full scale; above this wet level, the feedback is turned down
const float kShimmerTargetLevel = 0.3f;
// Headroom inside the reverb; FX MIX makes it back up (knob 0..1 -> 0..2)
const float kReverbSendLevel = 0.5f;

// One-pole ~10 Hz highpass: keeps DC offsets out of recordings and out of the
// long reverb feedback, which amplifies them enormously
struct DcBlocker {
    float x = 0.f, y = 0.f;
    inline float Process(float input, float coefficient) {
        y = coefficient * (y + input - x);
        x = input;
        return y;
    }
};
DcBlocker input_dc_blocker[2];
DcBlocker send_dc_blocker[2];
// Reverse-tail readers (see ReverseProcess): each plays one window backwards
// from where the write position was when its window began
struct ReverseReader {
    int start = 0; // Write position when this reader's window began
    int phase = 0; // Samples into the window
};
int reverse_write = 0;
ReverseReader reverse_reader[2]; // Half a window apart, crossfaded

// Tape aging: one-pole lowpass state along the loop, per channel
float age_lowpass[2] = { 0.f, 0.f };
// Tape wow (slow) and flutter (fast) oscillator phases, in cycles
float wow_phase = 0.f;
float flutter_phase = 0.f;

// Encoder events latched in the audio callback (libDaisy's debounce needs ~1 kHz)
volatile int32_t enc_delta_pending = 0;
volatile bool enc_rise_pending = false;
volatile bool enc_fall_pending = false;

// Waveform overview, one peak per screen column
float waveform[128];
bool waveform_dirty = false;
uint32_t last_waveform_millis = 0;
uint32_t last_screen_update_millis = 0;

// ---------------------------------------------------------------------------
// Helpers

// Transparent below the knee, then rounds peaks off smoothly toward +/-1.
// Used on every output and on overdub writes. It's a static curve (no attack or
// release), so it can't pump, but a hugely hot signal is flattened toward 1.
inline float PeakLimit(float x) {
    const float knee = 0.8f;
    float magnitude = fabsf(x);
    if (magnitude <= knee) {
        return x;
    }
    float over = (magnitude - knee) / (1.f - knee);
    float limited = knee + (1.f - knee) * over / (1.f + over);
    return x < 0.f ? -limited : limited;
}

// Soft clipper (rational tanh approximation), exactly +/-1 beyond +/-3. Colors
// even moderate levels, so it's used where saturation is the point: the
// reverb send and tape Age.
inline float Saturate(float x) {
    if (x > 3.f) return 1.f;
    if (x < -3.f) return -1.f;
    return x * (27.f + x * x) / (27.f + 9.f * x * x);
}

inline int WrapIndex(int index, int length) {
    index %= length;
    return index < 0 ? index + length : index;
}

// Reads the tape at a fractional position with linear interpolation, wrapping
// around the loop (heads can point anywhere, including past the end)
inline float ReadLoop(int channel, float index) {
    int length = loop_length;
    int i0 = (int)floorf(index);
    float fraction = index - i0;
    i0 = WrapIndex(i0, length);
    int i1 = i0 + 1 >= length ? 0 : i0 + 1;
    const float *buffer = loop_buffer[channel];
    return buffer[i0] + (buffer[i1] - buffer[i0]) * fraction;
}

// Knob 0..1 -> speed: left half reverse, right half forward, 0.25x..2x each way
// (exponential, so equal knob distances are equal musical intervals). Snap
// picks the nearest allowed ratio in pitch terms (log2).
float SpeedFromKnob(float knob) {
    float t = fabsf(knob - 0.5f) * 2.f;
    float speed = 0.25f * powf(8.f, t);
    if (snap != SNAP_OFF) {
        static const float octaves[] = { 0.25f, 0.5f, 1.f, 2.f };
        static const float musical[] = { 0.25f, 0.375f, 0.5f, 0.75f, 1.f, 1.5f, 2.f };
        const float *choices = snap == SNAP_OCTAVES ? octaves : musical;
        int count = snap == SNAP_OCTAVES ? 4 : 7;
        float best = choices[0];
        for (int i = 1; i < count; i++) {
            if (fabsf(log2f(choices[i] / speed)) < fabsf(log2f(best / speed))) {
                best = choices[i];
            }
        }
        speed = best;
    }
    return knob < 0.5f ? -speed : speed;
}

// Knob 0..1 -> fraction of the loop, squared for fine control of short windows
inline float LengthFraction(float knob) {
    return 0.01f + 0.99f * knob * knob;
}

// ---------------------------------------------------------------------------
// Looper control (audio callback)

// Steps the looper state machine. Called from the audio callback only (for the
// encoder press, GATE IN 1, UI requests, and a full buffer).
void ToggleRecording() {
    switch (loop_state) {
        case LOOP_EMPTY:
            record_position = 0;
            loop_length = 0;
            loop_state = LOOP_RECORDING;
            break;
        case LOOP_RECORDING: {
            // Close the loop: its length is however much was recorded
            int length = record_position;
            if (length < kMinLoopSamples) { // Too short to be a loop: discard
                loop_state = LOOP_EMPTY;
                break;
            }
            // Fade out the end of the take so the loop seam doesn't click
            int fade = std::min(kEdgeFadeSamples, length);
            for (int i = 0; i < fade; i++) {
                float gain = (float)i / fade;
                loop_buffer[0][length - 1 - i] *= gain;
                loop_buffer[1][length - 1 - i] *= gain;
            }
            loop_length = length;
            master_position = 0;
            for (int h = 0; h < kNumHeads; h++) {
                heads[h].position = 0.f;
                heads[h].bounce = 1.f;
            }
            loop_state = LOOP_PLAYING;
            last_loop_start_millis = System::GetNow();
            waveform_dirty = true;
            break;
        }
        case LOOP_PLAYING:
            loop_state = LOOP_OVERDUB;
            break;
        case LOOP_OVERDUB:
            loop_state = LOOP_PLAYING;
            waveform_dirty = true;
            break;
    }
}

void ClearLoop() {
    loop_state = LOOP_EMPTY;
    loop_length = 0;
    record_position = 0;
    master_position = 0;
    waveform_dirty = true;
}

// Page changes and knob pickup. Each page stores its own four values, but there
// are only four physical knobs, so after a page change a knob doesn't take over
// until it reaches the stored value: either it's already within tolerance, or
// it has crossed the value since the last block (the sign of knob - value
// flipped). Until then the display shows a dot next to that value.
void UpdateKnobs() {
    int page = requested_page;
    if (page != active_page) {
        active_page = page;
        for (int k = 0; k < kNumHeads; k++) {
            knob_caught[k] = false;
        }
    }
    if (active_page >= kNumKnobPages) {
        return; // SETUP page: knobs do nothing
    }
    for (int k = 0; k < kNumHeads; k++) {
        float knob = patch.GetKnobValue((DaisyPatch::Ctrl)k);
        float &value = page_value[active_page][k];
        if (!knob_caught[k]) {
            bool crossed = (last_knob[k] - value) * (knob - value) <= 0.f;
            if (fabsf(knob - value) < kPickupTolerance || crossed) {
                knob_caught[k] = true;
            }
        }
        if (knob_caught[k]) {
            value = knob;
        }
        last_knob[k] = knob;
        knob_caught_display[k] = knob_caught[k];
    }
}

// ---------------------------------------------------------------------------
// Reverb

// Plays the last `window` samples backwards with two overlapping readers
// (a classic reverse delay). Each reader starts at the current write position
// and walks backwards through the window just recorded, under a Hann envelope;
// the second reader runs half a window behind, and two Hann windows half a
// period apart sum to a constant, so the output doesn't pulse. Reading
// backwards while writing forwards needs 2x the window in buffer.
float ReverseProcess(int channel, float input, int window) {
    reverse_buffer[channel][reverse_write] = input;
    float out = 0.f;
    for (int r = 0; r < 2; r++) {
        ReverseReader &reader = reverse_reader[r];
        // Both channels share the readers' timing
        float envelope = 0.5f - 0.5f * cosf(2.f * PI_F * reader.phase / window);
        int index = WrapIndex(reader.start - reader.phase, kReverseBufferSamples);
        out += reverse_buffer[channel][index] * envelope;
    }
    return out;
}

// Steps both readers and the write position (once per sample, after both
// channels have been processed)
void AdvanceReverse(int window) {
    ReverseReader &first = reverse_reader[0];
    ReverseReader &second = reverse_reader[1];
    if (++first.phase >= window) {
        first.phase = 0;
        first.start = reverse_write;
        // Re-sync the second reader half a window behind, so the two
        // envelopes keep summing to a constant level if the window changes
        second.phase = window / 2;
        second.start = WrapIndex(reverse_write - window / 2, kReverseBufferSamples);
    } else if (++second.phase >= window) {
        second.phase = 0;
        second.start = reverse_write;
    }
    reverse_write = (reverse_write + 1) % kReverseBufferSamples;
}

// Zeroes and re-initializes the reverb and shifter (they live in SDRAM)
void ResetReverb() {
    memset((void *)&reverb, 0, sizeof(reverb));
    memset((void *)&shifter, 0, sizeof(shifter));
    reverb.Init(sample_rate);
    shifter.Init(sample_rate);
    shimmer_feedback = 0.f;
    wet_envelope = 0.f;
    shimmer_highpass_x = shimmer_highpass_y = 0.f;
}

// ---------------------------------------------------------------------------
// Audio

// Runs every 48 samples (1 kHz). Order:
//   1. Controls: encoder edges (acted on immediately), knobs/pickup, gates, UI requests
//   2. Per-block parameters: heads, reverb, tape, input levels
//   3. Per sample: input mix -> record/overdub write -> heads -> reverb -> outputs
//   4. CV and gate outputs
void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size) {
    patch.ProcessAnalogControls();
    patch.encoder.Debounce();
    enc_delta_pending += patch.encoder.Increment();
    if (patch.encoder.RisingEdge()) {
        // Act on the press itself (not the release) so takes and punch-ins land on
        // the beat. On the SETUP page presses are only clicks.
        if (requested_page == PAGE_SETUP) {
            press_action = PRESS_CLICK;
        } else {
            switch (loop_state) {
                case LOOP_EMPTY:
                case LOOP_RECORDING:
                    press_action = PRESS_RECORD;
                    ToggleRecording(); // Start the first take, or close the loop
                    break;
                case LOOP_PLAYING:
                    press_action = PRESS_PUNCH;
                    encoder_punch = true;
                    break;
                default:
                    press_action = PRESS_LEAVE_DUB; // Acts on release
                    break;
            }
        }
        enc_rise_pending = true;
    }
    if (patch.encoder.FallingEdge()) {
        enc_fall_pending = true;
        encoder_punch = false;
    }

    UpdateKnobs();

    // GATE IN 1 triggers step the state machine; the UI requests steps too
    // (latching DUB after a long hold, a click leaving DUB)
    bool gate1 = patch.gate_input[DaisyPatch::GATE_IN_1].State();
    if ((gate1 && !gate1_state) || record_toggle_requested) {
        record_toggle_requested = false;
        ToggleRecording();
    }
    gate1_state = gate1;
    if (clear_requested) {
        clear_requested = false;
        ClearLoop();
    }

    // GATE IN 2
    bool gate2 = patch.gate_input[DaisyPatch::GATE_IN_2].State();
    bool gate2_rise = gate2 && !gate2_state;
    gate2_state = gate2;
    bool reverse_all = gate2_mode == GATE2_REVERSE && gate2;

    // Head parameters
    int length = loop_length;
    bool playing = (loop_state == LOOP_PLAYING || loop_state == LOOP_OVERDUB) && length > 0;
    float level[kNumHeads], speed[kNumHeads], gain_l[kNumHeads], gain_r[kNumHeads];
    float window_start[kNumHeads], window_length[kNumHeads];
    for (int h = 0; h < kNumHeads; h++) {
        level[h] = page_value[PAGE_MIX][h];
        speed[h] = SpeedFromKnob(page_value[PAGE_SPEED][h]);
        if (direction[h] == DIR_REVERSE) speed[h] = -speed[h];
        if (reverse_all) speed[h] = -speed[h];
        window_start[h] = page_value[PAGE_START][h] * length;
        window_length[h] = std::min((float)length,
            std::max((float)kMinWindowSamples, LengthFraction(page_value[PAGE_LENGTH][h]) * length));
        head_window_start[h] = window_start[h];
        head_window_length[h] = window_length[h];

        float p = (pan[h] + 100) / 200.f; // 0 = left, 1 = right
        // Balance-style pan: centre is full level on both sides, never louder
        gain_l[h] = level[h] * std::min(1.f, 2.f * (1.f - p));
        gain_r[h] = level[h] * std::min(1.f, 2.f * p);

        if (playing && gate2_rise) {
            if (gate2_mode == GATE2_RETRIGGER) {
                heads[h].position = speed[h] >= 0.f ? 0.f : window_length[h] - 1.f;
            } else if (gate2_mode == GATE2_SCATTER) {
                heads[h].position = rand() * kRandFrac * window_length[h];
            }
        }
    }

    // Reverb parameters (FX page: MIX, DECAY, TONE, ODD). ODD ("strange") means
    // something different in each mode: pre-delay, shimmer amount, reverse
    // window, or freeze fill rate.
    float fx_mix = page_value[PAGE_FX][0] * 2.f; // Makes up for kReverbSendLevel
    const float dc_coefficient = 1.f - 2.f * PI_F * 10.f / sample_rate;
    float decay = page_value[PAGE_FX][1];
    float tone = page_value[PAGE_FX][2];
    float strange = page_value[PAGE_FX][3];
    bool freeze = reverb_mode == REVERB_FREEZE;
    reverb.SetFeedback(freeze ? 0.999f : 0.6f + decay * 0.38f);
    reverb.SetLpFreq(500.f * powf(36.f, tone));
    // Freeze feeds back at 0.999, so input accumulates ~1000x: ODD lets a trickle
    // in (0 = fully frozen), throttled as the reverb fills so it levels off
    float input_gain = freeze
        ? strange * 0.5f * std::min(1.f, kShimmerTargetLevel / std::max(wet_envelope, 1e-6f))
        : 1.f;
    float shimmer_amount = 0.f;
    // Highpass on the feedback keeps low end from piling up (lower for Sub)
    float shimmer_highpass = 1.f - 2.f * PI_F * (reverb_mode == REVERB_SUB ? 30.f : 150.f) / sample_rate;
    if (reverb_mode == REVERB_SHIMMER || reverb_mode == REVERB_SUB || reverb_mode == REVERB_GHOST) {
        shimmer_amount = strange * 0.7f;
        shifter.SetTransposition(reverb_mode == REVERB_SUB ? -12.f : 12.f);
    }
    const float envelope_attack = 1.f - expf(-1.f / (0.005f * sample_rate)); // 5 ms
    const float envelope_release = 1.f - expf(-1.f / (0.3f * sample_rate)); // 300 ms
    bool reversed_tail = reverb_mode == REVERB_BACKWARDS || reverb_mode == REVERB_GHOST;
    int reverse_window = reverb_mode == REVERB_GHOST
        ? 38400 // 800 ms
        : 4800 + (int)(strange * 67200); // 100 ms..1.5 s
    if (reverb_mode == REVERB_PLATE) {
        float delay = std::max(1.f, strange * (kPreDelaySamples - 1));
        pre_delay[0].SetDelay(delay);
        pre_delay[1].SetDelay(delay);
    }
    // Dubbing: DUB state, or a momentary punch-in (encoder held, or GATE IN 2
    // high with Gate 2 set to Dub) while in PLAY
    bool punch = loop_state == LOOP_PLAYING
        && ((gate2_mode == GATE2_DUB && gate2) || encoder_punch);
    punching_in = punch;
    float dub_target = (loop_state == LOOP_OVERDUB || punch) ? 1.f : 0.f;
    if (!playing) {
        dub_amount = 0.f;
    }
    const float dub_ramp = 1.f / 480.f; // 10 ms

    // Tape: the loop is rewritten while dubbing, and in PLAY too with Decay: Always
    float fade = dub_fade / 100.f;
    bool rewrite = playing && (dub_target > 0.f || dub_amount > 0.f || decay_always);

    // Input levels (INPUT page)
    float input_level[4];
    for (int k = 0; k < 4; k++) {
        input_level[k] = page_value[PAGE_INPUT][k];
    }
    float age = tape_age / 100.f;
    // Age darkens each pass (down to a ~2.5 kHz one-pole at 100%) and saturates it
    float age_coefficient = 1.f - age * 0.72f;
    float age_drive = 1.f + age * 2.f;
    float wow_depth = tape_wow / 100.f * 0.008f; // Up to +/-0.8% (about 14 cents)
    float flutter_depth = tape_wow / 100.f * 0.0015f;
    const float wow_increment = 0.6f / sample_rate; // 0.6 Hz
    const float flutter_increment = 7.f / sample_rate; // 7 Hz

    float block_input_peak = 0.f;
    for (size_t n = 0; n < size; n++) {
        // Stereo: IN 1 + IN 3 left, IN 2 + IN 4 right. Mono: all four to both.
        float in_1 = in[0][n] * input_level[0];
        float in_2 = in[1][n] * input_level[1];
        float in_3 = in[2][n] * input_level[2];
        float in_4 = in[3][n] * input_level[3];
        float mix_l = stereo_input ? in_1 + in_3 : in_1 + in_2 + in_3 + in_4;
        float mix_r = stereo_input ? in_2 + in_4 : mix_l;
        float in_l = input_dc_blocker[0].Process(mix_l, dc_coefficient);
        float in_r = input_dc_blocker[1].Process(mix_r, dc_coefficient);

        // Recording: the first take writes straight in (fading in over the first
        // 5 ms); afterwards the tape is rewritten at the speed-1 master position
        if (loop_state == LOOP_RECORDING) {
            int pos = record_position;
            float fade_in = std::min(1.f, (float)pos / kEdgeFadeSamples);
            loop_buffer[0][pos] = in_l * fade_in;
            loop_buffer[1][pos] = in_r * fade_in;
            record_position = pos + 1;
            if (record_position >= kMaxLoopSamples) {
                ToggleRecording(); // Buffer full: close the loop
            }
        } else if (rewrite) {
            int pos = master_position;
            if (dub_amount < dub_target) {
                dub_amount = std::min(dub_target, dub_amount + dub_ramp);
            } else if (dub_amount > dub_target) {
                dub_amount = std::max(dub_target, dub_amount - dub_ramp);
            }
            // With Decay: Dub only, the loop fades only while dubbing
            float keep = 1.f - fade * (decay_always ? 1.f : dub_amount);
            for (int c = 0; c < 2; c++) {
                // new = aged(old) * keep + input * dub_amount
                float old = loop_buffer[c][pos];
                if (age > 0.f) {
                    // Filtering along the loop darkens it a little more each pass
                    age_lowpass[c] += age_coefficient * (old - age_lowpass[c]);
                    old = age_lowpass[c];
                    old += (Saturate(old * age_drive) / age_drive - old) * age * 0.5f;
                }
                float input = (c == 0 ? in_l : in_r) * dub_amount;
                // Limited so a source left running in DUB can't build up forever
                loop_buffer[c][pos] = PeakLimit(old * keep + input);
            }
        }

        // Tape wow and flutter bend every head's speed together
        float wobble = 1.f;
        if (tape_wow > 0) {
            wow_phase += wow_increment;
            if (wow_phase >= 1.f) wow_phase -= 1.f;
            flutter_phase += flutter_increment;
            if (flutter_phase >= 1.f) flutter_phase -= 1.f;
            wobble += wow_depth * sinf(2.f * PI_F * wow_phase)
                + flutter_depth * sinf(2.f * PI_F * flutter_phase);
        }

        // Playheads: read, fade at the window edges (so wrapping doesn't click),
        // pan, then advance and handle the window boundary per direction mode
        float heads_l = 0.f, heads_r = 0.f;
        if (playing) {
            for (int h = 0; h < kNumHeads; h++) {
                Head &head = heads[h];
                float wl = window_length[h];
                if (level[h] > 0.001f) {
                    float index = window_start[h] + head.position;
                    float fade = std::min((float)kEdgeFadeSamples, wl * 0.25f);
                    float edge = std::min(head.position, wl - head.position);
                    float envelope = std::min(1.f, std::max(0.f, edge / fade));
                    heads_l += ReadLoop(0, index) * envelope * gain_l[h];
                    heads_r += ReadLoop(1, index) * envelope * gain_r[h];
                }

                head.position += speed[h] * head.bounce * wobble;
                if (head.position >= wl || head.position < 0.f) {
                    switch (direction[h]) {
                        case DIR_PINGPONG: // Reflect off the edge and reverse
                            head.bounce = -head.bounce;
                            head.position = head.position >= wl
                                ? std::max(0.f, 2.f * wl - head.position - 1.f)
                                : std::min(wl - 1.f, -head.position);
                            break;
                        case DIR_RANDOM: // Jump somewhere new in the window
                            head.position = rand() * kRandFrac * wl;
                            break;
                        default: // Forward/Reverse: wrap around the window
                            head.position = fmodf(head.position, wl);
                            if (head.position < 0.f) head.position += wl;
                            break;
                    }
                }
            }

            // The master position always moves at speed 1: it's where overdubs
            // and fading are written, and what CV OUT 2 and GATE OUT follow
            int next = master_position + 1;
            if (next >= length) {
                next = 0;
                last_loop_start_millis = System::GetNow();
            }
            master_position = next;
        }

        // Reverb: send the heads (DC-blocked, with headroom) plus any pitched
        // feedback, soft-clipped so nothing can overload the reverb input
        float send_l = Saturate(send_dc_blocker[0].Process(heads_l, dc_coefficient) * input_gain * kReverbSendLevel
            + shimmer_feedback * shimmer_amount);
        float send_r = Saturate(send_dc_blocker[1].Process(heads_r, dc_coefficient) * input_gain * kReverbSendLevel
            + shimmer_feedback * shimmer_amount);
        if (reverb_mode == REVERB_PLATE) {
            pre_delay[0].Write(send_l);
            pre_delay[1].Write(send_r);
            send_l = pre_delay[0].Read();
            send_r = pre_delay[1].Read();
        }
        float wet_l, wet_r;
        reverb.Process(send_l, send_r, &wet_l, &wet_r);
        // Track the wet level; Shimmer/Sub/Ghost and Freeze turn their feedback
        // or input down as it approaches kShimmerTargetLevel, so they level off
        // instead of running away to full scale
        float mono = 0.5f * (wet_l + wet_r);
        float level = fabsf(mono);
        wet_envelope += (level > wet_envelope ? envelope_attack : envelope_release)
            * (level - wet_envelope);
        if (shimmer_amount > 0.f) {
            float shifted = shifter.Process(mono);
            // One-pole highpass
            shimmer_highpass_y = shimmer_highpass * (shimmer_highpass_y + shifted - shimmer_highpass_x);
            shimmer_highpass_x = shifted;
            float gain_control = std::min(1.f, kShimmerTargetLevel / std::max(wet_envelope, 1e-6f));
            shimmer_feedback = shimmer_highpass_y * gain_control;
        } else {
            shimmer_feedback = 0.f;
        }
        // A NaN/Inf would stay in the feedback loop and silence everything:
        // reset the reverb instead
        if (!std::isfinite(wet_l) || !std::isfinite(wet_r) || !std::isfinite(shimmer_feedback)) {
            ResetReverb();
            wet_l = wet_r = 0.f;
        }
        if (reversed_tail) {
            wet_l = ReverseProcess(0, wet_l, reverse_window);
            wet_r = ReverseProcess(1, wet_r, reverse_window);
            AdvanceReverse(reverse_window);
        }

        float dry_l = monitor ? in_l : 0.f;
        float dry_r = monitor ? in_r : 0.f;
        out[0][n] = PeakLimit(heads_l + wet_l * fx_mix + dry_l);
        out[1][n] = PeakLimit(heads_r + wet_r * fx_mix + dry_r);
        out[2][n] = PeakLimit(wet_l);
        out[3][n] = PeakLimit(wet_r);

        // Input meter
        float input_peak = std::max(fabsf(in_l), fabsf(in_r));
        if (input_peak > block_input_peak) {
            block_input_peak = input_peak;
        }
    }

    // Meter falls back ~20 dB per second; clip warning holds for half a second
    input_meter = std::max(block_input_peak, input_meter * 0.9975f);
    if (block_input_peak > 0.95f) {
        last_input_clip_millis = System::GetNow();
    }

    // CV OUT 1: head A position, CV OUT 2: loop progress, GATE OUT: loop start
    if (playing) {
        float a = (head_window_start[0] + heads[0].position) / length;
        a -= floorf(a);
        patch.seed.dac.WriteValue(DacHandle::Channel::ONE, (uint16_t)(a * 4095));
        patch.seed.dac.WriteValue(DacHandle::Channel::TWO, (uint16_t)((float)master_position / length * 4095));
    } else {
        patch.seed.dac.WriteValue(DacHandle::Channel::ONE, 0);
        patch.seed.dac.WriteValue(DacHandle::Channel::TWO, 0);
    }
    patch.gate_output.Write(playing && System::GetNow() - last_loop_start_millis < kLoopPulseMillis);
}

// ---------------------------------------------------------------------------
// Settings persistence: SETUP settings are saved to the end of the QSPI flash
// (far from the app at 0x90040000) a couple of seconds after the last change

Settings CurrentSettings() {
    Settings s;
    memset(&s, 0, sizeof(s)); // Zero padding so the byte comparison is stable
    s.version = kSettingsVersion;
    s.reverb_mode = reverb_mode;
    for (int h = 0; h < kNumHeads; h++) {
        s.direction[h] = direction[h];
        s.pan[h] = pan[h];
    }
    s.snap = snap;
    s.gate2_mode = gate2_mode;
    s.dub_fade = dub_fade;
    s.tape_age = tape_age;
    s.tape_wow = tape_wow;
    s.decay_always = decay_always;
    s.stereo_input = stereo_input;
    s.monitor = monitor;
    s.latch_loops = latch_loops;
    return s;
}

// The globals' initial values are the defaults. A saved layout from another
// version is ignored, and every value is range-checked before it's used.
void LoadSettings() {
    settings_storage.Init(CurrentSettings(), kSettingsQspiOffset);
    Settings &s = settings_storage.GetSettings();
    if (s.version != kSettingsVersion) {
        return;
    }
    auto in_range = [](int32_t value, int32_t low, int32_t high) { return value >= low && value <= high; };
    if (in_range(s.reverb_mode, 0, NUM_REVERB_MODES - 1)) reverb_mode = s.reverb_mode;
    for (int h = 0; h < kNumHeads; h++) {
        if (in_range(s.direction[h], 0, NUM_DIRECTIONS - 1)) direction[h] = s.direction[h];
        if (in_range(s.pan[h], -100, 100)) pan[h] = s.pan[h];
    }
    if (in_range(s.snap, 0, NUM_SNAPS - 1)) snap = s.snap;
    if (in_range(s.gate2_mode, 0, NUM_GATE2_MODES - 1)) gate2_mode = s.gate2_mode;
    if (in_range(s.dub_fade, 0, 50)) dub_fade = s.dub_fade;
    if (in_range(s.tape_age, 0, 100)) tape_age = s.tape_age;
    if (in_range(s.tape_wow, 0, 100)) tape_wow = s.tape_wow;
    decay_always = s.decay_always;
    stereo_input = s.stereo_input;
    monitor = s.monitor;
    if (s.latch_loops == 0 || s.latch_loops == 1 || s.latch_loops == 2 || s.latch_loops == 4) latch_loops = s.latch_loops;
}

void MarkSettingsChanged() {
    settings_dirty = true;
    last_settings_change_millis = System::GetNow();
}

void SaveSettingsIfNeeded() {
    if (!settings_dirty || System::GetNow() - last_settings_change_millis < kSettingsSaveDelayMillis) {
        return;
    }
    settings_dirty = false;
    settings_storage.GetSettings() = CurrentSettings();
    settings_storage.Save();
}

// ---------------------------------------------------------------------------
// UI

// UI_PAGES: turning changes page. On the SETUP page a click enters UI_SETUP_NAV
// (turning picks an item); a click on an item enters UI_SETUP_EDIT (turning
// changes it) or, for on/off items and actions, applies it directly.
enum UiMode { UI_PAGES, UI_SETUP_NAV, UI_SETUP_EDIT };
int ui_mode = UI_PAGES;
int page = PAGE_MIX;
int last_play_page = PAGE_MIX; // Where the SETUP timeout returns to
uint32_t last_encoder_activity_millis = 0;
int setup_item = SETUP_REVERB;
bool encoder_press_armed = false;
bool encoder_hold_fired = false;

inline int Wrap(int value, int count) {
    return ((value % count) + count) % count;
}

void AdjustSetupValue(int amount) {
    switch (setup_item) {
        case SETUP_REVERB: reverb_mode = Wrap(reverb_mode + amount, NUM_REVERB_MODES); break;
        case SETUP_DIR_A: case SETUP_DIR_B: case SETUP_DIR_C: case SETUP_DIR_D: {
            int &d = direction[setup_item - SETUP_DIR_A];
            d = Wrap(d + amount, NUM_DIRECTIONS);
            break;
        }
        case SETUP_PAN_A: case SETUP_PAN_B: case SETUP_PAN_C: case SETUP_PAN_D: {
            int &p = pan[setup_item - SETUP_PAN_A];
            p = std::max(-100, std::min(100, p + amount * 10));
            break;
        }
        case SETUP_SNAP: snap = Wrap(snap + amount, NUM_SNAPS); break;
        case SETUP_GATE2: gate2_mode = Wrap(gate2_mode + amount, NUM_GATE2_MODES); break;
        case SETUP_DUB_FADE: dub_fade = std::max(0, std::min(50, dub_fade + amount)); break;
        case SETUP_DECAY: decay_always = !decay_always; break;
        case SETUP_AGE: tape_age = std::max(0, std::min(100, tape_age + amount * 5)); break;
        case SETUP_WOW: tape_wow = std::max(0, std::min(100, tape_wow + amount * 5)); break;
        case SETUP_INPUT: stereo_input = !stereo_input; break;
        case SETUP_MONITOR: monitor = !monitor; break;
        case SETUP_LATCH: {
            // 1, 2, 4 loops, Never
            static const int choices[] = { 1, 2, 4, 0 };
            int index = 0;
            while (choices[index] != latch_loops) index++;
            latch_loops = choices[Wrap(index + amount, 4)];
            break;
        }
        default: return;
    }
    MarkSettingsChanged();
}

void OnEncoderClick() {
    switch (ui_mode) {
        case UI_PAGES:
            if (page == PAGE_SETUP) {
                ui_mode = UI_SETUP_NAV;
                setup_item = SETUP_REVERB;
            }
            break;
        case UI_SETUP_NAV:
            if (setup_item == SETUP_BACK) {
                ui_mode = UI_PAGES;
            } else if (setup_item == SETUP_CLEAR) {
                clear_requested = true;
            } else if (setup_item == SETUP_INPUT || setup_item == SETUP_MONITOR
                       || setup_item == SETUP_DECAY) {
                AdjustSetupValue(1); // Toggles
            } else {
                ui_mode = UI_SETUP_EDIT;
            }
            break;
        case UI_SETUP_EDIT:
            ui_mode = UI_SETUP_NAV;
            break;
    }
}

// Main-loop half of the encoder: takes the events the audio callback latched,
// times holds, handles clicks and turns, and the SETUP timeout
void UpdateUi() {
    __disable_irq();
    int enc = enc_delta_pending;
    bool enc_rise = enc_rise_pending;
    bool enc_fall = enc_fall_pending;
    enc_delta_pending = 0;
    enc_rise_pending = false;
    enc_fall_pending = false;
    __enable_irq();

    // Encoder presses on the play pages (the audio callback acts on the press):
    //   EMPTY / REC: a press starts the first take / closes the loop
    //   PLAY: records while held (the audio callback punches in on the press);
    //         holding past `Latch after` loops (4 s minimum) latches DUB
    //   DUB: a click goes back to PLAY
    // On the SETUP page every press is a click, so navigating can't dub.
    if (enc_rise) {
        encoder_press_armed = true;
        // Recording presses already acted in the audio callback
        encoder_hold_fired = press_action == PRESS_RECORD;
    }
    latch_progress = -1.f;
    if (encoder_press_armed && !encoder_hold_fired && patch.encoder.Pressed()) {
        uint32_t held = patch.encoder.TimeHeldMs();
        if (press_action == PRESS_PUNCH && latch_loops > 0 && loop_length > 0) {
            float loop_millis = loop_length / sample_rate * 1000.f;
            float threshold = std::max((float)kLatchMinimumMillis, loop_millis * latch_loops);
            latch_progress = std::min(1.f, held / threshold);
            if (held >= threshold) {
                encoder_hold_fired = true;
                record_toggle_requested = true; // PLAY -> DUB
            }
        }
    }
    if (enc_fall) {
        if (encoder_press_armed && !encoder_hold_fired) {
            if (press_action == PRESS_CLICK) {
                OnEncoderClick();
            } else if (press_action == PRESS_LEAVE_DUB) {
                record_toggle_requested = true; // DUB -> PLAY
            }
        }
        encoder_press_armed = false;
    }

    if (enc != 0) {
        switch (ui_mode) {
            case UI_PAGES:
                page = std::max(0, std::min(NUM_PAGES - 1, page + enc));
                requested_page = page;
                if (page != PAGE_SETUP) {
                    last_play_page = page;
                }
                break;
            case UI_SETUP_NAV:
                setup_item = std::max(0, std::min(NUM_SETUP_ITEMS - 1, setup_item + enc));
                break;
            case UI_SETUP_EDIT:
                AdjustSetupValue(enc);
                break;
        }
    }

    // Leave SETUP after a few idle seconds so the play pages come back.
    // Holding the encoder (e.g. a long punch-in) counts as activity.
    uint32_t now = System::GetNow();
    if (enc != 0 || enc_rise || enc_fall || patch.encoder.Pressed()) {
        last_encoder_activity_millis = now;
    }
    if (page == PAGE_SETUP && now - last_encoder_activity_millis >= kSetupTimeoutMillis) {
        ui_mode = UI_PAGES;
        page = last_play_page;
        requested_page = page;
    }
}

// ---------------------------------------------------------------------------
// Display (128 x 64):
//   y 0-7    page name, input meter (+ clip block), looper state and length
//   y 8      latch progress bar while a punch is held
//   y 9-26   waveform, with the master position as a vertical line
//   y 28-43  one lane per head: its window (dotted if muted) and position
//   y 46-63  the page's four labels and values, or the SETUP item

const int kWaveTop = 9;
const int kWaveBottom = 26;
const int kLaneTop = 28;
const int kLabelY = 46;
const int kValueY = 56;

// Rebuilds the waveform overview from the tape (peak per screen column,
// sampling every 64th sample, which is plenty for one pixel)
void RefreshWaveform() {
    int length = loop_length;
    memset(waveform, 0, sizeof(waveform));
    if (length <= 0) {
        return;
    }
    for (int x = 0; x < 128; x++) {
        int begin = (int)((int64_t)x * length / 128);
        int end = (int)((int64_t)(x + 1) * length / 128);
        float peak = 0.f;
        for (int i = begin; i < end; i += 64) { // Sparse scan is plenty for a 1-pixel column
            peak = std::max(peak, fabsf(loop_buffer[0][i]) + fabsf(loop_buffer[1][i]));
        }
        waveform[x] = peak * 0.5f;
    }
}

void SetupValueText(int item, char *text, size_t size) {
    switch (item) {
        case SETUP_REVERB: snprintf(text, size, "%s", reverb_names[reverb_mode]); break;
        case SETUP_DIR_A: case SETUP_DIR_B: case SETUP_DIR_C: case SETUP_DIR_D:
            snprintf(text, size, "%s", direction_names[direction[item - SETUP_DIR_A]]); break;
        case SETUP_PAN_A: case SETUP_PAN_B: case SETUP_PAN_C: case SETUP_PAN_D:
            snprintf(text, size, "%+d", pan[item - SETUP_PAN_A]); break;
        case SETUP_SNAP: snprintf(text, size, "%s", snap_names[snap]); break;
        case SETUP_GATE2: snprintf(text, size, "%s", gate2_names[gate2_mode]); break;
        case SETUP_DUB_FADE: snprintf(text, size, "%d%%", dub_fade); break;
        case SETUP_DECAY: snprintf(text, size, "%s", decay_always ? "Always" : "Dub only"); break;
        case SETUP_AGE: snprintf(text, size, "%d%%", tape_age); break;
        case SETUP_WOW: snprintf(text, size, "%d%%", tape_wow); break;
        case SETUP_INPUT: snprintf(text, size, "%s", stereo_input ? "Stereo" : "Mono"); break;
        case SETUP_MONITOR: snprintf(text, size, "%s", monitor ? "On" : "Off"); break;
        case SETUP_LATCH:
            if (latch_loops == 0) snprintf(text, size, "Never");
            else snprintf(text, size, "%d loop%s", latch_loops, latch_loops == 1 ? "" : "s");
            break;
        default: text[0] = 0; break;
    }
}

void PageValueText(int p, int column, char *text, size_t size) {
    float v = page_value[p][column];
    switch (p) {
        case PAGE_SPEED: {
            float s = SpeedFromKnob(v);
            if (direction[column] == DIR_REVERSE) s = -s;
            // newlib-nano printf has no floats: print hundredths, trim zeros
            int hundredths = (int)(fabsf(s) * 100 + 0.5f);
            char decimals[4];
            snprintf(decimals, sizeof(decimals), "%02d", hundredths % 100);
            if (decimals[1] == '0') decimals[1] = 0;
            if (decimals[0] == '0' && decimals[1] == 0) decimals[0] = 0;
            snprintf(text, size, "%c%d%s%s", s < 0 ? '-' : '+', hundredths / 100,
                     decimals[0] ? "." : "", decimals);
            break;
        }
        case PAGE_LENGTH:
            snprintf(text, size, "%d%%", (int)(LengthFraction(v) * 100 + 0.5f));
            break;
        default:
            snprintf(text, size, "%d", (int)(v * 100 + 0.5f));
            break;
    }
}

void DrawScreen() {
    auto &d = patch.display;
    char text[32];
    d.Fill(false);

    // Header: page and looper state
    d.SetCursor(0, 0);
    d.WriteString(page_names[page], Font_6x8, true);

    // Input meter between the page name and the state; solid end block = clipping
    const int meter_x = 40, meter_width = 26;
    d.DrawRect(meter_x, 1, meter_x + meter_width, 6, true, false);
    int fill = (int)(std::min(1.f, (float)input_meter) * (meter_width - 2));
    if (fill > 0) {
        d.DrawRect(meter_x + 1, 2, meter_x + 1 + fill, 5, true, true);
    }
    if (System::GetNow() - last_input_clip_millis < 500 && last_input_clip_millis != 0) {
        d.DrawRect(meter_x + meter_width + 2, 0, meter_x + meter_width + 4, 7, true, true);
    }
    int state = loop_state;
    float seconds = (state == LOOP_RECORDING ? record_position : loop_length) / sample_rate;
    if (state == LOOP_PLAYING && punching_in) {
        state = LOOP_OVERDUB; // Momentary punch-ins show as DUB
    }
    if (state == LOOP_EMPTY) {
        snprintf(text, sizeof(text), "%s", state_names[state]);
    } else {
        int tenths = (int)(seconds * 10 + 0.5f);
        snprintf(text, sizeof(text), "%s %d.%ds", state_names[state], tenths / 10, tenths % 10);
    }
    int width = 6 * strlen(text);
    if (state == LOOP_RECORDING || state == LOOP_OVERDUB) {
        d.DrawRect(128 - width - 2, 0, 127, 7, true, true);
        d.SetCursor(128 - width - 1, 0);
        d.WriteString(text, Font_6x8, false);
    } else {
        d.SetCursor(128 - width, 0);
        d.WriteString(text, Font_6x8, true);
    }

    // A held punch heading toward latching DUB: bar fills along the top
    float progress = latch_progress;
    if (progress >= 0.f) {
        d.DrawLine(0, kWaveTop - 1, (int)(progress * 127), kWaveTop - 1, true);
    }

    int length = loop_length;
    int mid = (kWaveTop + kWaveBottom) / 2;
    if (state == LOOP_RECORDING) {
        // Progress toward the 60 s maximum
        int x = (int)((float)record_position / kMaxLoopSamples * 127);
        d.DrawRect(0, mid - 2, 127, mid + 2, true, false);
        d.DrawRect(0, mid - 2, x, mid + 2, true, true);
    } else if (length > 0) {
        // Dithered waveform, then the master position
        int half = (kWaveBottom - kWaveTop) / 2;
        for (int x = 0; x < 128; x++) {
            int h = (int)(std::min(1.f, waveform[x] * 4.f) * half);
            for (int y = mid - h; y <= mid + h; y++) {
                if (((x + y) & 1) == 0) d.DrawPixel(x, y, true);
            }
        }
        int mx = (int)((float)master_position / length * 127);
        d.DrawLine(mx, kWaveTop, mx, kWaveBottom, true);

        // One lane per head: its window and position
        for (int h = 0; h < kNumHeads; h++) {
            int y = kLaneTop + h * 4;
            int x0 = (int)(head_window_start[h] / length * 127);
            int w = std::max(1, (int)(head_window_length[h] / length * 127));
            for (int i = 0; i < w; i += (page_value[PAGE_MIX][h] > 0.001f ? 1 : 3)) {
                d.DrawPixel((x0 + i) % 128, y + 1, true);
            }
            float pos = (head_window_start[h] + heads[h].position) / length;
            pos -= floorf(pos);
            int px = (int)(pos * 127);
            d.DrawLine(px, y, px, y + 2, true);
        }
    }

    if (ui_mode == UI_PAGES && page < kNumKnobPages) {
        // Four columns: label, value, and a dot if the knob hasn't picked up yet
        for (int c = 0; c < kNumHeads; c++) {
            int x = c * 32 + 1;
            const char *label = page == PAGE_FX ? fx_labels[c]
                : page == PAGE_INPUT ? input_labels[c] : head_labels[c];
            d.SetCursor(x, kLabelY);
            d.WriteString(label, Font_6x8, true);
            PageValueText(page, c, text, sizeof(text));
            d.SetCursor(x, kValueY);
            d.WriteString(text, Font_6x8, true);
            if (!knob_caught_display[c]) {
                d.DrawRect(x + 26, kValueY, x + 28, kValueY + 2, true, true);
            }
        }
    } else {
        // SETUP: current item and its value, inverted while editing
        if (ui_mode == UI_PAGES) {
            d.SetCursor(1, kLabelY);
            d.WriteString("Click for setup", Font_6x8, true);
            snprintf(text, sizeof(text), "Reverb: %s", reverb_names[reverb_mode]);
            d.SetCursor(1, kValueY);
            d.WriteString(text, Font_6x8, true);
        } else {
            snprintf(text, sizeof(text), "> %s", setup_names[setup_item]);
            d.SetCursor(1, kLabelY);
            d.WriteString(text, Font_6x8, true);
            char value[16];
            SetupValueText(setup_item, value, sizeof(value));
            bool editing = ui_mode == UI_SETUP_EDIT;
            if (editing) {
                d.DrawRect(0, kValueY - 1, 127, 63, true, true);
            }
            d.SetCursor(13, kValueY);
            d.WriteString(value, Font_6x8, !editing);
        }
    }

    d.Update();
}

// Startup: hardware, clear SDRAM, reverb, settings, knobs, then audio. The main
// loop runs the UI, saves settings, and redraws the waveform and screen.
int main(void) {
    patch.Init();
    sample_rate = patch.AudioSampleRate();

    // SDRAM isn't zeroed at boot, and DaisySP's Init() functions assume it is
    // (PitchShifter leaves several members unset, which would start as NaN)
    memset(loop_buffer, 0, sizeof(loop_buffer));
    memset(reverse_buffer, 0, sizeof(reverse_buffer));
    memset((void *)pre_delay, 0, sizeof(pre_delay));
    pre_delay[0].Init();
    pre_delay[1].Init();
    ResetReverb();

    LoadSettings();

    // Knobs start caught on the MIX page: its values take the physical knob
    // positions, so turning a knob does something straight away
    patch.StartAdc();
    System::Delay(20);
    patch.ProcessAnalogControls();
    for (int k = 0; k < kNumHeads; k++) {
        last_knob[k] = patch.GetKnobValue((DaisyPatch::Ctrl)k);
        page_value[PAGE_MIX][k] = last_knob[k];
    }

    patch.StartAudio(AudioCallback);

    while (1) {
        UpdateUi();
        SaveSettingsIfNeeded();

        uint32_t now = System::GetNow();
        // The loop can change whenever it plays (latched DUB, momentary punch-ins,
        // Decay: Always), so redraw it regularly
        bool playing_now = loop_state == LOOP_PLAYING || loop_state == LOOP_OVERDUB;
        bool refresh = waveform_dirty
            || (playing_now && now - last_waveform_millis >= kWaveformRefreshMillis);
        if (refresh) {
            waveform_dirty = false;
            last_waveform_millis = now;
            RefreshWaveform();
        }
        if (now - last_screen_update_millis >= kScreenUpdateMillis) {
            last_screen_update_millis = now;
            DrawScreen();
        }
    }
}
