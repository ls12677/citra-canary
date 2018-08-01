#!/bin/bash -ex

cd /citra

mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=/usr/lib/ccache/gcc -DCMAKE_CXX_COMPILER=/usr/lib/ccache/g++ -DCITRA_ENABLE_COMPATIBILITY_REPORTING=${ENABLE_COMPATIBILITY_REPORTING:-"OFF"} -DENABLE_COMPATIBILITY_LIST_DOWNLOAD=ON
make -j4

ctest -VV -C Release
