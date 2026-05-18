#ifndef VOICEPRINT_UPLOADER_H
#define VOICEPRINT_UPLOADER_H

#include <functional>
#include <string>

class AudioCodec;
class AudioService;

class VoiceprintUploader {
public:
    static bool IsEnabled();
    static bool CaptureAndUpload(AudioService& audio_service, AudioCodec* codec,
        std::string* sample_url, std::string* error,
        std::function<void(const char*)> status_callback = nullptr);
};

#endif  // VOICEPRINT_UPLOADER_H
