find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)

add_executable(test_pipewire_target
    test_pipewire_target.cpp
    ${CMAKE_SOURCE_DIR}/src/common/audio/pipewire_device.cpp
)

target_include_directories(test_pipewire_target PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${PIPEWIRE_INCLUDE_DIRS}
)

target_link_libraries(test_pipewire_target PRIVATE
    ${PIPEWIRE_LIBRARIES}
)

add_test(NAME test_pipewire_target COMMAND test_pipewire_target)

find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME test_quality_metrics
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/quality/test_quality.py)

if(VINPUT_ENABLE_LOCAL_ASR)
    add_executable(asr_replay
        quality/asr_replay.cpp
        ${CMAKE_SOURCE_DIR}/src/daemon/asr/backends/refining_backend.cpp
        ${CMAKE_SOURCE_DIR}/src/daemon/asr/backends/sherpa_offline_backend.cpp
        ${CMAKE_SOURCE_DIR}/src/daemon/asr/backends/sherpa_streaming_backend.cpp
        ${CMAKE_SOURCE_DIR}/src/daemon/asr/hotword_utils.cpp
        ${CMAKE_SOURCE_DIR}/src/daemon/asr/vad_trimmer.cpp
    )
    target_compile_definitions(asr_replay PRIVATE
        VINPUT_VAD_MODEL_PATH="${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}/fcitx5-vinput/vad/silero_vad.onnx")
    target_link_libraries(asr_replay PRIVATE common sherpa-onnx-c-api pthread)
endif()
