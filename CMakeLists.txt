cmake_minimum_required(VERSION 3.20)
project(WinkLinuxC C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(init init.c)
add_executable(sh sh.c)
add_executable(winkfetch winkfetch.c)