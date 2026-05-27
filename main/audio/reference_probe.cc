#include "reference_probe.h"

#include "audio_codec.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "AudioRefProbe"

#ifndef CONFIG_AUDIO_REFERENCE_PROBE_BASELINE_MS
#define CONFIG_AUDIO_REFERENCE_PROBE_BASELINE_MS 1000
#endif

#ifndef CONFIG_AUDIO_REFERENCE_PROBE_STIMULUS_MS
#define CONFIG_AUDIO_REFERENCE_PROBE_STIMULUS_MS 3000
#endif

#ifndef CONFIG_AUDIO_REFERENCE_PROBE_CHUNK_MS
#define CONFIG_AUDIO_REFERENCE_PROBE_CHUNK_MS 20
#endif

#ifndef CONFIG_AUDIO_REFERENCE_PROBE_AMPLITUDE
#define CONFIG_AUDIO_REFERENCE_PROBE_AMPLITUDE 5000
#endif

#ifndef CONFIG_AUDIO_REFERENCE_PROBE_MAX_LAG_MS
#define CONFIG_AUDIO_REFERENCE_PROBE_MAX_LAG_MS 80
#endif

namespace audio {
namespace {

struct RmsStats {
    int64_t count = 0;
    int64_t sum = 0;
    double sum_squares = 0.0;
    int peak = 0;

    void Add(int16_t sample) {
        const int value = sample;
        ++count;
        sum += value;
        sum_squares += static_cast<double>(value) * value;
        peak = std::max(peak, std::abs(value));
    }

    double Mean() const {
        return count > 0 ? static_cast<double>(sum) / count : 0.0;
    }

    double Rms() const {
        return count > 0 ? std::sqrt(sum_squares / count) : 0.0;
    }
};

struct CorrStats {
    double dot = 0.0;
    double x2 = 0.0;
    double y2 = 0.0;
    int64_t count = 0;

    void Add(int16_t x, int16_t y) {
        dot += static_cast<double>(x) * y;
        x2 += static_cast<double>(x) * x;
        y2 += static_cast<double>(y) * y;
        ++count;
    }

    double Value() const {
        if (count == 0 || x2 <= 0.0 || y2 <= 0.0) {
            return 0.0;
        }
        return dot / std::sqrt(x2 * y2);
    }
};

struct ProbeSignal {
    uint32_t lfsr = 0x13579bdf;
    int32_t smoothed = 0;

    int16_t NextSample() {
        uint32_t bit = ((lfsr >> 0) ^ (lfsr >> 2) ^ (lfsr >> 3) ^ (lfsr >> 5)) & 1;
        lfsr = (lfsr >> 1) | (bit << 31);
        const int32_t target = (lfsr & 1) ? CONFIG_AUDIO_REFERENCE_PROBE_AMPLITUDE : -CONFIG_AUDIO_REFERENCE_PROBE_AMPLITUDE;
        smoothed = (smoothed * 3 + target) / 4;
        return static_cast<int16_t>(smoothed);
    }
};

class OutputHistory {
public:
    explicit OutputHistory(int capacity)
        : samples_(std::max(capacity, 1), 0) {
    }

    size_t NextIndex() const {
        return write_index_;
    }

    void Push(int16_t sample) {
        samples_[write_index_ % samples_.size()] = sample;
        ++write_index_;
    }

