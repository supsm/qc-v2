cmake_minimum_required(VERSION 3.5)

project(jsoncons CXX)

set(JSONCONS_PROJECT_DIR ${PROJECT_SOURCE_DIR})
set(JSONCONS_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Versioning
# ==========

file(STRINGS "${JSONCONS_INCLUDE_DIR}/jsoncons/config/version.hpp" jsoncons_version_defines
     REGEX "#define JSONCONS_VERSION_(MAJOR|MINOR|PATCH)")
foreach(ver ${jsoncons_version_defines})
    if(ver MATCHES "#define JSONCONS_VERSION_(MAJOR|MINOR|PATCH) +([^ ]+)$")
        set(JSONCONS_VERSION_${CMAKE_MATCH_1} "${CMAKE_MATCH_2}" CACHE INTERNAL "")
    endif()
endforeach()
set(${PROJECT_NAME}_VERSION
    ${JSONCONS_VERSION_MAJOR}.${JSONCONS_VERSION_MINOR}.${JSONCONS_VERSION_PATCH})
message(STATUS "jsoncons v${${PROJECT_NAME}_VERSION}")

# Build
# =====

add_library(jsoncons INTERFACE)
target_include_directories(jsoncons INTERFACE ${JSONCONS_INCLUDE_DIR})
