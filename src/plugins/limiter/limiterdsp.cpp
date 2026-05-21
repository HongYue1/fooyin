#include "limiterdsp.h"

#include <utils/timeconstants.h>

#include <QDataStream>
#include <QIODevice>

#include <algorithm>
#include <array>
#include <cmath>

constexpr quint32 PresetVersion = 2;

constexpr auto CeilingMinDb   = -20.0;
constexpr auto CeilingMaxDb   = 0.0;
constexpr auto InputGainMinDb = -20.0;
constexpr auto InputGainMaxDb = 20.0;
constexpr auto AttackMinMs    = 0.0;
constexpr auto AttackMaxMs    = 50.0;
constexpr auto ReleaseMinMs   = 1.0;
constexpr auto ReleaseMaxMs   = 2000.0;
constexpr auto LookaheadMinMs = 0.0;
constexpr auto LookaheadMaxMs = 50.0;
constexpr auto KneeMinDb      = 0.0;
constexpr auto KneeMaxDb      = 12.0;

constexpr auto SettingsEpsilon = 1.0e-9;
constexpr auto SilenceFloor    = 1.0e-12;

constexpr int TruePeakOversample = 4;
constexpr int TruePeakPhases     = TruePeakOversample - 1;
constexpr int TruePeakTaps       = 32;
constexpr int TruePeakGroupDelay = (TruePeakTaps / 2) - 1;

namespace {
std::atomic<float> s_globalGainReductionDb{0.0F};

uint64_t nominalFrameDurationNs(const int sampleRate)
{
    const int safeRate   = std::max(1, sampleRate);
    const double frameNs = static_cast<double>(Fooyin::Time::NsPerSecond) / static_cast<double>(safeRate);
    return static_cast<uint64_t>(std::max<int64_t>(1, std::llround(frameNs)));
}

double timeCoefficient(const double timeMs, const int sampleRate)
{
    if(timeMs <= 0.0 || sampleRate <= 0) {
        return 0.0;
    }

    return std::exp(-1.0 / (timeMs * 0.001 * static_cast<double>(sampleRate)));
}

int lookaheadFramesFor(const double lookaheadMs, const int sampleRate)
{
    if(lookaheadMs <= 0.0 || sampleRate <= 0) {
        return 0;
    }

    return static_cast<int>(std::ceil(lookaheadMs * 0.001 * static_cast<double>(sampleRate)));
}

int effectiveLookaheadFramesFor(const Fooyin::Limiter::LimiterDsp::Settings& settings, const int sampleRate)
{
    int frames = lookaheadFramesFor(settings.lookaheadMs, sampleRate);

    // The fractional-delay true-peak FIR reports reconstructed peaks after its group delay.
    // Keep at least that much lookahead so true-peak detections can still affect
    // the corresponding delayed output frame even when the UI lookahead is set very low.
    if(settings.truePeak && sampleRate > 0) {
        frames = std::max(frames, TruePeakGroupDelay);
    }

    return frames;
}

double finiteOr(const double value, const double fallback)
{
    return std::isfinite(value) ? value : fallback;
}

double finiteSampleOrZero(const double value)
{
    return std::isfinite(value) ? value : 0.0;
}

double sinc(const double value)
{
    static constexpr double Pi = 3.14159265358979323846264338327950288;

    if(std::abs(value) < 1.0e-12) {
        return 1.0;
    }

    const double x = Pi * value;
    return std::sin(x) / x;
}

std::array<std::array<double, TruePeakTaps>, TruePeakPhases> makeTruePeakCoefficients()
{
    static constexpr double Pi = 3.14159265358979323846264338327950288;

    std::array<std::array<double, TruePeakTaps>, TruePeakPhases> coefficients{};

    for(int phase{0}; phase < TruePeakPhases; ++phase) {
        const double fractionalDelay = static_cast<double>(phase + 1) / static_cast<double>(TruePeakOversample);
        const double delay           = static_cast<double>(TruePeakGroupDelay) + fractionalDelay;

        double sum{0.0};

        for(int tap{0}; tap < TruePeakTaps; ++tap) {
            const double window
                = 0.5 - (0.5 * std::cos((2.0 * Pi * static_cast<double>(tap)) / static_cast<double>(TruePeakTaps - 1)));

            const double value = sinc(static_cast<double>(tap) - delay) * window;
            coefficients[static_cast<size_t>(phase)][static_cast<size_t>(tap)] = value;
            sum += value;
        }

        if(std::abs(sum) > 1.0e-12) {
            for(double& value : coefficients[static_cast<size_t>(phase)]) {
                value /= sum;
            }
        }
    }

    return coefficients;
}

const std::array<std::array<double, TruePeakTaps>, TruePeakPhases>& truePeakCoefficients()
{
    static const auto coefficients = makeTruePeakCoefficients();
    return coefficients;
}
} // namespace

