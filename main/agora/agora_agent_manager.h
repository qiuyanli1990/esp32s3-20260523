#ifndef AGORA_AGENT_MANAGER_H
#define AGORA_AGENT_MANAGER_H

#include <atomic>
#include <cstdint>
#include <string>

struct AgoraAgentConfig {
    std::string agent_name;
    std::string user_id;
    std::string device_id;
    std::string conversation_id;
    std::string channel_name;
    std::string app_id;
    std::string rtc_token;
    uint32_t agent_rtc_uid = 0;
    uint32_t remote_rtc_uid = 0;
    int rtc_audio_codec_type = 0;
    int pcm_sample_rate = 16000;
    int pcm_frame_bytes = 640;
};

class AgoraAgentManager {
public:
    bool Initialize();
    bool Start();
    void Stop();
    void StopAsync();

    bool is_ready() const { return ready_; }
    bool is_running() const { return running_; }
    bool is_stop_in_progress() const { return stop_in_progress_; }
    const std::string& agent_id() const { return config_.agent_name; }
    const std::string& conversation_id() const { return conversation_id_; }
    const AgoraAgentConfig& config() const { return config_; }
    const std::string& last_error() const { return last_error_; }

private:
    bool LoadStaticConfig();
    bool BuildStartJson(std::string* body);
    bool StartRequest();
    bool StopConversationById(const std::string& conversation_id);
    void ClearRunningState();
    std::string BuildAuthHeader() const;
    bool WaitForPendingStop(int timeout_ms);
    void SetError(const std::string& error);

    AgoraAgentConfig config_;
    std::string conversation_id_;
    std::string last_error_;
    bool ready_ = false;
    bool running_ = false;
    std::atomic<bool> stop_in_progress_{false};
    std::atomic<bool> last_stop_success_{false};
};

#endif  // AGORA_AGENT_MANAGER_H
