#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <esp_timer.h>

#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

#include "agora/agora_agent_manager.h"
#include "agora/agora_audio_bridge.h"
#include "agora/agora_rtc_session.h"
#include "agora/agora_video_bridge.h"
#include "audio_service.h"
#include "conversation_types.h"
#include "device_state.h"
#include "device_state_machine.h"

#define MAIN_EVENT_SCHEDULE             (1 << 0)
#define MAIN_EVENT_VAD_CHANGE           (1 << 1)
#define MAIN_EVENT_ERROR                (1 << 2)
#define MAIN_EVENT_CLOCK_TICK           (1 << 3)
#define MAIN_EVENT_NETWORK_CONNECTED    (1 << 4)
#define MAIN_EVENT_NETWORK_DISCONNECTED (1 << 5)
#define MAIN_EVENT_TOGGLE_CHAT          (1 << 6)
#define MAIN_EVENT_START_LISTENING      (1 << 7)
#define MAIN_EVENT_STOP_LISTENING       (1 << 8)
#define MAIN_EVENT_STATE_CHANGED        (1 << 9)

enum AecMode {
    kAecOff,
    kAecOnDeviceSide,
    kAecOnServerSide,
};

class Application {
public:
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    void Initialize();
    void Run();

    DeviceState GetDeviceState() const { return state_machine_.GetState(); }
    bool IsVoiceDetected() const { return audio_service_.IsVoiceDetected(); }
    bool SetDeviceState(DeviceState state);

    void Schedule(std::function<void()>&& callback);
    void Alert(const char* status, const char* message, const char* emotion = "", const std::string_view& sound = "");
    void DismissAlert();

    void AbortSpeaking(AbortReason reason);
    void ToggleChatState();
    void StartListening();
    void StopListening();

    void Reboot();
    void WakeWordInvoke(const std::string& wake_word);
    bool CanEnterSleepMode();
    void SetAecMode(AecMode mode);
    AecMode GetAecMode() const { return aec_mode_; }
    void PlaySound(const std::string_view& sound);
    AudioService& GetAudioService() { return audio_service_; }
    void ResetConversationSession();

private:
    Application();
    ~Application();

    std::mutex mutex_;
    std::deque<std::function<void()>> main_tasks_;
    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t clock_timer_handle_ = nullptr;
    DeviceStateMachine state_machine_;
    AecMode aec_mode_ = kAecOff;
    std::string last_error_message_;
    AudioService audio_service_;
    AgoraAgentManager agora_agent_manager_;
    AgoraRtcSession agora_rtc_session_;
    AgoraAudioBridge agora_audio_bridge_;
    AgoraVideoBridge agora_video_bridge_;
    AudioCodec* audio_codec_ = nullptr;
    std::atomic<bool> network_connected_{false};
    std::atomic<bool> session_running_{false};
    std::atomic<bool> session_command_pending_{false};
    std::atomic<bool> stop_requested_{false};
    int clock_ticks_ = 0;

    void HandleStateChangedEvent();
    void HandleToggleChatEvent();
    void HandleStartListeningEvent();
    void HandleStopListeningEvent();
    void HandleNetworkConnectedEvent();
    void HandleNetworkDisconnectedEvent();
    void ShowWifiConfigPrompt();

    void StartSessionAsync();
    void StartAgoraRtcTask();
    void StopSessionAsync();
    bool StartAgoraAgentInternal();
    bool StartAgoraSessionInternal();
    void StopAgoraSessionInternal();
    void SetError(const std::string& error);
};

class TaskPriorityReset {
public:
    TaskPriorityReset(BaseType_t priority) {
        original_priority_ = uxTaskPriorityGet(NULL);
        vTaskPrioritySet(NULL, priority);
    }
    ~TaskPriorityReset() {
        vTaskPrioritySet(NULL, original_priority_);
    }

private:
    BaseType_t original_priority_;
};

#endif // _APPLICATION_H_
