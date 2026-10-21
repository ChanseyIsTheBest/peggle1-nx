// peggle1-nx (tools/renut_hook_remap.py): the addresses both Renut's renderer and Peggle 1's own code hook.
// Peggle 1's hook calls renut_native_sub_X first when Renut's renderer is chosen (BANJO_RENUT_YIELD_SHARED).
#pragma once

#include <cstdint>

#include <rex/ppc.h>

extern "C" void renut_native_sub_824430C8(PPCContext& ctx, uint8_t* base);
