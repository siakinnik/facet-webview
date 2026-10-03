cmake_minimum_required(VERSION 3.18)
project(facet-webview VERSION 0.1.0 LANGUAGES CXX)
set(FIREFOX_VERSION_SUFFIX "alpha")  # pre-release tag; empty for final releases; keep manifest.json in sync

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(FIREFOX_STATIC "Link fully static (portable release binaries)" OFF)
option(FIREFOX_RUNTIME "Build the bundled runtime (libraries, fonts)" ON)
option(FIREFOX_EXAMPLES "Build the example app module" ON)

if(FIREFOX_VERSION_SUFFIX)
    set(FIREFOX_MODULE_VERSION "${PROJECT_VERSION}-${FIREFOX_VERSION_SUFFIX}")
else()
    set(FIREFOX_MODULE_VERSION "${PROJECT_VERSION}")
endif()

# The plugin SDK lives in facet-core: a local checkout next to this project
# (or FACET_CORE_DIR) is used when present, otherwise it is downloaded.
# Capability endpoints need facet-core 0.6.
set(FACET_CORE_DIR "" CACHE PATH "facet-core checkout providing the SDK (default: ../facet-core, else download)")
set(FACET_CORE_REF "v0.7.0-alpha" CACHE STRING "facet-core git ref to download the SDK from")
if(NOT FACET_CORE_DIR AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../facet-core/sdk/CMakeLists.txt")
    set(FACET_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../facet-core")
endif()
if(FACET_CORE_DIR)
    add_subdirectory(${FACET_CORE_DIR}/sdk ${CMAKE_BINARY_DIR}/facet_sdk)
else()
    include(FetchContent)
    FetchContent_Declare(facet_core
        GIT_REPOSITORY https://github.com/siakinnik/facet-core.git
        GIT_TAG ${FACET_CORE_REF}
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR sdk)
    FetchContent_MakeAvailable(facet_core)
endif()

# OpenSSL (downloads) and liblzma (.tar.xz) are linked statically.
set(OPENSSL_USE_STATIC_LIBS ON)
find_package(OpenSSL REQUIRED)
find_library(LZMA_STATIC NAMES liblzma.a lzma REQUIRED)
find_package(Threads REQUIRED)

# The web.runtime@1 client; app modules copy client/webview.*.
add_library(facet_webview STATIC client/webview.cpp)
target_include_directories(facet_webview PUBLIC client)
target_link_libraries(facet_webview PUBLIC facet_sdk)
target_compile_options(facet_webview PRIVATE -Wall -Wextra)

add_library(firefox_core STATIC src/https.cpp src/package.cpp src/installer.cpp src/browser.cpp)
target_include_directories(firefox_core PUBLIC src)
target_compile_options(firefox_core PRIVATE -Wall -Wextra)
target_link_libraries(firefox_core PUBLIC facet_webview OpenSSL::SSL OpenSSL::Crypto ${LZMA_STATIC} Threads::Threads
                                          ${CMAKE_DL_LIBS})

set(PLUGIN_OUT ${CMAKE_BINARY_DIR}/webview.plugin)
add_executable(webview src/main.cpp src/i18n/ru.cpp)
target_link_libraries(webview PRIVATE firefox_core)
target_compile_options(webview PRIVATE -Wall -Wextra)
target_compile_definitions(webview PRIVATE FIREFOX_MODULE_VERSION="${FIREFOX_MODULE_VERSION}")
set_target_properties(webview PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${PLUGIN_OUT})
if(FIREFOX_STATIC)
    target_link_options(webview PRIVATE -static -s)
endif()

# The build dir doubles as a plugin dir: FACET_PLUGIN_PATH=<build>/webview.plugin
facet_plugin_manifest(${CMAKE_CURRENT_SOURCE_DIR}/manifest.json ${PLUGIN_OUT}/manifest.json)
if(FIREFOX_RUNTIME)
    set(FIREFOX_RUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/runtime" CACHE PATH "Where the bundled runtime is built")
    execute_process(COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/scripts/build-runtime.sh" "${FIREFOX_RUNTIME_DIR}"
                    RESULT_VARIABLE runtime_result)
    if(NOT runtime_result EQUAL 0)
        message(FATAL_ERROR "Building the runtime failed (scripts/build-runtime.sh)")
    endif()
    file(COPY ${FIREFOX_RUNTIME_DIR}/ DESTINATION ${PLUGIN_OUT}/runtime PATTERN ".runtime" EXCLUDE)
endif()

# Example app module: one web page full screen (examples/web-app/url).
if(FIREFOX_EXAMPLES)
    set(EXAMPLE_OUT ${CMAKE_BINARY_DIR}/examples/web-app.plugin)
    add_executable(web-app examples/web-app/main.cpp examples/web-app/i18n/ru.cpp)
    target_include_directories(web-app PRIVATE examples/web-app)
    target_link_libraries(web-app PRIVATE facet_webview)
    target_compile_options(web-app PRIVATE -Wall -Wextra)
    set_target_properties(web-app PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${EXAMPLE_OUT})
    facet_plugin_manifest(${CMAKE_CURRENT_SOURCE_DIR}/examples/web-app/manifest.json ${EXAMPLE_OUT}/manifest.json)
    configure_file(examples/web-app/url ${EXAMPLE_OUT}/url COPYONLY)
endif()

enable_testing()
add_executable(firefox_test tests/firefox_test.cpp)
target_link_libraries(firefox_test PRIVATE firefox_core)
add_test(NAME firefox COMMAND firefox_test)
