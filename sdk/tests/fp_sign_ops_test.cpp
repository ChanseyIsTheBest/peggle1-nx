// banjo-nx (2026-10-14; sasrt-nx's tool, idea 7 of sa2-nx's list): fneg / fabs / fnabs as float operations
// (tools/fp_sign_ops.py: REX_FNEG / REX_FABS / REX_FNABS) against the integer forms the generator writes (an XOR,
// AND-NOT or OR of the sign bit), bit for bit: special values, every single-precision bit pattern of a sample widened
// to double, and random 64-bit patterns; with the flush-to-zero / denormals-are-zero mode off and on, in all four
// rounding modes. Operands come through a volatile, so nothing is worked out at build time. Exit 1 on a difference.
#include <cfenv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>

#if defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
#endif

namespace {

union Reg {
  double f64;
  uint64_t u64;
};

#define FLOAT_FNEG(d, s) ((d).f64 = -(s).f64)
#define FLOAT_FABS(d, s) ((d).f64 = __builtin_fabs((s).f64))
#define FLOAT_FNABS(d, s) ((d).f64 = -__builtin_fabs((s).f64))
#define INT_FNEG(d, s) ((d).u64 = (s).u64 ^ 0x8000000000000000ull)
#define INT_FABS(d, s) ((d).u64 = (s).u64 & ~0x8000000000000000ull)
#define INT_FNABS(d, s) ((d).u64 = (s).u64 | 0x8000000000000000ull)

uint64_t g_checked = 0, g_failures = 0;

void Fail(const char* what, uint64_t in, uint64_t got, uint64_t want) {
  if (g_failures++ < 10)
    std::printf("FAILED %s %016llX: %016llX, want %016llX\n", what, (unsigned long long)in, (unsigned long long)got,
                (unsigned long long)want);
}

[[gnu::noinline]] void Check(uint64_t bits) {
  volatile uint64_t in = bits;
  Reg s, a, b;
  s.u64 = in;
  FLOAT_FNEG(a, s);
  INT_FNEG(b, s);
  if (a.u64 != b.u64) Fail("fneg", bits, a.u64, b.u64);
  FLOAT_FABS(a, s);
  INT_FABS(b, s);
  if (a.u64 != b.u64) Fail("fabs", bits, a.u64, b.u64);
  FLOAT_FNABS(a, s);
  INT_FNABS(b, s);
  if (a.u64 != b.u64) Fail("fnabs", bits, a.u64, b.u64);
  g_checked += 3;
}

void SetFlush(bool on) {
#if defined(__x86_64__) || defined(_M_X64)
  _mm_setcsr(on ? (_mm_getcsr() | 0x8040u) : (_mm_getcsr() & ~0x8040u));  // FTZ + DAZ, the PC's flush mode
#elif defined(__aarch64__)
  uint64_t fpcr;
  __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
  fpcr = on ? (fpcr | (1ull << 24)) : (fpcr & ~(1ull << 24));
  __asm__ volatile("msr fpcr, %0" : : "r"(fpcr));
#endif
}

}  // namespace

int main() {
  const uint64_t specials[] = {
      0x0000000000000000ull, 0x8000000000000000ull, 0x0000000000000001ull, 0x8000000000000001ull,  // zeros, denormals
      0x000FFFFFFFFFFFFFull, 0x800FFFFFFFFFFFFFull, 0x0010000000000000ull, 0x3FF0000000000000ull,  // max denormal, 1
      0xBFF0000000000000ull, 0x7FEFFFFFFFFFFFFFull, 0x7FF0000000000000ull, 0xFFF0000000000000ull,  // max, infinities
      0x7FF8000000000000ull, 0xFFF8000000000000ull, 0x7FF0000000000001ull, 0xFFF7FFFFFFFFFFFFull,  // quiet, signalling
      0x36A0000000000000ull, 0x380FFFFFE0000000ull, 0x47EFFFFFE0000000ull};  // single-range edges
  const int modes[] = {FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD};
  std::mt19937_64 rng(20261014);
  for (int flush = 0; flush < 2; ++flush) {
    SetFlush(flush != 0);
    for (const int mode : modes) {
      std::fesetround(mode);
      for (const uint64_t v : specials) Check(v);
      for (uint32_t k = 0; k < 2000000; ++k) {  // single-precision values widened to double, then raw patterns
        float f;
        const uint32_t b = uint32_t(rng());
        std::memcpy(&f, &b, 4);
        Reg r;
        r.f64 = double(f);
        Check(r.u64);
        Check(rng());
      }
    }
  }
  std::fesetround(FE_TONEAREST);
  SetFlush(false);
  std::printf("fp_sign_ops_test: %llu comparisons, %llu differences\n", (unsigned long long)g_checked,
              (unsigned long long)g_failures);
  return g_failures ? 1 : 0;
}
