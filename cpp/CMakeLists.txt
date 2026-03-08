cmake_minimum_required(VERSION 3.16)
project(polymarket_arb_detector LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Try system Boost first, fall back to anaconda for local dev
find_package(Boost 1.70 QUIET COMPONENTS system)
if(Boost_FOUND)
    message(STATUS "Using system Boost: ${Boost_INCLUDE_DIRS}")
    include_directories(${Boost_INCLUDE_DIRS})
else()
    # Local dev: anaconda Boost headers (beast is header-only)
    set(ANACONDA_ROOT "$ENV{HOME}/anaconda3")
    if(EXISTS "${ANACONDA_ROOT}/include/boost/beast.hpp")
        message(STATUS "Using Anaconda Boost: ${ANACONDA_ROOT}/include")
        include_directories(${ANACONDA_ROOT}/include)
    else()
        message(FATAL_ERROR "Boost not found. Install libboost-dev or set ANACONDA_ROOT.")
    endif()
endif()

# Boost.System header-only mode
add_definitions(-DBOOST_ERROR_CODE_HEADER_ONLY)

include_directories(
    ${CMAKE_SOURCE_DIR}/deps
    ${CMAKE_SOURCE_DIR}/src
)

# Find system OpenSSL
find_package(OpenSSL REQUIRED)

# simdjson library — always optimized
add_library(simdjson STATIC deps/simdjson.cpp)
target_include_directories(simdjson PUBLIC deps)
target_compile_options(simdjson PRIVATE -O3 -march=native -DNDEBUG)

# Main executable
add_executable(arb_detector
    src/main.cpp
    src/websocket.cpp
    src/parser.cpp
    src/orderbook.cpp
    src/arbitrage.cpp
    src/logger.cpp
    src/gcc14_compat.cpp
)

# Always -O3 -march=native for the hot path
target_compile_options(arb_detector PRIVATE -O3 -march=native -DNDEBUG)

target_link_libraries(arb_detector
    simdjson
    OpenSSL::SSL
    OpenSSL::Crypto
    pthread
)

# If system Boost has a library (some distros), link it
if(Boost_FOUND AND Boost_LIBRARIES)
    target_link_libraries(arb_detector ${Boost_LIBRARIES})
endif()

# Don't accidentally pull in anaconda runtime libs
set_target_properties(arb_detector PROPERTIES
    SKIP_BUILD_RPATH TRUE
    BUILD_WITH_INSTALL_RPATH TRUE
)
