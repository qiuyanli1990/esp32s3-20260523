#include "agora/agora_audio_bridge.h"

bool AgoraAudioBridge::Start(RealtimeAudioPort* audio_port, AudioService* audio_service, AgoraRtcSession* session,
    int rtc_sample_rate, int pcm_frame_bytes, bool use_processed_input) {
    if (session == nullptr) {
        return false;
    }

    return realtime_audio_service_.Start(audio_port, audio_service,
        rtc_sample_rate, pcm_frame_bytes, use_processed_input,
        [session]() {
            return session->is_started() && session->is_in_channel();
        },
        [session](const int16_t* data, size_t bytes) {
            return session->is_started() && session->is_in_channel() && session->SendAudio(data, bytes);
        });
}

void AgoraAudioBridge::Stop() {
    realtime_audio_service_.Stop();
}

void AgoraAudioBridge::PushIncomingPcm(const void* data, size_t len) {
    realtime_audio_service_.PushIncomingPcm(data, len);
}
