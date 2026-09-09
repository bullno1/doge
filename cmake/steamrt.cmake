set(CMAKE_C_COMPILER "clang")
set(CMAKE_CXX_COMPILER "clang++")
add_link_options("-Wl,--build-id")

# SteamSDK has an older cmake version

macro(cmake_minimum_required)
endmacro()
