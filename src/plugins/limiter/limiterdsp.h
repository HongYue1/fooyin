#pragma once

#include <core/engine/dsp/dspnode.h>

#include <QByteArray>
#include <QString>

#include <atomic>
#include <cstdint>
#include <vector>

namespace Fooyin::Limiter {
class LimiterDsp : public DspNode
{
public:
    struct Settings
    {
        bool enabled{true};

        double ceilingDb{-1.0};
        double inputGainDb{0.0};
        double attackMs{0.0};
        double releaseMs{200.0};
        double lookaheadMs{5.0};
        double kneeDb{3.0};
        bool truePeak{true};
    };

    LimiterDsp();

    [[nodiscard]] QString name() const override;
    [[nodiscard]] QString id() const override;
    [[nodiscard]] bool supportsLiveSettings() const override;

    [[nodiscard]] AudioFormat outputFormat(const AudioFormat& input) const override;
    void prepare(const AudioFormat& format) override;
    void process(ProcessingBufferList& chunks) override;
    void reset() override;
    void flush(ProcessingBufferList& chunks, FlushMode mode) override;
    [[nodiscard]] int latencyFrames() const override;

    [[nodiscard]] QByteArray saveSettings() const override;
    bool loadSettings(const QByteArray& preset) override;

    void setSettings(const Settings& settings);
    [[nodiscard]] Settings settings() const;

    [[nodiscard]] float lastGainReductionDb() const
    {
        return m_lastGainReductionDb.load(std::memory_order_relaxed);
    }

    static Settings defaultSettings();

    static double clampCeiling(double value);
    static double clampInputGain(double value);
    static double clampAttack(double value);
    static double clampRelease(double value);
    static double clampLookahead(double value);
    static double clampKnee(double value);

    static float globalGainReductionDb();

private:
    struct GainPoint
    {
        uint64_t frame{0};
        double gainDb{0.0};
    };

    static Settings sanitiseSettings(Settings settings);
    static bool settingsEqual(const Settings& lhs, const Settings& rhs);
    static bool requiresRuntimeResetForSettingsChange(const Settings& lhs, const Settings& rhs);
    static double dbToLinear(double db);

    void publishSettings(const Settings& settings);
    bool applySettingsIfNeeded();
    void rebuildCoefficients();
    void resizeDelayBuffer();
    void ensurePreparedFor(const AudioFormat& format);

    void resetRuntimeState();
    void resetDelayState();
    void resetGainQueue();
    void resetTruePeakState();
    void resizeTruePeakHistory();

    void expireGainTargets(uint64_t firstValidFrame);
    void pushGainTarget(uint64_t frame, double gainDb);
    [[nodiscard]] double currentLookaheadGainTargetDb() const;

    [[nodiscard]] double gainComputer(double levelDb) const;
    [[nodiscard]] double peakDetect(const double* frame, int channels) const;
    [[nodiscard]] double truePeakDetect(const double* frame, int channels);

    void processBuffer(const ProcessingBuffer& buffer, ProcessingBufferList& output);

    [[nodiscard]] double detectFrameTargetGainDb(const double* frame, int channels);
    float appendProcessedFrame(const double* frame, int channels, double targetGainDb);
    float appendDelayedFrame(int channels);
    void pushDelayFrame(const double* frame, int channels);

    void flushDelayTail(ProcessingBufferList& output, uint64_t sourceFrameDurationNs);

    void emitOutputChunk(ProcessingBufferList& output, int frames, uint64_t sourceFrameDurationNs);
    void advanceOutputCursor(int frames, uint64_t sourceFrameDurationNs);

    AudioFormat m_format;

    std::vector<double> m_delayBuffer;
    int m_delayReadPos{0};
    int m_delayWritePos{0};
    int m_delayFill{0};

    std::vector<GainPoint> m_gainQueue;
    size_t m_gainQueueStart{0};
    size_t m_gainQueueCount{0};

    uint64_t m_nextInputFrame{0};
    uint64_t m_nextOutputFrame{0};

    double m_gainStateDb{0.0};
    double m_attackCoeff{0.0};
    double m_releaseCoeff{0.0};

    double m_ceilingLinear{1.0};
    double m_inputGainLinear{1.0};
    int m_lookaheadFrames{0};

    std::vector<double> m_truePeakHistory;
    int m_truePeakHistoryPos{0};

    std::vector<double> m_outputScratch;

    bool m_hasOutputCursor{false};
    uint64_t m_outputCursorNs{0};
    double m_outputCursorRemainderNs{0.0};
    uint64_t m_outputSourceFrameDurationNs{0};

    std::atomic<float> m_lastGainReductionDb{0.0F};

    Settings m_settings;
    Settings m_appliedSettings;
    bool m_hasAppliedSettings{false};
    bool m_settingsDirty{true};
};
} // namespace Fooyin::Limiter
