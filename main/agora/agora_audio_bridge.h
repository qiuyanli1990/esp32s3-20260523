#ifndef AGORA_AUDIO_BRIDGE_H
#define AGORA_AUDIO_BRIDGE_H

#include <cstddef>

#include "agora_rtc_session.h"
#include "audio_service.h"
#include "realtime_audio_port.h"
#include "realtime_audio_service.h"

class AgoraAudioBridge {
public:
    AgoraAudioBridge() = default;

    bool Start(RealtimeAudioPort* audio_port, AudioService* audio_service, AgoraRtcSession* session,
        int rtc_sample_rate, int pcm_frame_bytes, bool use_processed_input);
    void Stop();
    void PushIncomingPcm(const void* data, size_t len);

    bool is_running() const { return realtime_audio_service_.is_running(); }

private:
    RealtimeAudioService realtime_audio_service_;
};

#endif  // AGORA_AUDIO_BRIDGE_H