namespace Fooyin::Limiter {
LimiterDsp::LimiterDsp()
    : m_settings{defaultSettings()}
    , m_appliedSettings{defaultSettings()}
{ }

QString LimiterDsp::name() const
{
    return QStringLiteral("Limiter");
}

QString LimiterDsp::id() const
{
    return QStringLiteral("fooyin.dsp.limiter");
}

bool LimiterDsp::supportsLiveSettings() const
{
    return true;
}

AudioFormat LimiterDsp::outputFormat(const AudioFormat& input) const
{
    return input;
}

void LimiterDsp::prepare(const AudioFormat& format)
{
    m_format = format;

    if(!m_format.isValid()) {
        reset();
        return;
    }

    m_hasAppliedSettings = false;
    m_settingsDirty      = true;

    applySettingsIfNeeded();
    resetRuntimeState();
}

void LimiterDsp::process(ProcessingBufferList& chunks)
{
    if(applySettingsIfNeeded()) {
        return;
    }

    ProcessingBufferList output;
    output.clear();

    const size_t count = chunks.count();
    for(size_t i{0}; i < count; ++i) {
        const auto* buffer = chunks.item(i);
        if(!buffer || !buffer->isValid() || buffer->frameCount() <= 0) {
            continue;
        }

        ensurePreparedFor(buffer->format());
        if(!m_format.isValid()) {
            continue;
        }

        if(!m_hasOutputCursor) {
            m_outputCursorNs          = buffer->startTimeNs();
            m_outputCursorRemainderNs = 0.0;
            m_hasOutputCursor         = true;
        }

        m_outputSourceFrameDurationNs = buffer->sourceFrameDurationNs() > 0
                                          ? buffer->sourceFrameDurationNs()
                                          : nominalFrameDurationNs(m_format.sampleRate());

        processBuffer(*buffer, output);
    }

    chunks.clear();

    const size_t outCount = output.count();
    for(size_t i{0}; i < outCount; ++i) {
        const auto* buffer = output.item(i);
        if(buffer && buffer->isValid()) {
            chunks.addChunk(*buffer);
        }
    }
}

void LimiterDsp::reset()
{
    resetRuntimeState();
    m_lastGainReductionDb.store(0.0F, std::memory_order_relaxed);
    s_globalGainReductionDb.store(0.0F, std::memory_order_relaxed);
}

void LimiterDsp::flush(ProcessingBufferList& chunks, const FlushMode mode)
{
    if(mode == FlushMode::Flush) {
        reset();
        return;
    }

    if(applySettingsIfNeeded()) {
        reset();
        return;
    }

    if(!m_format.isValid() || m_lookaheadFrames <= 0 || m_delayFill <= 0) {
        reset();
        return;
    }

    if(!m_hasOutputCursor) {
        m_hasOutputCursor         = true;
        m_outputCursorNs          = 0;
        m_outputCursorRemainderNs = 0.0;
    }

    const uint64_t sourceFrameDurationNs = m_outputSourceFrameDurationNs > 0
                                             ? m_outputSourceFrameDurationNs
                                             : nominalFrameDurationNs(m_format.sampleRate());

    flushDelayTail(chunks, sourceFrameDurationNs);
    reset();
}

int LimiterDsp::latencyFrames() const
{
    if(!m_settings.enabled) {
        return 0;
    }

    const int sampleRate = m_format.isValid() ? m_format.sampleRate() : 44100;
    return effectiveLookaheadFramesFor(m_settings, sampleRate);
}

QByteArray LimiterDsp::saveSettings() const
{
    QByteArray data;
    QDataStream stream{&data, QIODevice::WriteOnly};
    stream.setVersion(QDataStream::Qt_6_0);

    const Settings s = settings();

    stream << quint32(PresetVersion);
    stream << s.enabled;
    stream << s.ceilingDb;
    stream << s.inputGainDb;
    stream << s.attackMs;
    stream << s.releaseMs;
    stream << s.lookaheadMs;
    stream << s.kneeDb;
    stream << s.truePeak;

    return data;
}

bool LimiterDsp::loadSettings(const QByteArray& preset)
{
    if(preset.isEmpty()) {
        return false;
    }

    QDataStream stream{preset};
    stream.setVersion(QDataStream::Qt_6_0);

    quint32 version{0};
    Settings s = defaultSettings();

    stream >> version;
    if(stream.status() != QDataStream::Ok) {
        return false;
    }

    if(version != 1 && version != PresetVersion) {
        return false;
    }

    if(version >= 2) {
        stream >> s.enabled;
    }
    else {
        s.enabled = true;
    }

    stream >> s.ceilingDb;
    stream >> s.inputGainDb;
    stream >> s.attackMs;
    stream >> s.releaseMs;
    stream >> s.lookaheadMs;
    stream >> s.kneeDb;
    stream >> s.truePeak;

    if(stream.status() != QDataStream::Ok) {
        return false;
    }

    setSettings(s);
    return true;
}

void LimiterDsp::setSettings(const Settings& settings)
{
    publishSettings(sanitiseSettings(settings));
}

LimiterDsp::Settings LimiterDsp::settings() const
{
    return m_settings;
}

LimiterDsp::Settings LimiterDsp::defaultSettings()
{
    return {};
}

float LimiterDsp::globalGainReductionDb()
{
    return s_globalGainReductionDb.load(std::memory_order_relaxed);
}

double LimiterDsp::clampCeiling(const double value)
{
    return std::clamp(value, CeilingMinDb, CeilingMaxDb);
}

double LimiterDsp::clampInputGain(const double value)
{
    return std::clamp(value, InputGainMinDb, InputGainMaxDb);
}

double LimiterDsp::clampAttack(const double value)
{
    return std::clamp(value, AttackMinMs, AttackMaxMs);
}

double LimiterDsp::clampRelease(const double value)
{
    return std::clamp(value, ReleaseMinMs, ReleaseMaxMs);
}

double LimiterDsp::clampLookahead(const double value)
{
    return std::clamp(value, LookaheadMinMs, LookaheadMaxMs);
}

double LimiterDsp::clampKnee(const double value)
{
    return std::clamp(value, KneeMinDb, KneeMaxDb);
}

LimiterDsp::Settings LimiterDsp::sanitiseSettings(Settings settings)
{
    const Settings fallback = defaultSettings();

    settings.ceilingDb   = clampCeiling(finiteOr(settings.ceilingDb, fallback.ceilingDb));
    settings.inputGainDb = clampInputGain(finiteOr(settings.inputGainDb, fallback.inputGainDb));
    settings.attackMs    = clampAttack(finiteOr(settings.attackMs, fallback.attackMs));
    settings.releaseMs   = clampRelease(finiteOr(settings.releaseMs, fallback.releaseMs));
    settings.lookaheadMs = clampLookahead(finiteOr(settings.lookaheadMs, fallback.lookaheadMs));
    settings.kneeDb      = clampKnee(finiteOr(settings.kneeDb, fallback.kneeDb));

    return settings;
}

bool LimiterDsp::settingsEqual(const Settings& lhs, const Settings& rhs)
{
    return lhs.enabled == rhs.enabled && std::abs(lhs.ceilingDb - rhs.ceilingDb) <= SettingsEpsilon
        && std::abs(lhs.inputGainDb - rhs.inputGainDb) <= SettingsEpsilon
        && std::abs(lhs.attackMs - rhs.attackMs) <= SettingsEpsilon
        && std::abs(lhs.releaseMs - rhs.releaseMs) <= SettingsEpsilon
        && std::abs(lhs.lookaheadMs - rhs.lookaheadMs) <= SettingsEpsilon
        && std::abs(lhs.kneeDb - rhs.kneeDb) <= SettingsEpsilon && lhs.truePeak == rhs.truePeak;
}

bool LimiterDsp::requiresRuntimeResetForSettingsChange(const Settings& lhs, const Settings& rhs)
{
    if(lhs.enabled != rhs.enabled) {
        return true;
    }

    if(!rhs.enabled) {
        return false;
    }

    return std::abs(lhs.ceilingDb - rhs.ceilingDb) > SettingsEpsilon
        || std::abs(lhs.inputGainDb - rhs.inputGainDb) > SettingsEpsilon
        || std::abs(lhs.lookaheadMs - rhs.lookaheadMs) > SettingsEpsilon
        || std::abs(lhs.kneeDb - rhs.kneeDb) > SettingsEpsilon || lhs.truePeak != rhs.truePeak;
}

double LimiterDsp::dbToLinear(const double db)
{
    return std::pow(10.0, db / 20.0);
}

void LimiterDsp::publishSettings(const Settings& settings)
{
    if(settingsEqual(settings, m_settings)) {
        return;
    }

    m_settings      = settings;
    m_settingsDirty = true;
}

bool LimiterDsp::applySettingsIfNeeded()
{
    if(!m_settingsDirty && m_hasAppliedSettings) {
        return !m_appliedSettings.enabled;
    }

    const Settings previousSettings = m_appliedSettings;
    const bool hadAppliedSettings   = m_hasAppliedSettings;
    const bool wasEnabled           = !m_hasAppliedSettings || m_appliedSettings.enabled;

    m_appliedSettings    = m_settings;
    m_hasAppliedSettings = true;
    m_settingsDirty      = false;

    if(!m_appliedSettings.enabled) {
        if(wasEnabled) {
            resetRuntimeState();
        }

        m_lastGainReductionDb.store(0.0F, std::memory_order_relaxed);
        s_globalGainReductionDb.store(0.0F, std::memory_order_relaxed);

        return true;
    }

    const bool needsRuntimeReset
        = hadAppliedSettings && requiresRuntimeResetForSettingsChange(previousSettings, m_appliedSettings);

    rebuildCoefficients();

    if(m_appliedSettings.truePeak && m_format.isValid()) {
        resizeTruePeakHistory();
    }
    else {
        m_truePeakHistory.clear();
        m_truePeakHistoryPos = 0;
    }

    if(needsRuntimeReset) {
        resetRuntimeState();
    }

    return false;
}

void LimiterDsp::rebuildCoefficients()
{
    const int sampleRate = m_format.isValid() ? m_format.sampleRate() : 44100;
    const int channels   = m_format.isValid() ? std::max(1, m_format.channelCount()) : 1;

    m_attackCoeff     = timeCoefficient(m_appliedSettings.attackMs, sampleRate);
    m_releaseCoeff    = timeCoefficient(m_appliedSettings.releaseMs, sampleRate);
    m_ceilingLinear   = dbToLinear(m_appliedSettings.ceilingDb);
    m_inputGainLinear = dbToLinear(m_appliedSettings.inputGainDb);

    const int newLookaheadFrames = effectiveLookaheadFramesFor(m_appliedSettings, sampleRate);
    const size_t requiredDelaySamples
        = newLookaheadFrames > 0 ? static_cast<size_t>(newLookaheadFrames) * static_cast<size_t>(channels) : 0U;

    if(newLookaheadFrames != m_lookaheadFrames || m_delayBuffer.size() != requiredDelaySamples) {
        m_lookaheadFrames = newLookaheadFrames;
        resizeDelayBuffer();
    }
}

void LimiterDsp::resizeDelayBuffer()
{
    const int channels = m_format.isValid() ? std::max(1, m_format.channelCount()) : 1;

    if(m_lookaheadFrames > 0) {
        m_delayBuffer.assign(static_cast<size_t>(m_lookaheadFrames) * static_cast<size_t>(channels), 0.0);
    }
    else {
        m_delayBuffer.clear();
    }

    resetDelayState();
    resetGainQueue();
}

void LimiterDsp::ensurePreparedFor(const AudioFormat& format)
{
    if(!m_format.isValid() || m_format.sampleRate() != format.sampleRate()
       || m_format.channelCount() != format.channelCount()) {
        prepare(format);
    }
}

void LimiterDsp::resetRuntimeState()
{
    resetDelayState();
    resetGainQueue();
    resetTruePeakState();

    m_nextInputFrame  = 0;
    m_nextOutputFrame = 0;

    m_gainStateDb = 0.0;

    if(!m_delayBuffer.empty()) {
        std::fill(m_delayBuffer.begin(), m_delayBuffer.end(), 0.0);
    }

    m_outputScratch.clear();

    m_hasOutputCursor             = false;
    m_outputCursorNs              = 0;
    m_outputCursorRemainderNs     = 0.0;
    m_outputSourceFrameDurationNs = 0;
}

void LimiterDsp::resetDelayState()
{
    m_delayReadPos  = 0;
    m_delayWritePos = 0;
    m_delayFill     = 0;
}

void LimiterDsp::resetGainQueue()
{
    const size_t capacity = static_cast<size_t>(std::max(0, m_lookaheadFrames)) + 1U;

    m_gainQueue.clear();
    m_gainQueue.resize(std::max<size_t>(1U, capacity));

    m_gainQueueStart = 0;
    m_gainQueueCount = 0;
}

void LimiterDsp::resizeTruePeakHistory()
{
    const int channels           = m_format.isValid() ? std::max(1, m_format.channelCount()) : 1;
    const size_t requiredSamples = static_cast<size_t>(channels) * static_cast<size_t>(TruePeakTaps);

    if(m_truePeakHistory.size() != requiredSamples) {
        m_truePeakHistory.assign(requiredSamples, 0.0);
        m_truePeakHistoryPos = 0;
    }
}

void LimiterDsp::resetTruePeakState()
{
    if(!m_truePeakHistory.empty()) {
        std::fill(m_truePeakHistory.begin(), m_truePeakHistory.end(), 0.0);
    }

    m_truePeakHistoryPos = 0;
}

void LimiterDsp::expireGainTargets(const uint64_t firstValidFrame)
{
    if(m_gainQueue.empty()) {
        return;
    }

    const size_t capacity = m_gainQueue.size();

    while(m_gainQueueCount > 0) {
        const GainPoint& front = m_gainQueue[m_gainQueueStart];
        if(front.frame >= firstValidFrame) {
            break;
        }

        m_gainQueueStart = (m_gainQueueStart + 1U) % capacity;
        --m_gainQueueCount;
    }
}

void LimiterDsp::pushGainTarget(const uint64_t frame, const double gainDb)
{
    if(m_gainQueue.empty()) {
        resetGainQueue();
    }

    const size_t capacity = m_gainQueue.size();

    while(m_gainQueueCount > 0) {
        const size_t backIndex = (m_gainQueueStart + m_gainQueueCount - 1U) % capacity;
        if(m_gainQueue[backIndex].gainDb <= gainDb) {
            break;
        }

        --m_gainQueueCount;
    }

    if(m_gainQueueCount >= capacity) {
        m_gainQueueStart = (m_gainQueueStart + 1U) % capacity;
        --m_gainQueueCount;
    }

    const size_t writeIndex = (m_gainQueueStart + m_gainQueueCount) % capacity;
    m_gainQueue[writeIndex] = {frame, gainDb};
    ++m_gainQueueCount;
}

double LimiterDsp::currentLookaheadGainTargetDb() const
{
    if(m_gainQueue.empty() || m_gainQueueCount == 0) {
        return 0.0;
    }

    return m_gainQueue[m_gainQueueStart].gainDb;
}

double LimiterDsp::gainComputer(const double levelDb) const
{
    const double ceiling   = m_appliedSettings.ceilingDb;
    const double knee      = m_appliedSettings.kneeDb;
    const double overshoot = levelDb - ceiling;

    if(knee <= 0.0) {
        return overshoot > 0.0 ? -overshoot : 0.0;
    }

    const double halfKnee = knee * 0.5;

    if(levelDb <= ceiling - halfKnee) {
        return 0.0;
    }

    if(levelDb >= ceiling + halfKnee) {
        return -overshoot;
    }

    const double x = levelDb - ceiling + halfKnee;
    return -(x * x) / (2.0 * knee);
}

double LimiterDsp::peakDetect(const double* frame, const int channels) const
{
    double peak{0.0};

    for(int channel{0}; channel < channels; ++channel) {
        peak = std::max(peak, std::abs(finiteSampleOrZero(frame[channel])));
    }

    return peak;
}

double LimiterDsp::truePeakDetect(const double* frame, const int channels)
{
    if(channels <= 0) {
        return 0.0;
    }

    if(m_truePeakHistory.size() != static_cast<size_t>(channels) * static_cast<size_t>(TruePeakTaps)) {
        resizeTruePeakHistory();
    }

    double peak = peakDetect(frame, channels);

    for(int channel{0}; channel < channels; ++channel) {
        const size_t base = static_cast<size_t>(channel) * static_cast<size_t>(TruePeakTaps);
        m_truePeakHistory[base + static_cast<size_t>(m_truePeakHistoryPos)] = finiteSampleOrZero(frame[channel]);
    }

    const auto& coefficients = truePeakCoefficients();

    for(int channel{0}; channel < channels; ++channel) {
        const size_t base = static_cast<size_t>(channel) * static_cast<size_t>(TruePeakTaps);

        for(int phase{0}; phase < TruePeakPhases; ++phase) {
            double reconstructed{0.0};

            for(int tap{0}; tap < TruePeakTaps; ++tap) {
                const int historyIndex = (m_truePeakHistoryPos - tap + TruePeakTaps) % TruePeakTaps;
                const double sample    = m_truePeakHistory[base + static_cast<size_t>(historyIndex)];
                reconstructed += coefficients[static_cast<size_t>(phase)][static_cast<size_t>(tap)] * sample;
            }

            peak = std::max(peak, std::abs(reconstructed));
        }
    }

    m_truePeakHistoryPos = (m_truePeakHistoryPos + 1) % TruePeakTaps;

    return peak;
}

double LimiterDsp::detectFrameTargetGainDb(const double* frame, const int channels)
{
    const double peak = m_appliedSettings.truePeak ? truePeakDetect(frame, channels) : peakDetect(frame, channels);

    const double levelDb = peak > SilenceFloor ? 20.0 * std::log10(peak) + m_appliedSettings.inputGainDb : -200.0;

    return gainComputer(levelDb);
}

void LimiterDsp::processBuffer(const ProcessingBuffer& buffer, ProcessingBufferList& output)
{
    const int channels   = std::max(1, m_format.channelCount());
    const int frameCount = buffer.frameCount();
    const auto inputData = buffer.constData();

    if(frameCount <= 0 || inputData.empty()) {
        return;
    }

    const size_t availableFrames = inputData.size() / static_cast<size_t>(channels);
    const int framesToProcess    = std::min(frameCount, static_cast<int>(availableFrames));
    if(framesToProcess <= 0) {
        return;
    }

    m_outputScratch.clear();
    m_outputScratch.reserve(static_cast<size_t>(framesToProcess) * static_cast<size_t>(channels));

    std::vector<double> frameScratch(static_cast<size_t>(channels), 0.0);

    int producedFrames{0};
    float blockGainReductionDb{0.0F};

    for(int frameIndex{0}; frameIndex < framesToProcess; ++frameIndex) {
        const double* inputFrame = inputData.data() + (static_cast<size_t>(frameIndex) * static_cast<size_t>(channels));

        for(int channel{0}; channel < channels; ++channel) {
            frameScratch[static_cast<size_t>(channel)] = finiteSampleOrZero(inputFrame[channel]);
        }

        const double* frame = frameScratch.data();

        const uint64_t detectorFrame = m_nextInputFrame++;
        const double targetGainDb    = detectFrameTargetGainDb(frame, channels);

        expireGainTargets(m_nextOutputFrame);
        pushGainTarget(detectorFrame, targetGainDb);

        if(m_lookaheadFrames > 0) {
            if(m_delayFill >= m_lookaheadFrames) {
                const float gr       = appendDelayedFrame(channels);
                blockGainReductionDb = std::min(blockGainReductionDb, gr);
                ++producedFrames;
            }

            pushDelayFrame(frame, channels);
        }
        else {
            const double outputTargetGainDb = currentLookaheadGainTargetDb();
            const float gr                  = appendProcessedFrame(frame, channels, outputTargetGainDb);
            blockGainReductionDb            = std::min(blockGainReductionDb, gr);
            ++producedFrames;
        }
    }

    m_lastGainReductionDb.store(blockGainReductionDb, std::memory_order_relaxed);
    s_globalGainReductionDb.store(blockGainReductionDb, std::memory_order_relaxed);

    if(producedFrames > 0) {
        emitOutputChunk(output, producedFrames, m_outputSourceFrameDurationNs);
    }
}

float LimiterDsp::appendProcessedFrame(const double* frame, const int channels, const double targetGainDb)
{
    if(targetGainDb < m_gainStateDb) {
        m_gainStateDb = (m_attackCoeff * m_gainStateDb) + ((1.0 - m_attackCoeff) * targetGainDb);
    }
    else {
        m_gainStateDb = (m_releaseCoeff * m_gainStateDb) + ((1.0 - m_releaseCoeff) * targetGainDb);
    }

    m_gainStateDb = std::min(m_gainStateDb, 0.0);

    if(std::abs(m_gainStateDb) < 1.0e-12) {
        m_gainStateDb = 0.0;
    }

    const double totalGain = m_inputGainLinear * dbToLinear(m_gainStateDb);

    for(int channel{0}; channel < channels; ++channel) {
        // Final sample-peak safety clamp. Normal limiter operation should avoid
        // relying on this; it is retained to prevent invalid overs from escaping.
        const double processed = finiteSampleOrZero(frame[channel]) * totalGain;
        const double sample    = std::clamp(processed, -m_ceilingLinear, m_ceilingLinear);
        m_outputScratch.push_back(sample);
    }

    ++m_nextOutputFrame;
    expireGainTargets(m_nextOutputFrame);

    return static_cast<float>(m_gainStateDb);
}

float LimiterDsp::appendDelayedFrame(const int channels)
{
    if(m_delayFill <= 0 || m_delayBuffer.empty()) {
        return 0.0F;
    }

    const double* frame = m_delayBuffer.data() + (static_cast<size_t>(m_delayReadPos) * static_cast<size_t>(channels));

    const double targetGainDb = currentLookaheadGainTargetDb();
    const float gr            = appendProcessedFrame(frame, channels, targetGainDb);

    m_delayReadPos = (m_delayReadPos + 1) % std::max(1, m_lookaheadFrames);
    --m_delayFill;

    return gr;
}

void LimiterDsp::pushDelayFrame(const double* frame, const int channels)
{
    if(m_lookaheadFrames <= 0 || m_delayBuffer.empty()) {
        return;
    }

    if(m_delayFill >= m_lookaheadFrames) {
        m_delayReadPos = (m_delayReadPos + 1) % m_lookaheadFrames;
        --m_delayFill;
    }

    double* slot = m_delayBuffer.data() + (static_cast<size_t>(m_delayWritePos) * static_cast<size_t>(channels));

    for(int channel{0}; channel < channels; ++channel) {
        slot[channel] = finiteSampleOrZero(frame[channel]);
    }

    m_delayWritePos = (m_delayWritePos + 1) % m_lookaheadFrames;
    ++m_delayFill;
}

void LimiterDsp::flushDelayTail(ProcessingBufferList& output, const uint64_t sourceFrameDurationNs)
{
    if(m_delayFill <= 0 || !m_format.isValid()) {
        return;
    }

    const int channels      = std::max(1, m_format.channelCount());
    const int framesToFlush = m_delayFill;

    m_outputScratch.clear();
    m_outputScratch.reserve(static_cast<size_t>(framesToFlush) * static_cast<size_t>(channels));

    float blockGainReductionDb{0.0F};

    while(m_delayFill > 0) {
        expireGainTargets(m_nextOutputFrame);
        const float gr       = appendDelayedFrame(channels);
        blockGainReductionDb = std::min(blockGainReductionDb, gr);
    }

    m_lastGainReductionDb.store(blockGainReductionDb, std::memory_order_relaxed);
    s_globalGainReductionDb.store(blockGainReductionDb, std::memory_order_relaxed);

    emitOutputChunk(output, framesToFlush, sourceFrameDurationNs);
}

void LimiterDsp::emitOutputChunk(ProcessingBufferList& output, const int frames, const uint64_t sourceFrameDurationNs)
{
    if(frames <= 0 || !m_format.isValid()) {
        return;
    }

    const int channels       = std::max(1, m_format.channelCount());
    const size_t sampleCount = static_cast<size_t>(frames) * static_cast<size_t>(channels);

    ProcessingBuffer out{m_format, m_outputCursorNs};
    out.resizeSamples(sampleCount);

    auto outData           = out.data();
    const size_t copyCount = std::min(outData.size(), m_outputScratch.size());

    for(size_t i{0}; i < copyCount; ++i) {
        outData[i] = m_outputScratch[i];
    }

    out.setSourceFrameDurationNs(sourceFrameDurationNs);
    output.addChunk(out);

    advanceOutputCursor(frames, sourceFrameDurationNs);
}

void LimiterDsp::advanceOutputCursor(const int frames, const uint64_t sourceFrameDurationNs)
{
    if(frames <= 0 || sourceFrameDurationNs == 0) {
        return;
    }

    const double advanceNsFull
        = (static_cast<double>(frames) * static_cast<double>(sourceFrameDurationNs)) + m_outputCursorRemainderNs;

    const auto advanceNs = static_cast<uint64_t>(std::max(0.0, std::floor(advanceNsFull)));

    m_outputCursorRemainderNs = advanceNsFull - static_cast<double>(advanceNs);
    m_outputCursorNs += advanceNs;
}
} // namespace Fooyin::Limiter
