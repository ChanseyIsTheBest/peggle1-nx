# Writes the code generation profile marker (app/generated/codegen_profile.txt): cmake -DOUT=<file> -DPROFILE=<name>
# -P write_codegen_profile.cmake. Run by the build after it re-runs codegen itself (see app/CMakeLists.txt).
file(WRITE "${OUT}" "${PROFILE}")
