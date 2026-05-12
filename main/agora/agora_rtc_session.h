#ifndef AGORA_RTC_SESSION_H
#define AGORA_RTC_SESSION_H

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

#include "agora_agent_manager.h"

extern "C" {
#include "agora_rtc_api.h"
}

class AgoraRtcSession {
public:
    bool Start(const AgoraAgentConfig& config);
    void Stop();

    bool SendAudio(const void* data, size_t len);
    bool SendVideo(const void* data, size_t len, video_data_type_e data_type, bool key_frame, uint16_t frame_rate = 0);

    bool is_started() const { return started_; }
    bool is_in_channel() const { return in_channel_; }
    const std::string& last_error() const { return last_error_; }

    void SetIncomingAudioCallback(std::function<void(const void*, size_t)> callback) {
        incoming_audio_callback_ = std::move(callback);
    }
    void SetStreamMessageCallback(std::function<void(uint32_t, const std::string&)> callback) {
        stream_message_callback_ = std::move(callback);
    }

private:
    static AgoraRtcSession* instance_;

    static void OnJoinChannelSuccess(connection_id_t conn_id, uint32_t uid, int elapsed_ms);
    static void OnConnectionLost(connection_id_t conn_id);
    static void OnRejoinChannelSuccess(connection_id_t conn_id, uint32_t uid, int elapsed_ms);
    static void OnUserJoined(connection_id_t conn_id, uint32_t uid, int elapsed_ms);
    static void OnUserOffline(connection_id_t conn_id, uint32_t uid, int reason);
    static void OnAudioData(connection_id_t conn_id, uint32_t uid, uint16_t sent_ts, const void* data_ptr,
        size_t data_len, const audio_frame_info_t* info_ptr);
    static void OnStreamMessage(connection_id_t conn_id, uint32_t uid, int stream_id, const char* data,
        size_t length, uint64_t sent_ts);
    static void OnError(connection_id_t conn_id, int code, const char* msg);

    void SetError(const std::string& error);
    void HandleIncomingAudio(const void* data_ptr, size_t data_len);
    void HandleStreamMessage(uint32_t uid, const std::string& text);
    void HandleStreamChunk(uint32_t uid, const char* data, size_t length);

    connection_id_t conn_id_ = CONNECTION_ID_INVALID;
    AgoraAgentConfig config_{};
    std::string app_id_;
    std::function<void(const void*, size_t)> incoming_audio_callback_;
    std::function<void(uint32_t, const std::string&)> stream_message_callback_;
    std::string last_error_;
    bool started_ = false;
    bool in_channel_ = false;
    agora_rtc_event_handler_t event_handler_ = {};
    rtc_service_option_t service_opt_ = {};
    std::mutex stream_buffer_mutex_;
    std::unordered_map<uint32_t, std::string> stream_buffers_;
    std::unordered_map<uint32_t, std::string> last_stream_texts_;
};

#endif  // AGORA_RTC_SESSION_H
