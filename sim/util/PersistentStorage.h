#pragma once
namespace daisy {
template <typename T> class PersistentStorage {
  public:
    PersistentStorage(QSPIHandle&) {}
    void Init(const T& defaults, uint32_t = 0) { s_ = defaults; }
    T& GetSettings() { return s_; }
    void Save() {}
  private:
    T s_;
};
}
