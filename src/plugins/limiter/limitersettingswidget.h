#pragma once

#include "limiterdsp.h"

#include <gui/dsp/dspsettingsprovider.h>

#include <QBasicTimer>

class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QTimerEvent;

namespace Fooyin::Limiter {
class GrMeterWidget;

class LimiterSettingsWidget : public DspSettingsDialog
{
    Q_OBJECT

public:
    explicit LimiterSettingsWidget(QWidget* parent = nullptr);

    void loadSettings(const QByteArray& settings) override;
    [[nodiscard]] QByteArray saveSettings() const override;

protected:
    void restoreDefaults() override;
    void timerEvent(QTimerEvent* event) override;

private:
    static constexpr int PreviewDebounceMs = 16;
    static constexpr int MeterPollMs       = 50;

    void connectSignals();
    void applyToUi(const LimiterDsp::Settings& settings);
    [[nodiscard]] LimiterDsp::Settings readFromUi() const;

    void schedulePreview();
    void updateGainReductionMeter();

    QCheckBox* m_enableCheck;

    QDoubleSpinBox* m_ceilingSpin;
    QDoubleSpinBox* m_inputGainSpin;
    QDoubleSpinBox* m_attackSpin;
    QDoubleSpinBox* m_releaseSpin;
    QDoubleSpinBox* m_lookaheadSpin;
    QDoubleSpinBox* m_kneeSpin;
    QCheckBox* m_truePeakCheck;

    GrMeterWidget* m_gainReductionMeter;
    QLabel* m_gainReductionCurrentLabel;
    QLabel* m_gainReductionMaxLabel;

    QBasicTimer m_previewTimer;
    QBasicTimer m_meterPollTimer;
};

class LimiterSettingsProvider : public DspSettingsProvider
{
public:
    [[nodiscard]] QString id() const override;
    [[nodiscard]] QString displayName() const override;
    [[nodiscard]] QString viewMenuText() const override;
    [[nodiscard]] QString viewMenuStatusTip() const override;

    [[nodiscard]] bool showInViewMenu() const override;
    [[nodiscard]] bool showAsLayoutWidget() const override;

    DspSettingsDialog* createSettingsWidget(QWidget* parent) override;
};
} // namespace Fooyin::Limiter
