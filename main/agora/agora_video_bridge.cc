#include "agora/agora_video_bridge.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

namespace {
constexpr char kTag[] = "AgoraVideo";
constexpr int64_t kMinVideoFrameIntervalUs = 1500000;
constexpr int64_t kSendFailureBackoffUs = 3000000;
constexpr size_t kMaxJpegFrameBytes = 16 * 1024;
constexpr size_t kMinInternalSramBytes = 48 * 1024;
}

bool AgoraVideoBridge::Start(Camera* camera, AgoraRtcSession* session) {
    if (running_) {
        return true;
    }
    if (camera == nullptr || session == nullptr) {
        return false;
    }

    camera_ = camera;
    session_ = session;
    last_frame_sent_us_ = 0;
    last_warn_us_ = 0;
    video_backoff_until_us_ = 0;
    running_ = camera_->StartVideoStream([this](const CameraVideoFrame& frame) {
        if (!running_ || session_ == nullptr || !session_->is_in_channel()) {
            return;
        }

        int64_t now_us = esp_timer_get_time();
        if (now_us < video_backoff_until_us_) {
            return;
        }
        if (last_frame_sent_us_ != 0 && (now_us - last_frame_sent_us_) < kMinVideoFrameIntervalUs) {
            return;
        }

        size_t free_internal_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        if (free_internal_sram < kMinInternalSramBytes) {
            if (last_warn_us_ == 0 || (now_us - last_warn_us_) > 5000000) {
                ESP_LOGW(kTag, "Skipping video frame due to low internal SRAM: %u bytes",
                    static_cast<unsigned>(free_internal_sram));
                last_warn_us_ = now_us;
            }
            return;
        }

        video_data_type_e data_type = VIDEO_DATA_TYPE_GENERIC_JPEG;
        switch (frame.format) {
            case CameraStreamFormat::kYuv420:
                data_type = VIDEO_DATA_TYPE_YUV420;
                break;
            case CameraStreamFormat::kH264:
                data_type = VIDEO_DATA_TYPE_H264;
                break;
            case CameraStreamFormat::kH265:
                data_type = VIDEO_DATA_TYPE_H265;
                break;
            case CameraStreamFormat::kJpeg:
            default:
                data_type = VIDEO_DATA_TYPE_GENERIC_JPEG;
                break;
        }

        if (data_type == VIDEO_DATA_TYPE_GENERIC_JPEG && frame.len > kMaxJpegFrameBytes) {
            return;
        }

        if (!session_->SendVideo(frame.data, frame.len, data_type, frame.key_frame, frame.frame_rate)) {
            ESP_LOGW(kTag, "Failed to send video frame");
            video_backoff_until_us_ = now_us + kSendFailureBackoffUs;
            return;
        }
        last_frame_sent_us_ = now_us;
    });
    return running_;
}

void AgoraVideoBridge::Stop() {
    if (!running_) {
        return;
    }
    running_ = false;
    if (camera_ != nullptr) {
        camera_->StopVideoStream();
    }
}
