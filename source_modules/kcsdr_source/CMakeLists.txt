cmake_minimum_required(VERSION 3.13)
project(kcsdr_source)

file(GLOB SRC "src/*.cpp")

include(${SDRPP_MODULE_CMAKE})

if (MSVC)
    # TODO
else (MSVC)
    target_link_directories(kcsdr_source PRIVATE "/usr/local/lib/")
    target_include_directories(kcsdr_source PRIVATE "/usr/local/include/kcsdr/")
    target_link_libraries(kcsdr_source PRIVATE kcsdr)
endif ()
