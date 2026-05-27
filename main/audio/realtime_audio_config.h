#ifndef REALTIME_AUDIO_CONFIG_H
#define REALTIME_AUDIO_CONFIG_H

#include <cstddef>
#include <cstdint>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

enum class RealtimePlaybackBufferMode {
    kFrameQueue,
    kByteStream,
};

struct RealtimeAudioConfig {
    uint32_t capture_task_stack_size = 4096;
    uint32_t playback_task_stack_size = 4096;
    size_t max_playback_queue_frames = 24;
    size_t playback_prebuffer_frames = 5;
    size_t playback_latency_high_water_frames = 0;
    size_t playback_latency_target_frames = 0;
    UBaseType_t capture_task_priority = 5;
    UBaseType_t playback_task_priority = 6;
    BaseType_t playback_task_core = 1;
    bool pin_capture_task = false;
    BaseType_t capture_task_core = tskNO_AFFINITY;
    bool prefer_spiram_task_stack = false;
    RealtimePlaybackBufferMode playback_buffer_mode = RealtimePlaybackBufferMode::kFrameQueue;
    const char* profile_name = "generic";
};

#endif  // REALTIME_AUDIO_CONFIG_H