    bool GetAt(size_t absolute_index, int16_t* sample) const {
        if (absolute_index >= write_index_ || write_index_ - absolute_index > samples_.size()) {
            return false;
        }
        *sample = samples_[absolute_index % samples_.size()];
        return true;
    }

private:
    std::vector<int16_t> samples_;
    size_t write_index_ = 0;
};

double RatioDb(double signal, double baseline) {
    return 20.0 * std::log10(std::max(signal, 1.0) / std::max(baseline, 1.0));
}

void AddInterleavedStats(const std::vector<int16_t>& data, int frames, int channels,
                         std::vector<RmsStats>* stats) {
    for (int frame = 0; frame < frames; ++frame) {
        const int base = frame * channels;
        for (int channel = 0; channel < channels; ++channel) {
            (*stats)[channel].Add(data[base + channel]);
        }
    }
}

std::vector<int> BuildLagSamples(int sample_rate) {
    static constexpr std::array<int, 8> kLagMs = {0, 2, 5, 10, 20, 40, 60, 80};
    std::vector<int> lags;
    for (int lag_ms : kLagMs) {
        if (lag_ms <= CONFIG_AUDIO_REFERENCE_PROBE_MAX_LAG_MS) {
            lags.push_back(sample_rate * lag_ms / 1000);
        }
    }
    if (lags.empty()) {
        lags.push_back(0);
    }
    return lags;
}

}  // namespace

void RunReferenceProbe(AudioCodec* codec) {
    if (codec == nullptr) {
        ESP_LOGW(TAG, "Skipping reference probe: codec is null");
        return;
    }

    const int channels = codec->GetReferenceProbeChannelCount();
    if (channels <= 0) {
        ESP_LOGW(TAG, "Skipping reference probe: codec does not expose probe channels");
        return;
    }

    const int input_rate = codec->input_sample_rate();
    const int output_rate = codec->output_sample_rate();
    if (input_rate <= 0 || output_rate <= 0) {
        ESP_LOGW(TAG, "Skipping reference probe: invalid sample rates input=%d output=%d",
            input_rate, output_rate);
        return;
    }
    const int input_frames_per_chunk = std::max(1, input_rate * CONFIG_AUDIO_REFERENCE_PROBE_CHUNK_MS / 1000);
    const int output_samples_per_chunk = std::max(1, output_rate * CONFIG_AUDIO_REFERENCE_PROBE_CHUNK_MS / 1000);
    const bool can_correlate = input_rate == output_rate && input_frames_per_chunk == output_samples_per_chunk;
    const std::vector<int> lag_samples = BuildLagSamples(input_rate);
    const int max_lag_samples = lag_samples.empty() ? 0 : lag_samples.back();

    ESP_LOGW(TAG, "Reference probe enabled. Keep near-end quiet while the test signal plays.");
    ESP_LOGI(TAG, "input_rate=%d output_rate=%d channels=%d baseline=%dms stimulus=%dms chunk=%dms corr=%s",
        input_rate, output_rate, channels,
        CONFIG_AUDIO_REFERENCE_PROBE_BASELINE_MS,
        CONFIG_AUDIO_REFERENCE_PROBE_STIMULUS_MS,
        CONFIG_AUDIO_REFERENCE_PROBE_CHUNK_MS,
        can_correlate ? "on" : "off(sample-rate mismatch)");

    const bool restore_input_disabled = !codec->input_enabled();
    const bool restore_output_disabled = !codec->output_enabled();
    codec->EnableInput(true);
    codec->EnableOutput(true);
    vTaskDelay(pdMS_TO_TICKS(120));

    std::vector<int16_t> input_buffer(input_frames_per_chunk * channels);
    std::vector<int16_t> output_buffer(output_samples_per_chunk);
    std::vector<RmsStats> baseline_stats(channels);
    std::vector<RmsStats> stimulus_stats(channels);
    std::vector<std::vector<CorrStats>> corr_stats(
        channels, std::vector<CorrStats>(lag_samples.size()));
    OutputHistory output_history(max_lag_samples + output_samples_per_chunk + 1);

    const int baseline_chunks = std::max(1, CONFIG_AUDIO_REFERENCE_PROBE_BASELINE_MS / CONFIG_AUDIO_REFERENCE_PROBE_CHUNK_MS);
    std::fill(output_buffer.begin(), output_buffer.end(), 0);
    for (int chunk = 0; chunk < baseline_chunks; ++chunk) {
        codec->WriteReferenceProbeData(output_buffer.data(), output_buffer.size());
        int actual_channels = channels;
        int frames = codec->ReadReferenceProbeData(input_buffer.data(), input_frames_per_chunk, &actual_channels);
        if (frames <= 0 || actual_channels != channels) {
            ESP_LOGW(TAG, "Baseline read failed: frames=%d channels=%d expected_channels=%d",
                frames, actual_channels, channels);
            continue;
        }
        AddInterleavedStats(input_buffer, frames, channels, &baseline_stats);
    }

    ProbeSignal signal;
    const int stimulus_chunks = std::max(1, CONFIG_AUDIO_REFERENCE_PROBE_STIMULUS_MS / CONFIG_AUDIO_REFERENCE_PROBE_CHUNK_MS);
    for (int chunk = 0; chunk < stimulus_chunks; ++chunk) {
        const size_t output_chunk_start = output_history.NextIndex();
        for (int i = 0; i < output_samples_per_chunk; ++i) {
            output_buffer[i] = signal.NextSample();
            if (can_correlate) {
                output_history.Push(output_buffer[i]);
            }
        }
        codec->WriteReferenceProbeData(output_buffer.data(), output_buffer.size());

        int actual_channels = channels;
        int frames = codec->ReadReferenceProbeData(input_buffer.data(), input_frames_per_chunk, &actual_channels);
        if (frames <= 0 || actual_channels != channels) {
            ESP_LOGW(TAG, "Stimulus read failed: frames=%d channels=%d expected_channels=%d",
                frames, actual_channels, channels);
            continue;
        }

        AddInterleavedStats(input_buffer, frames, channels, &stimulus_stats);
        if (can_correlate) {
            for (int frame = 0; frame < frames; ++frame) {
                const int base = frame * channels;
                for (int lag_index = 0; lag_index < static_cast<int>(lag_samples.size()); ++lag_index) {
                    if (output_chunk_start + frame < static_cast<size_t>(lag_samples[lag_index])) {
                        continue;
                    }
                    int16_t reference_sample = 0;
                    const size_t output_index = output_chunk_start + frame - lag_samples[lag_index];
                    if (!output_history.GetAt(output_index, &reference_sample)) {
                        continue;
                    }
                    for (int channel = 0; channel < channels; ++channel) {
                        corr_stats[channel][lag_index].Add(reference_sample, input_buffer[base + channel]);
                    }
                }
            }
        }
    }

    std::fill(output_buffer.begin(), output_buffer.end(), 0);
    for (int i = 0; i < 5; ++i) {
        codec->WriteReferenceProbeData(output_buffer.data(), output_buffer.size());
    }

    int best_channel = -1;
    double best_score = -1.0;
    double best_corr = 0.0;
    int best_lag_ms = 0;

    ESP_LOGI(TAG, "Reference probe result:");
    for (int channel = 0; channel < channels; ++channel) {
        double channel_best_corr = 0.0;
        int channel_best_lag_ms = 0;
        if (can_correlate) {
            for (int lag_index = 0; lag_index < static_cast<int>(lag_samples.size()); ++lag_index) {
                const double corr = std::abs(corr_stats[channel][lag_index].Value());
                if (corr > channel_best_corr) {
                    channel_best_corr = corr;
                    channel_best_lag_ms = lag_samples[lag_index] * 1000 / input_rate;
                }
            }
        }

        const double baseline_rms = baseline_stats[channel].Rms();
        const double stimulus_rms = stimulus_stats[channel].Rms();
        const double delta_db = RatioDb(stimulus_rms, baseline_rms);
        const double score = delta_db + channel_best_corr * 20.0;
        if (score > best_score) {
            best_score = score;
            best_channel = channel;
            best_corr = channel_best_corr;
            best_lag_ms = channel_best_lag_ms;
        }

        ESP_LOGI(TAG, "  %s baseline_rms=%.1f stimulus_rms=%.1f delta=%.1fdB peak=%d corr=%.3f lag=%dms mean=%.1f",
            codec->GetReferenceProbeChannelLabel(channel).c_str(),
            baseline_rms,
            stimulus_rms,
            delta_db,
            stimulus_stats[channel].peak,
            channel_best_corr,
            channel_best_lag_ms,
            stimulus_stats[channel].Mean());
    }

    if (best_channel >= 0) {
        ESP_LOGW(TAG, "Best reference candidate: %s score=%.1f corr=%.3f lag=%dms",
            codec->GetReferenceProbeChannelLabel(best_channel).c_str(),
            best_score, best_corr, best_lag_ms);
        ESP_LOGW(TAG, "Use the candidate only if it rises with speaker playback and stays low for near-end speech.");
    }

    if (restore_input_disabled) {
        codec->EnableInput(false);
    }
    if (restore_output_disabled) {
        codec->EnableOutput(false);
    }
}

}  // namespace audio
