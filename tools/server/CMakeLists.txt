include_directories(${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})

find_package(SQLite3 REQUIRED)

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
    model-registry.cpp
    model-registry.h
    model-operations.cpp
    model-operations.h
    least-cost-router.cpp
    least-cost-router.h
    quality-evidence.cpp
    quality-evidence.h
    streaming-profiler.cpp
    streaming-profiler.h
    server-caliber-advisor.cpp
    server-caliber-advisor.h
    server.cpp
    server-ds4.cpp
    server-ds4.h
    server-fit-advisor.cpp
    server-fit-advisor.h
    server-http.cpp
    server-http.h
    server-models.cpp
    server-models.h
    server-persistence.cpp
    server-persistence.h
)
set_target_properties(${TARGET} PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
target_compile_definitions(${TARGET} PRIVATE
    LLAMA_DS4_EVAL_CASES_PATH="${CMAKE_CURRENT_SOURCE_DIR}/ds4-eval-cases.json"
    LLAMA_LOCAL_QUALITY_PACKS_PATH="${CMAKE_CURRENT_SOURCE_DIR}/local-quality-packs.json"
)

target_include_directories(${TARGET} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(${TARGET} PRIVATE ../mtmd ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/vendor ${CMAKE_SOURCE_DIR}/ggml/include)
target_link_libraries(${TARGET} PUBLIC server-context llama-ui cpp-httplib SQLite::SQLite3 ${CMAKE_THREAD_LIBS_INIT})

if(LLAMA_TOOLS_INSTALL)
    install(TARGETS ${TARGET} LIBRARY)
endif()

if(LLAMA_BUILD_TESTS)
    add_executable(test-caliber-scoring
        tests/test-caliber-scoring.cpp
        caliber-scoring.cpp
        caliber-scoring.h
    )
    target_compile_features(test-caliber-scoring PRIVATE cxx_std_17)
    target_compile_definitions(test-caliber-scoring PRIVATE
        CALIBER_WINNER_FIXTURE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/caliber-winner-policy-cases.json"
    )
    target_include_directories(test-caliber-scoring PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/vendor)

    add_executable(test-caliber-plan
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

    add_test(NAME caliber-scoring COMMAND test-caliber-scoring)
    add_test(NAME caliber-plan COMMAND test-caliber-plan)

    add_executable(test-model-registry
        tests/test-model-registry.cpp
        model-registry.cpp
        model-registry.h
        caliber-plan.cpp
        caliber-plan.h
        caliber-scoring.cpp
        caliber-scoring.h
    )
    target_compile_features(test-model-registry PRIVATE cxx_std_17)
    target_include_directories(test-model-registry PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/vendor
        ${CMAKE_SOURCE_DIR}/ggml/include
    )
    target_link_libraries(test-model-registry PRIVATE ggml)
    add_test(NAME model-registry COMMAND test-model-registry)

    add_executable(test-server-persistence tests/test-server-persistence.cpp)
    target_compile_features(test-server-persistence PRIVATE cxx_std_17)
    target_include_directories(test-server-persistence PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(test-server-persistence PRIVATE llama-server-impl SQLite::SQLite3)
    add_test(NAME server-persistence COMMAND test-server-persistence)

    add_executable(test-model-operations tests/test-model-operations.cpp model-operations.cpp model-operations.h)
    target_compile_features(test-model-operations PRIVATE cxx_std_17)
    target_include_directories(test-model-operations PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME model-operations COMMAND test-model-operations)

    add_executable(test-streaming-profiler tests/test-streaming-profiler.cpp)
    target_compile_features(test-streaming-profiler PRIVATE cxx_std_17)
    target_include_directories(test-streaming-profiler PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(test-streaming-profiler PRIVATE llama-server-impl)
    add_test(NAME streaming-profiler COMMAND test-streaming-profiler)

    add_executable(test-quality-evidence tests/test-quality-evidence.cpp quality-evidence.cpp)
    target_compile_features(test-quality-evidence PRIVATE cxx_std_17)
    target_include_directories(test-quality-evidence PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/vendor)
    add_test(NAME quality-evidence COMMAND test-quality-evidence)

    add_executable(test-least-cost-router tests/test-least-cost-router.cpp least-cost-router.cpp)
    target_compile_features(test-least-cost-router PRIVATE cxx_std_17)
    target_include_directories(test-least-cost-router PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/vendor)
    add_test(NAME least-cost-router COMMAND test-least-cost-router)
endif()

# llama-server executable

set(TARGET llama-server)

add_executable(${TARGET} main.cpp)
install(TARGETS ${TARGET} RUNTIME)

target_link_libraries(${TARGET} PRIVATE llama-server-impl)
target_compile_features(${TARGET} PRIVATE cxx_std_17)
