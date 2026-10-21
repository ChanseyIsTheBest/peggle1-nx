// Temporary hooks that log the callers of functions a boot crashed in (one per finding, each with its evidence).
//
// peggle1-nx (2026-10-21): empty at the start of the port; Peggle 2's hooks were at Peggle 2's addresses. The
// WaitForSingleObject counter (peggle1_diag_waits) moved to peggle1_worker_pool.cpp with the fix it found.

#include <rex/hook.h>
