cmake_minimum_required(VERSION 3.10)

project(TS2Extender LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

include(FetchContent)

FetchContent_Declare(
  MinHook
  GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
  GIT_TAG        master
)

FetchContent_MakeAvailable(MinHook)

file(GLOB_RECURSE SOURCES_SHARED CONFIGURE_DEPENDS "src/*.cpp")
# miniz (MIT, single amalgamated .c) extracts the .zip files of ExtractNeighborhood and builds those of ZipNeighborhood.
set(MINIZ_SOURCE thirdparty/miniz/miniz.c)
set(MINIZ_DEFINES MINIZ_NO_STDIO MINIZ_NO_TIME MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
file(GLOB_RECURSE HEADERS_SHARED CONFIGURE_DEPENDS "include/*.h")

file(GLOB_RECURSE SOURCES_LC CONFIGURE_DEPENDS "src_lc/*.cpp")
file(GLOB_RECURSE SOURCES_UC CONFIGURE_DEPENDS "src_uc/*.cpp")

file(GLOB_RECURSE HEADERS_LC CONFIGURE_DEPENDS "include_lc/*.h")
file(GLOB_RECURSE HEADERS_UC CONFIGURE_DEPENDS "include_uc/*.h")

function(setup_ts2extender_target target_name compile_defs sources headers include_dir)
    add_library(${target_name} SHARED ${SOURCES_SHARED} ${MINIZ_SOURCE} ${sources} ${HEADERS_SHARED} ${headers})

    target_include_directories(${target_name} PRIVATE 
	include
	"${include_dir}"
	minhook/include
	thirdparty/miniz
	thirdparty/mINI/src/mini
    )

    target_link_libraries(${target_name} PRIVATE 
    minhook
    gdiplus
    winhttp
    bcrypt
    )

    set_target_properties(${target_name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
        SUFFIX ".asi"
    )

    target_compile_definitions(${target_name} PRIVATE ${compile_defs} ${MINIZ_DEFINES})
endfunction()

# Legacy Collection build (Standard)
setup_ts2extender_target(TS2Extender "TS2_LC" "${SOURCES_LC}" "${HEADERS_LC}" include_lc)

# Ultimate Collection build
setup_ts2extender_target(TS2ExtenderUC "TS2_UC" "${SOURCES_UC}" "${HEADERS_UC}" include_uc)

# Console tests for the network code (no game needed). Not built by default:
#   cmake --build out/build/x86 --target NetworkTests
# then: python tests/run_network_tests.py out/build/x86/bin/NetworkTests.exe
add_executable(NetworkTests EXCLUDE_FROM_ALL tests/NetworkTests.cpp src/SafePath.cpp src/HttpClient.cpp src/ZipExtract.cpp src/ZipCreate.cpp ${MINIZ_SOURCE})
target_include_directories(NetworkTests PRIVATE include thirdparty/miniz)
target_link_libraries(NetworkTests PRIVATE winhttp bcrypt)
target_compile_definitions(NetworkTests PRIVATE ${MINIZ_DEFINES})
set_target_properties(NetworkTests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
