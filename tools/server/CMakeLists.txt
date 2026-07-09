include_directories(${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})

# server-context containing the core server logic, used by llama-server and CLI

set(TARGET server-context)

add_library(${TARGET} STATIC
    server-chat.cpp
    server-chat.h
    server-task.cpp
    server-task.h
    server-queue.cpp
    server-queue.h
    server-common.cpp
    server-common.h
    server-context.cpp
    server-context.h
    server-stream.cpp
    server-stream.h
    server-tools.cpp
    server-tools.h
    server-schema.cpp
    server-schema.h
)

if (BUILD_SHARED_LIBS)
    set_target_properties(${TARGET} PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

target_include_directories(${TARGET} PRIVATE ../mtmd)
target_include_directories(${TARGET} PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(${TARGET} PUBLIC llama-common mtmd ${CMAKE_THREAD_LIBS_INIT})

# llama-server-impl: server logic, reusable by app

set(TARGET llama-server-impl)

add_library(${TARGET}
    caliber-plan.cpp
    caliber-plan.h
    caliber-scoring.cpp
    caliber-scoring.h
    server.cpp
    server-ds4.cpp
    server-ds4.h
    server-fit-advisor.cpp
    server-fit-advisor.h
    server-http.cpp
    server-http.h
    server-models.cpp
    server-models.h
)
set_target_properties(${TARGET} PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
target_compile_definitions(${TARGET} PRIVATE
    LLAMA_DS4_EVAL_CASES_PATH="${CMAKE_CURRENT_SOURCE_DIR}/ds4-eval-cases.json"
    LLAMA_DS4_REPORTS_DIR="${CMAKE_SOURCE_DIR}/tools/ui/static/reports"
)

target_include_directories(${TARGET} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(${TARGET} PRIVATE ../mtmd ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/vendor ${CMAKE_SOURCE_DIR}/ggml/include)
target_link_libraries(${TARGET} PUBLIC server-context llama-ui cpp-httplib ${CMAKE_THREAD_LIBS_INIT})

if(LLAMA_TOOLS_INSTALL)
    install(TARGETS ${TARGET} LIBRARY)
endif()

add_executable(test-caliber-scoring EXCLUDE_FROM_ALL
    tests/test-caliber-scoring.cpp
    caliber-scoring.cpp
    caliber-scoring.h
)
target_compile_features(test-caliber-scoring PRIVATE cxx_std_17)
target_compile_definitions(test-caliber-scoring PRIVATE
    CALIBER_WINNER_FIXTURE_PATH="/home/cooper/calibr/tests/fixtures/winner-policy-cases.json"
)
target_include_directories(test-caliber-scoring PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/vendor)

add_executable(test-caliber-plan EXCLUDE_FROM_ALL
    tests/test-caliber-plan.cpp
    caliber-plan.cpp
    caliber-plan.h
    caliber-scoring.cpp
    caliber-scoring.h
)
target_compile_features(test-caliber-plan PRIVATE cxx_std_17)
target_include_directories(test-caliber-plan PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/vendor
    ${CMAKE_SOURCE_DIR}/ggml/include
)
target_link_libraries(test-caliber-plan PRIVATE ggml)

# llama-server executable

set(TARGET llama-server)

add_executable(${TARGET} main.cpp)
install(TARGETS ${TARGET} RUNTIME)

target_link_libraries(${TARGET} PRIVATE llama-server-impl)
target_compile_features(${TARGET} PRIVATE cxx_std_17)
