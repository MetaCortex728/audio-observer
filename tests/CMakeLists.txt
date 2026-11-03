add_library(doctest INTERFACE)
target_include_directories(doctest SYSTEM INTERFACE
    ${PROJECT_SOURCE_DIR}/third_party/doctest)

add_executable(audio_observer_tests
    main.cpp
    test_backend.cpp
    test_block_queue.cpp
    test_core_types.cpp
    test_dropout_detector.cpp
    test_meters.cpp
    test_session.cpp
    test_spsc_ring_buffer.cpp
    test_version.cpp)
target_link_libraries(audio_observer_tests PRIVATE
    audio_observer::audio_observer doctest audio_observer_options Threads::Threads)
target_compile_definitions(audio_observer_tests PRIVATE
    AUDIO_OBSERVER_EXPECTED_VERSION="${PROJECT_VERSION}")

add_test(NAME audio_observer_tests COMMAND audio_observer_tests)
