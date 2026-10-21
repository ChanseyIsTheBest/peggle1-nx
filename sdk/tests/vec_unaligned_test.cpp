// banjo-nx: rex/ppc/vec_unaligned.h against the generator's byte loops for stvlx / stvrx (tools/vec_unaligned_stores.py).
// Every alignment of the effective address, random vectors and random memory around the block; the whole 48-byte
// window around the block is compared, so a byte written that the loop does not write shows too. Exit 1 on a
// difference.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>

#include <rex/ppc/vec_unaligned.h>

namespace {

void LoopLeft(uint8_t* mem, uint32_t ea, const uint8_t* u8) {
  for (size_t i = 0; i < (16 - (ea & 0xF)); i++) mem[ea + i] = u8[15 - i];
}
void LoopRight(uint8_t* mem, uint32_t ea, const uint8_t* u8) {
  for (size_t i = 0; i < (ea & 0xF); i++) mem[ea - i - 1] = u8[i];
}

}  // namespace

int main() {
  std::mt19937 rng(20261014);
  uint64_t checks = 0, differences = 0;
  alignas(16) uint8_t a[64], b[64];
  for (int round = 0; round < 20000; ++round) {
    uint8_t v[16];
    for (uint8_t& x : v) x = uint8_t(rng());
    for (uint32_t k = 0; k < 16; ++k) {
      for (int side = 0; side < 2; ++side) {
        for (uint8_t& x : a) x = uint8_t(rng());
        std::memcpy(b, a, sizeof(a));
        const uint32_t ea = 16 + k;  // the block is a[16..31]
        if (side == 0) {
          LoopLeft(a, ea, v);
          rex::ppc::StoreVectorLeft(b + ea, ea, v);
        } else {
          LoopRight(a, ea, v);
          rex::ppc::StoreVectorRight(b + (ea & ~0xFu), ea, v);
        }
        ++checks;
        if (std::memcmp(a, b, sizeof(a)) != 0) {
          if (differences++ < 8) std::printf("DIFFERENCE: %s, ea & 15 = %u\n", side ? "stvrx" : "stvlx", k);
        }
      }
    }
  }
  std::printf("vec_unaligned_test: %llu stores compared, %llu differences\n", (unsigned long long)checks,
              (unsigned long long)differences);
  return differences ? 1 : 0;
}
