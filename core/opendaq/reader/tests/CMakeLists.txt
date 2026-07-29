opendaq_set_cmake_folder_context(TARGET_FOLDER_NAME)
set(MODULE_NAME reader)

set(TEST_HEADERS reader_common.h
)

set(TEST_SOURCES test_factories.cpp
                 test_tail_reader.cpp
                 test_packet_reader.cpp
                 test_stream_reader.cpp
                 test_date.cpp
                 test_block_reader.cpp
                 test_time_reader.cpp
                 test_multi_reader.cpp
                 test_stream_reader_from_input_port.cpp
                 test_domain_value.cpp
                 test_typed_reading.cpp
                 test_queue_reader.cpp
                 test_multi_reader_input.cpp
                 test_multi_reader_state_transitions.cpp
                 test_synchronization_manager.cpp
                 test_callback_gate.cpp
                 test_notification_coordinator.cpp
                 test_read_coordinator.cpp
                 test_data_loss_monitor.cpp
                 ../src/typed_reading_utils.cpp
                 ../src/multi_reader/queue_reader.cpp
                 ../src/multi_reader/input.cpp
                 ../src/multi_reader/synchronization_manager.cpp
                 ../src/multi_reader/notification_coordinator.cpp
                 ../src/multi_reader/read_coordinator.cpp
                 ../src/multi_reader/data_loss_monitor.cpp
)

opendaq_prepare_test_runner(TEST_APP FOR ${MODULE_NAME}
                       SOURCES
                           test_app.cpp
                           ${TEST_SOURCES}
                           ${TEST_HEADERS}
)

if (MSVC)
    set_source_files_properties(
        test_stream_reader.cpp
        PROPERTIES
            COMPILE_FLAGS /wd4309 # truncation of constant value
    )

    target_compile_options(${TEST_APP} PRIVATE /bigobj)
endif()


add_test(NAME ${TEST_APP}
         COMMAND $<TARGET_FILE_NAME:${TEST_APP}>
         WORKING_DIRECTORY $<TARGET_FILE_DIR:${TEST_APP}>
)

target_link_libraries(${TEST_APP} PRIVATE daq::coreobjects daq::opendaq
)

if (OPENDAQ_ENABLE_COVERAGE)
    setup_target_for_coverage(${TEST_APP}coverage ${TEST_APP} ${TEST_APP}coverage)
endif()

# Standalone multi reader performance benchmark (see bench_multi_reader.cpp). Not a test and
# EXCLUDE_FROM_ALL: built only on demand (cmake --build ... --target bench_multi_reader) for
# perf comparison, never as part of the normal/CI build. Uses only the public API so the same
# source builds against the pre-rework branch too.
add_executable(bench_multi_reader EXCLUDE_FROM_ALL bench_multi_reader.cpp)
target_link_libraries(bench_multi_reader PRIVATE daq::coreobjects daq::opendaq)
if (MSVC)
    target_compile_options(bench_multi_reader PRIVATE /bigobj)
endif()
