#ifndef AGORA_VIDEO_BRIDGE_H
#define AGORA_VIDEO_BRIDGE_H

#include "agora_rtc_session.h"
#include "camera.h"

class AgoraVideoBridge {
public:
    bool Start(Camera* camera, AgoraRtcSession* session);
    void Stop();
    bool is_running() const { return running_; }

private:
    AgoraRtcSession* session_ = nullptr;
    Camera* camera_ = nullptr;
    bool running_ = false;
    int64_t last_frame_sent_us_ = 0;
    int64_t last_warn_us_ = 0;
    int64_t video_backoff_until_us_ = 0;
};

#endif  // AGORA_VIDEO_BRIDGE_H
