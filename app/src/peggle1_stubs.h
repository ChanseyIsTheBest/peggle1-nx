// The linker's jump stubs of Peggle's executable, in the function table (see peggle1_stubs.cpp).

#pragma once

namespace rex {
class Runtime;
}

namespace peggle1::stubs {

// Gives every jump stub without a generated function its target's function in the function table. After the image is
// loaded and the generated functions are registered (ReXApp::OnPostLoadXexImage).
void Install(rex::Runtime* runtime);

}  // namespace peggle1::stubs
