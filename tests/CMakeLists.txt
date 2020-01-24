# Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
# documentation files (the "Software"), to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software,
# and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all copies or substantial portions
# of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
# WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS
# OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
# OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
#
# Copyright (c) 2020 UAVCAN Development Team
# Authors: Pavel Kirienko <pavel.kirienko@zubax.com>

cmake_minimum_required(VERSION 3.15)
project(o1heap_tests C CXX)

# C options
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Werror -pedantic")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wdouble-promotion -Wswitch-enum -Wfloat-equal -Wundef")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wconversion -Wtype-limits")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wsign-conversion -Wcast-align -Wmissing-declarations")

# C++ options
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Werror -pedantic")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wdouble-promotion -Wswitch-enum -Wfloat-equal -Wundef")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wconversion -Wsign-promo")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wsign-conversion -Wcast-align -Wmissing-declarations")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wtype-limits -Wzero-as-null-pointer-constant -Wnon-virtual-dtor")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Woverloaded-virtual -Wsign-promo -Wold-style-cast")

include_directories(catch ../o1heap)

set(common_sources catch/main.cpp ../o1heap/o1heap.c)

# Upon generation, all tests can be run via run_all_tests.sh.
function(add_test name files compile_definitions compile_features flags)
    add_executable(${name} ${common_sources} ${files})
    target_compile_definitions(${name} PUBLIC ${compile_definitions})
    target_compile_features(${name} PUBLIC ${compile_features})
    set_target_properties(${name} PROPERTIES COMPILE_FLAGS "${flags}" LINK_FLAGS "${flags}")

    file(APPEND ${CMAKE_BINARY_DIR}/run_all_tests.sh "./${name} --rng-seed time || exit 1\n")
endfunction()

function(add_test_matrix name files compile_definitions)
    add_test("${name}_c11_x64" "${files}" "${compile_definitions}" c_std_11 "-m64")
    add_test("${name}_c11_x32" "${files}" "${compile_definitions}" c_std_11 "-m32")
    add_test("${name}_c99_x64" "${files}" "${compile_definitions}" c_std_99 "-m64")
    add_test("${name}_c99_x32" "${files}" "${compile_definitions}" c_std_99 "-m32")
endfunction()

add_test_matrix(test_private test_private.cpp O1HEAP_EXPOSE_INTERNALS=1)

add_test_matrix(test_general test_general.cpp "")
