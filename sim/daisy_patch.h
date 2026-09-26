// Host stand-in for the Daisy Patch hardware, just enough to run Plooper's audio code.
#pragma once
#include <cstdint>
#include <cstddef>
#include "daisysp.h"
#define DSY_SDRAM_BSS
inline void __disable_irq() {}
inline void __enable_irq() {}
namespace daisy {
struct FontDef { int w, h; };
extern FontDef Font_6x8, Font_4x6, Font_7x10;
struct System {
    static uint32_t now_ms;
    static uint32_t GetNow() { return now_ms; }
    static void Delay(uint32_t) {}
};
struct AudioHandle {
    typedef const float* const* InputBuffer;
    typedef float** OutputBuffer;
    typedef void (*AudioCallback)(InputBuffer, OutputBuffer, size_t);
};
struct DacHandle {
    enum class Channel { ONE, TWO, BOTH };
    int WriteValue(Channel, uint16_t) { return 0; }
};
struct QSPIHandle {};
struct CpuLoadMeter {
    void Init(float, size_t) {} void OnBlockStart() {} void OnBlockEnd() {} void Reset() {}
    float GetAvgCpuLoad() { return 0.f; } float GetMaxCpuLoad() { return 0.f; }
};
struct GPIO { bool state = false; void Write(bool s) { state = s; } };
struct GateIn { bool high = false; bool State() { return high; } bool Trig() { return false; } };
struct Encoder {
    // Set `down` from the harness; Debounce() turns changes into edges
    bool down = false, state = false, rising = false, falling = false;
    uint32_t press_time = 0;
    void Debounce();
    int Increment() { return 0; }
    bool RisingEdge() { return rising; }
    bool FallingEdge() { return falling; }
    bool Pressed() { return state; }
    float TimeHeldMs();
};
struct Display {
    void Fill(bool) {} void SetCursor(int, int) {} void WriteString(const char*, FontDef, bool) {}
    void DrawRect(int, int, int, int, bool, bool = false) {} void DrawPixel(int, int, bool) {}
    void DrawLine(int, int, int, int, bool) {} void Update() {}
};
struct Seed { QSPIHandle qspi; DacHandle dac; void SetLed(bool) {} };
class DaisyPatch {
  public:
    enum Ctrl { CTRL_1, CTRL_2, CTRL_3, CTRL_4, CTRL_LAST };
    enum GateInput { GATE_IN_1, GATE_IN_2, GATE_IN_LAST };
    float knobs[4] = {0, 0, 0, 0};
    Seed seed; Encoder encoder; GateIn gate_input[2]; GPIO gate_output; Display display;
    void Init() {} float AudioSampleRate() { return 48000.f; } size_t AudioBlockSize() { return 48; }
    void StartAdc() {} void StartAudio(AudioHandle::AudioCallback) {}
    void ProcessAnalogControls() {}
    float GetKnobValue(Ctrl k) { return knobs[k]; }
};
inline void Encoder::Debounce() {
    rising = down && !state;
    falling = !down && state;
    if (rising) press_time = System::GetNow();
    state = down;
}
inline float Encoder::TimeHeldMs() { return state ? System::GetNow() - press_time : 0; }
}  // namespace daisy
