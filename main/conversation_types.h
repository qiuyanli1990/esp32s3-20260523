#ifndef CONVERSATION_TYPES_H
#define CONVERSATION_TYPES_H

#include <cstdint>
#include <vector>

// Shared payload types for the active Sentino/Agora conversation pipeline.
struct AudioStreamPacket {
    int sample_rate = 0;
    int frame_duration = 0;
    uint32_t timestamp = 0;
    std::vector<uint8_t> payload;
};

enum AbortReason {
    kAbortReasonNone,
    kAbortReasonWakeWordDetected
};

#endif  // CONVERSATION_TYPES_H
