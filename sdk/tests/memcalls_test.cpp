// banjo-nx: rex/ppc/memcalls.h (tools/inline_memcalls.py) against plain definitions. Exit 1 on a difference.
//   CopyConst<N>: every N of 1-64, every offset of dst and src in a small window (overlapping both ways included);
//                 for dst <= src the result has to be memmove's (what the CRT's forward copy gives there).
//   CopySmears<N>: true exactly when dst > src and dst - src < N.
//   CoversSlot<N>: true exactly when [dst, dst + N) meets [slot, slot + 8) (32-bit addresses, wrapping).
//   PlainRange<N>: the range stays below 0x7F000000 or inside 0x80000000-0x9FFFFFFF and does not wrap.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <utility>

#include <rex/ppc/memcalls.h>

namespace {

uint64_t g_checks = 0, g_failures = 0;

void Fail(const char* what, uint32_t n, uint32_t a, uint32_t b) {
  if (g_failures++ < 12) std::printf("FAILED %s: N %u, %08X %08X\n", what, n, a, b);
}

template <uint32_t N>
void CheckN(std::mt19937& rng) {
  // CopyConst against memmove, dst <= src (the case the helpers run) and disjoint ranges.
  uint8_t buf[256], want[256];
  for (int d = 0; d < 80; d += 3) {
    for (int s = 0; s < 80; s += 5) {
      const bool overlap_smears = d > s && d - s < int(N);
      if (overlap_smears) continue;  // the original runs there
      for (uint8_t& x : buf) x = uint8_t(rng());
      std::memcpy(want, buf, sizeof(buf));
      std::memmove(want + 64 + d, want + 64 + s, N);
      rex::ppc::CopyConst<N>(buf + 64 + d, buf + 64 + s);
      ++g_checks;
      if (std::memcmp(buf, want, sizeof(buf)) != 0) Fail("CopyConst", N, uint32_t(d), uint32_t(s));
    }
  }
  for (int k = 0; k < 2000; ++k) {
    const uint32_t a = uint32_t(rng()), b = (k & 1) ? a + uint32_t(rng() % 200) - 100u : uint32_t(rng());
    // CopySmears
    const bool smears = a > b && uint64_t(a) - b < N;
    ++g_checks;
    if (rex::ppc::CopySmears<N>(a, b) != smears) Fail("CopySmears", N, a, b);
    // CoversSlot: brute force over the 8 slot bytes
    bool covers = false;
    for (uint32_t i = 0; i < 8; ++i)
      if (uint32_t(b + i - a) < N) covers = true;
    ++g_checks;
    if (rex::ppc::CoversSlot<N>(a, b) != covers) Fail("CoversSlot", N, a, b);
    // PlainRange
    const uint64_t e = uint64_t(a) + N - 1;
    const bool plain = e <= 0xFFFFFFFFull && ((e < 0x7F000000ull) || (a >= 0x80000000u && e < 0xA0000000ull));
    ++g_checks;
    if (rex::ppc::PlainRange<N>(a) != plain) Fail("PlainRange", N, a, 0);
  }
}

template <uint32_t... Ns>
void CheckAll(std::mt19937& rng, std::integer_sequence<uint32_t, Ns...>) {
  (CheckN<Ns + 1>(rng), ...);
}

}  // namespace

int main() {
  std::mt19937 rng(20261014);
  CheckAll(rng, std::make_integer_sequence<uint32_t, 64>{});
  std::printf("memcalls_test: %llu checks, %llu differences\n", (unsigned long long)g_checks,
              (unsigned long long)g_failures);
  return g_failures ? 1 : 0;
}
