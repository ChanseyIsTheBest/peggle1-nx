// banjo-nx (2026-10-14; sasrt-nx's tools/single_floats.py, idea 2 of sa2-nx's list): with inputs that are singles,
// double(float(a) op float(b)) == double(float(double(a) op double(b))) for + - * / and sqrt, bit for bit, in all four
// rounding modes (double rounding is innocuous: double has more than 2 * 24 + 2 bits). Inputs: special values, every
// exponent class (denormals, near overflow), random patterns; operands through a volatile. Exit 1 on a difference.
#include <cfenv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>

namespace {

uint64_t g_checked = 0, g_failures = 0;

uint64_t Bits(double d) {
  uint64_t u;
  std::memcpy(&u, &d, 8);
  return u;
}
float F(uint32_t b) {
  float f;
  std::memcpy(&f, &b, 4);
  return f;
}

[[gnu::noinline]] void Check(uint32_t ab, uint32_t bb) {
  volatile float va = F(ab), vb = F(bb);
  const float a = va, b = vb;
  const double da = a, db = b;
  const double d[5] = {double(float(da + db)), double(float(da - db)), double(float(da * db)), double(float(da / db)),
                       double(float(std::sqrt(da)))};
  const double s[5] = {double(a + b), double(a - b), double(a * b), double(a / b), double(std::sqrt(a))};
  static const char* names[5] = {"fadds", "fsubs", "fmuls", "fdivs", "fsqrts"};
  for (int i = 0; i < 5; ++i) {
    ++g_checked;
    const bool both_nan = std::isnan(d[i]) && std::isnan(s[i]);
    if (Bits(d[i]) != Bits(s[i]) && !(both_nan && (Bits(d[i]) | (1ull << 51)) == (Bits(s[i]) | (1ull << 51)))) {
      if (g_failures++ < 10)
        std::printf("FAILED %s %08X %08X: float form %016llX, double form %016llX\n", names[i], ab, bb,
                    (unsigned long long)Bits(s[i]), (unsigned long long)Bits(d[i]));
    }
  }
}

}  // namespace

int main() {
  const uint32_t specials[] = {0x00000000, 0x80000000, 0x00000001, 0x80000001, 0x007FFFFF, 0x00800000, 0x3F800000,
                               0xBF800000, 0x7F7FFFFF, 0xFF7FFFFF, 0x7F800000, 0xFF800000, 0x7FC00000, 0xFFC00000,
                               0x3F7FFFFF, 0x3F800001, 0x34000000, 0x4B000000, 0x1F800000, 0x5F800000};
  const int modes[] = {FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD};
  std::mt19937 rng(20261014);
  for (const int mode : modes) {
    std::fesetround(mode);
    for (const uint32_t a : specials)
      for (const uint32_t b : specials) Check(a, b);
    for (int k = 0; k < 3000000; ++k) {
      uint32_t a = rng(), b = rng();
      if (k & 1) b = (b & 0x807FFFFFu) | (((a >> 23) & 0xFFu) + (rng() % 48) - 24u) % 255u << 23;  // close exponents
      Check(a, b);
    }
  }
  std::fesetround(FE_TONEAREST);
  std::printf("single_floats_test: %llu comparisons, %llu differences\n", (unsigned long long)g_checked,
              (unsigned long long)g_failures);
  return g_failures ? 1 : 0;
}
