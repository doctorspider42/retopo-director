add_executable(rd-tests
    test_main.cpp
    test_core.cpp
    test_mesh.cpp
    test_pipeline.cpp)
target_link_libraries(rd-tests PRIVATE rd_engine)
if(MINGW)
    target_link_options(rd-tests PRIVATE -static-libgcc -static-libstdc++ -static)
endif()
if(NOT MSVC)
    target_compile_options(rd-tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

add_test(NAME unit COMMAND rd-tests)
set_tests_properties(unit PROPERTIES TIMEOUT 300)

# Whole runs of the real executable on the synthetic meshes, through the same
# script the benchmark uses. Needs Python with numpy to generate the meshes;
# without it the unit tests still run and these are simply not registered.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    execute_process(COMMAND ${Python3_EXECUTABLE} -c "import numpy"
                    RESULT_VARIABLE RD_HAVE_NUMPY OUTPUT_QUIET ERROR_QUIET)
    if(RD_HAVE_NUMPY EQUAL 0)
        # --no-gpu keeps it runnable on a CI box with no driver; the silhouette
        # metric is then absent, and the benchmark covers it on a real machine.
        add_test(NAME pipeline_synthetic
                 COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/bench.py
                         --synthetic-only --require-pass
                         --exe $<TARGET_FILE:retopo-director>
                         --out ${CMAKE_BINARY_DIR}/ctest-bench
                         -- --no-gpu)
        set_tests_properties(pipeline_synthetic PROPERTIES TIMEOUT 900)
    else()
        message(STATUS "numpy not found: pipeline_synthetic test not registered")
    endif()
endif()
