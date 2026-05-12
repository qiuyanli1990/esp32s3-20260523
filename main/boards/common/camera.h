#ifndef CAMERA_H
#define CAMERA_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

enum class CameraStreamFormat {
    kYuv420,
    kH264,
    kH265,
    kJpeg,
};

struct CameraVideoFrame {
    const uint8_t* data = nullptr;
    size_t len = 0;
    CameraStreamFormat format = CameraStreamFormat::kJpeg;
    bool key_frame = true;
    uint16_t frame_rate = 0;
};

class Camera {
public:
    using VideoStreamCallback = std::function<void(const CameraVideoFrame&)>;

    virtual void SetExplainUrl(const std::string& url, const std::string& token) = 0;
    virtual bool Capture() = 0;
    virtual bool SetHMirror(bool enabled) = 0;
    virtual bool SetVFlip(bool enabled) = 0;
    virtual bool SetSwapBytes(bool enabled) { return false; }  // Optional, default no-op
    virtual void SetSessionActive(bool active) { (void)active; }
    virtual bool StartVideoStream(VideoStreamCallback callback) {
        (void)callback;
        return false;
    }
    virtual void StopVideoStream() {}
    virtual std::string Explain(const std::string& question) = 0;
};

#endif // CAMERA_H
