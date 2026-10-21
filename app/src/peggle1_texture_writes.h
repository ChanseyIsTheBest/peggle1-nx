// Peggle 2: the game's texture loader tells the renderer which texture it has just written (peggle1_gpu_lock_rechecks).
//
// The native renderer keeps a copy of every texture and looks at guest memory again only every 2, 4 ... 32 frames once
// a texture has looked unchanged (PrepareTexture, src/me/masseffect/masseffect_native_draws.cpp). Peggle 2 loads a
// level's atlas into the memory the previous level's atlas had; the first frames of the new level were drawn with the
// old image until that interval ran out (console, 2026-10-13: one frame of the previous level's pieces in the new
// level's background).
//
// A console GPU reads the memory at every draw, so the game has no reason to say anything, and nothing in the command
// list named a texture's memory on the PC route (Direct3D's LockResource, sub_82617A50, can send the locked range to
// the GPU, but no range ever matched a texture; by its code it can also just set a "texture cache invalid" flag for
// the whole device). So the note is taken where the game writes: the loader's routine that copies decoded rows into
// the texture (sub_8245E890: LockRect, the streaming copy, UnlockRect), reached through a vtable. After each call
// the texture's address goes to the ring thread, which reads the textures at that address again at their next use.
#pragma once

namespace peggle1::texture_writes {

// Once, after the guest image is loaded and its functions are registered (a no-op before that and after the first
// success).
void Install();

}  // namespace peggle1::texture_writes
