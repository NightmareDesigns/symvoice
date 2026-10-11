cmake_minimum_required(VERSION 3.20)
project(symvoice C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_executable(symvoice-audio native/src/main.c)

if(WIN32)
    target_link_libraries(symvoice-audio PRIVATE winmm)

    if(MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 4)
        set(SAPI_EXPORTS_FILE native/src/symvoice-sapi-x86.def)
    else()
        set(SAPI_EXPORTS_FILE native/src/symvoice-sapi.def)
    endif()
    add_library(symvoice-sapi SHARED
        native/src/sapi_engine.c
        ${SAPI_EXPORTS_FILE}
    )
    target_compile_definitions(symvoice-sapi PRIVATE CINTERFACE COBJMACROS)
    target_link_libraries(symvoice-sapi PRIVATE sapi ole32 winhttp)
endif()

if(MSVC)
    target_compile_options(symvoice-audio PRIVATE /W4)
    if(WIN32)
        target_compile_options(symvoice-sapi PRIVATE /W4)
    endif()
else()
    target_compile_options(symvoice-audio PRIVATE -Wall -Wextra -Wpedantic)
    if(WIN32)
        target_compile_options(symvoice-sapi PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

include(CTest)

if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(
        NAME prepare-audio
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_prepare_audio.py"
                "$<TARGET_FILE:symvoice-audio>"
    )
endif()
