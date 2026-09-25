// Mac simulation harness (see run.sh). Records a test signal into Plooper, then plays it back and prints output
// levels over time, separating the reverb (OUT 3/4) from the main mix.
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include "daisy_patch.h"
namespace daisy { uint32_t System::now_ms = 0; FontDef Font_6x8, Font_4x6, Font_7x10; }
int plooper_main();
#define main plooper_main
#include "Plooper.cpp"
#undef main

static float in_buf[4][48], out_buf[4][48];

// Runs `seconds` of audio; prints RMS of OUT1 and OUT3 every 0.5 s
void Run(float seconds, float amp, float dc, const char* label) {
    const float* ins[4] = { in_buf[0], in_buf[1], in_buf[2], in_buf[3] };
    float* outs[4] = { out_buf[0], out_buf[1], out_buf[2], out_buf[3] };
    int blocks = (int)(seconds * 1000);
    double sum_main = 0, sum_verb = 0, sum_in = 0; int count = 0; static double t = 0;
    for (int b = 0; b < blocks; b++) {
        for (int n = 0; n < 48; n++) {
            float x = amp * sinf(2 * 3.14159f * 220.f * t) * (fmod(t, 0.5) < 0.25 ? 1.f : 0.3f) + dc;
            t += 1.0 / 48000;
            in_buf[0][n] = x; in_buf[1][n] = 0.f;
        }
        AudioCallback(ins, outs, 48);
        daisy::System::now_ms++;
        for (int n = 0; n < 48; n++) {
            sum_main += out_buf[0][n] * out_buf[0][n];
            sum_verb += out_buf[2][n] * out_buf[2][n];
            sum_in += in_buf[0][n] * in_buf[0][n];
            count++;
        }
        if ((b + 1) % 500 == 0) {
            printf("%-6s t=%5.1fs  in=%.3f  OUT1=%.3f  reverb(OUT3)=%.3f  state=%s\n", label,
                   daisy::System::now_ms / 1000.0, sqrt(sum_in / count), sqrt(sum_main / count),
                   sqrt(sum_verb / count), state_names[loop_state]);
            sum_main = sum_verb = sum_in = 0; count = 0;
        }
    }
}

int main(int argc, char** argv) {
    float dc = argc > 1 ? atof(argv[1]) : 0.f;
    reverb_mode = argc > 2 ? atoi(argv[2]) : REVERB_PLATE;
    printf("reverb mode: %s, input DC %.3f\n", reverb_names[reverb_mode], dc);
    // Boot as main() does
    sample_rate = 48000.f;
    memset(loop_buffer, 0, sizeof(loop_buffer));
    ResetReverb();
    pre_delay[0].Init(); pre_delay[1].Init();
    // Knobs: MIX A at half, others down (the isolation-test setup)
    bool stress = argc > 3;
    float mix[4] = { 0.5f, 0.f, 0.f, 0.f };
    if (stress) {
        for (int k = 0; k < 4; k++) mix[k] = getenv("ONEHEAD") && k ? 0.f : 1.f;
        page_value[PAGE_FX][1] = getenv("DECAY") ? atof(getenv("DECAY")) : 1.f; // DECAY
        page_value[PAGE_FX][3] = 1.f; // ODD max
        page_value[PAGE_FX][0] = 0.5f; // reverb MIX at the same level as before (x2 range)
    }
    for (int k = 0; k < 4; k++) { patch.knobs[k] = mix[k]; page_value[PAGE_MIX][k] = mix[k]; }
    Run(2, 0.5f, dc, "empty");
    record_toggle_requested = true; Run(4, 0.5f, dc, "rec");
    record_toggle_requested = true; Run(stress ? 30 : 10, 0.5f, dc, "play");
}
