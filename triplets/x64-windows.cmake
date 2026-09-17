set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)

# Workaround for CMake 4.x removing compatibility with cmake_minimum_required < 3.5
# Affects ports like usrsctp 0.9.5.0 that still declare old CMake minimums.
set(VCPKG_CMAKE_CONFIGURE_OPTIONS "-DCMAKE_POLICY_VERSION_MINIMUM=3.5")
