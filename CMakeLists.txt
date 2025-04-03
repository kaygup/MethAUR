cmake_minimum_required(VERSION 3.10)
project(methaur VERSION 1.0.0 LANGUAGES C)

# Find required packages
find_package(CURL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(JSONC REQUIRED json-c)

# Set compiler flags
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra")

# Add executable
add_executable(methaur src/methaur.c)

# Include directories
include_directories(
    ${CURL_INCLUDE_DIRS}
    ${JSONC_INCLUDE_DIRS}
)

# Link libraries
target_link_libraries(methaur
    ${CURL_LIBRARIES}
    ${JSONC_LIBRARIES}
)

# Install target
install(TARGETS methaur DESTINATION bin)

# Add uninstall target
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cmake_uninstall.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
    IMMEDIATE @ONLY
)

add_custom_target(uninstall
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake
)