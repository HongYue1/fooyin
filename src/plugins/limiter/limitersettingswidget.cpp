#include "limitersettingswidget.h"

#include <QCheckBox>
#include <QCoreApplication>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QGradient>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimerEvent>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

namespace {
QString formatGainReduction(float value)
{
    value = std::clamp(value, -20.0F, 0.0F);

    if(std::abs(value) < 0.05F) {
        value = 0.0F;
    }

    return QString::number(value, 'f', 1) + QStringLiteral(" dB");
}

QDoubleSpinBox* createSpinBox(double min, double max, double step, int decimals, const QString& suffix, QWidget* parent)
{
    auto* spin = new QDoubleSpinBox(parent);
    spin->setRange(min, max);
    spin->setSingleStep(step);
    spin->setDecimals(decimals);
    spin->setSuffix(suffix);
    spin->setAlignment(Qt::AlignRight);
    return spin;
}

QFrame* createSeparator(QWidget* parent)
{
    auto* separator = new QFrame(parent);
    separator->setFrameShape(QFrame::HLine);
    separator->setFrameShadow(QFrame::Sunken);
    return separator;
}
} // namespace

namespace Fooyin::Limiter {
class GrMeterWidget : public QWidget
{
    Q_OBJECT

public:
    explicit GrMeterWidget(QWidget* parent = nullptr)
        : QWidget{parent}
    {
        setFixedSize(96, 190);
        setToolTip(tr("Gain reduction"));
    }

    void setValue(float gainReductionDb)
    {
        gainReductionDb = std::clamp(gainReductionDb, -20.0F, 0.0F);

        const float smoothing = gainReductionDb < m_displayDb ? 0.65F : 0.20F;
        m_displayDb += (gainReductionDb - m_displayDb) * smoothing;
        m_valueDb = gainReductionDb;

        if(gainReductionDb < m_holdDb) {
            m_holdDb      = gainReductionDb;
            m_holdCounter = HoldFrames;
        }
        else if(m_holdCounter > 0) {
            --m_holdCounter;
        }
        else {
            m_holdDb = std::min(0.0F, m_holdDb + 0.15F);
        }

        if(gainReductionDb < m_maxDb) {
            m_maxDb = gainReductionDb;
        }

        if(std::abs(m_displayDb) < 0.03F) {
            m_displayDb = 0.0F;
        }

        update();
    }

    void clearMaximum()
    {
        m_holdDb      = 0.0F;
        m_maxDb       = 0.0F;
        m_holdCounter = 0;
        update();
    }

    [[nodiscard]] float valueDb() const
    {
        return m_valueDb;
    }

    [[nodiscard]] float maxDb() const
    {
        return m_maxDb;
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter{this};
        painter.setRenderHint(QPainter::Antialiasing, true);

        const QRect outer = rect().adjusted(2, 2, -2, -2);
        const QRect scaleRect{outer.left(), outer.top(), 34, outer.height()};
        const QRect barRect{scaleRect.right() + 6, outer.top() + 4, 24, outer.height() - 8};
        const QRect textRect{barRect.right() + 6, outer.top(), outer.right() - barRect.right() - 6, outer.height()};

        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor{24, 24, 24});
        painter.drawRoundedRect(barRect.adjusted(-1, -1, 1, 1), 3, 3);

        const auto yForDb = [&barRect](float db) {
            db                   = std::clamp(db, -20.0F, 0.0F);
            const float fraction = std::clamp(-db / 20.0F, 0.0F, 1.0F);
            return barRect.top() + static_cast<int>(std::round(fraction * static_cast<float>(barRect.height())));
        };

        painter.setRenderHint(QPainter::Antialiasing, false);

        const QFontMetrics metrics{font()};

        struct Tick
        {
            float db;
            const char* label;
        };

        static constexpr Tick Ticks[] = {
            {0.0F, "0"}, {-3.0F, "-3"}, {-6.0F, "-6"}, {-12.0F, "-12"}, {-20.0F, "-20"},
        };

        for(const Tick& tick : Ticks) {
            const int y = yForDb(tick.db);

            painter.setPen(QColor{95, 95, 95});
            painter.drawLine(barRect.left() - 4, y, barRect.right() + 3, y);

            painter.setPen(QColor{145, 145, 145});
            const QString label = QString::fromLatin1(tick.label);
            const int baseline  = std::clamp(y + (metrics.ascent() / 2), metrics.ascent(), height() - 2);
            painter.drawText(scaleRect.left(), baseline, label);
        }

        const int fillBottom = yForDb(m_displayDb);
        if(fillBottom > barRect.top()) {
            QRect fill = barRect;
            fill.setBottom(fillBottom);

            QLinearGradient gradient{fill.topLeft(), fill.bottomLeft()};
            gradient.setColorAt(0.00, QColor{70, 210, 90});
            gradient.setColorAt(0.30, QColor{170, 220, 70});
            gradient.setColorAt(0.55, QColor{235, 190, 45});
            gradient.setColorAt(0.78, QColor{235, 110, 35});
            gradient.setColorAt(1.00, QColor{220, 45, 35});

            painter.fillRect(fill, gradient);
        }

        if(m_holdDb < -0.05F) {
            const int holdY = yForDb(m_holdDb);
            painter.setPen(QPen{QColor{255, 255, 255, 210}, 2});
            painter.drawLine(barRect.left() - 2, holdY, barRect.right() + 2, holdY);
        }

        painter.setPen(QColor{95, 95, 95});
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(barRect.adjusted(-1, -1, 1, 1), 3, 3);

        painter.setPen(QColor{125, 125, 125});
        painter.drawText(textRect, Qt::AlignTop | Qt::AlignLeft, tr("GR"));
        painter.drawText(textRect, Qt::AlignBottom | Qt::AlignLeft, tr("dB"));
    }

private:
    static constexpr int HoldFrames = 50;

    float m_valueDb{0.0F};
    float m_displayDb{0.0F};
    float m_holdDb{0.0F};
    float m_maxDb{0.0F};
    int m_holdCounter{0};
};

LimiterSettingsWidget::LimiterSettingsWidget(QWidget* parent)
    : DspSettingsDialog{parent}
    , m_enableCheck{new QCheckBox(tr("Enable limiter"), this)}
    , m_ceilingSpin{createSpinBox(-20.0, 0.0, 0.1, 2, tr(" dBFS"), this)}
    , m_inputGainSpin{createSpinBox(-20.0, 20.0, 0.1, 2, tr(" dB"), this)}
    , m_attackSpin{createSpinBox(0.0, 50.0, 0.1, 2, tr(" ms"), this)}
    , m_releaseSpin{createSpinBox(1.0, 2000.0, 1.0, 1, tr(" ms"), this)}
    , m_lookaheadSpin{createSpinBox(0.0, 50.0, 0.5, 2, tr(" ms"), this)}
    , m_kneeSpin{createSpinBox(0.0, 12.0, 0.5, 2, tr(" dB"), this)}
    , m_truePeakCheck{new QCheckBox(tr("True peak detection"), this)}
    , m_gainReductionMeter{new GrMeterWidget(this)}
    , m_gainReductionCurrentLabel{new QLabel(this)}
    , m_gainReductionMaxLabel{new QLabel(this)}
{
    setWindowTitle(tr("Limiter Settings"));
    setRestoreDefaultsVisible(true);

    auto* root = contentLayout();

    auto* mainLayout = new QHBoxLayout();
    mainLayout->setSpacing(12);

    auto* parameterGroup = new QGroupBox(tr("Parameters"), this);
    auto* form           = new QFormLayout(parameterGroup);
    form->setSpacing(8);
    form->setContentsMargins(10, 10, 10, 10);

    form->addRow(m_enableCheck);
    form->addRow(tr("Ceiling:"), m_ceilingSpin);
    form->addRow(tr("Input gain:"), m_inputGainSpin);
    form->addRow(createSeparator(parameterGroup));
    form->addRow(tr("Attack:"), m_attackSpin);
    form->addRow(tr("Release:"), m_releaseSpin);
    form->addRow(tr("Lookahead:"), m_lookaheadSpin);
    form->addRow(createSeparator(parameterGroup));
    form->addRow(tr("Knee:"), m_kneeSpin);
    form->addRow(m_truePeakCheck);

    m_enableCheck->setToolTip(tr("Quickly enable or bypass the limiter."));

    m_ceilingSpin->setToolTip(tr("Maximum output level in dBFS.\n"
                                 "A ceiling below 0 dBFS helps prevent clipping after reconstruction."));

    m_inputGainSpin->setToolTip(tr("Gain applied before limiting.\n"
                                   "Use positive values only if you intentionally want more limiting."));

    m_attackSpin->setToolTip(tr("How quickly gain reduction is applied.\n"
                                "0 ms is recommended when lookahead is enabled."));

    m_releaseSpin->setToolTip(tr("How quickly gain reduction recovers after peaks.\n"
                                 "Longer values are smoother; shorter values recover faster."));

    m_lookaheadSpin->setToolTip(tr("Audio delay used to anticipate peaks.\n"
                                   "Adds latency equal to the selected lookahead time.\n"
                                   "Very low values are less reliable for true-peak limiting."));

    m_kneeSpin->setToolTip(tr("Soft-knee width in dB.\n"
                              "Higher values make limiting start more gradually."));

    m_truePeakCheck->setToolTip(
        tr("Approximate true peak detection using 4× FIR oversampling.\n"
           "Recommended for reducing inter-sample peak overs.\n"
           "When enabled, a small internal lookahead is used even if lookahead is set to 0 ms."));

    mainLayout->addWidget(parameterGroup, 1);

    auto* meterGroup  = new QGroupBox(tr("Gain Reduction"), this);
    auto* meterLayout = new QVBoxLayout(meterGroup);
    meterLayout->setAlignment(Qt::AlignHCenter);
    meterLayout->setContentsMargins(8, 10, 8, 10);
    meterLayout->setSpacing(6);

    meterLayout->addWidget(m_gainReductionMeter, 0, Qt::AlignHCenter);

    m_gainReductionCurrentLabel->setAlignment(Qt::AlignHCenter);
    m_gainReductionMaxLabel->setAlignment(Qt::AlignHCenter);

    m_gainReductionCurrentLabel->setStyleSheet(u"font-weight: 600;"_s);
    m_gainReductionMaxLabel->setStyleSheet(u"color: #888;"_s);

    m_gainReductionCurrentLabel->setText(tr("Now 0.0 dB"));
    m_gainReductionMaxLabel->setText(tr("Max 0.0 dB"));

    meterLayout->addWidget(m_gainReductionCurrentLabel);
    meterLayout->addWidget(m_gainReductionMaxLabel);

    auto* clearButton = new QPushButton(tr("Clear"), this);
    clearButton->setToolTip(tr("Clear the maximum gain-reduction reading"));
    clearButton->setMinimumWidth(clearButton->fontMetrics().horizontalAdvance(clearButton->text()) + 28);
    meterLayout->addWidget(clearButton, 0, Qt::AlignHCenter);

    QObject::connect(clearButton, &QPushButton::clicked, this, [this]() {
        m_gainReductionMeter->clearMaximum();
        m_gainReductionMaxLabel->setText(tr("Max 0.0 dB"));
    });

    mainLayout->addWidget(meterGroup);

    root->addLayout(mainLayout);
    root->addStretch(1);

    connectSignals();
    applyToUi(LimiterDsp::defaultSettings());

    m_meterPollTimer.start(MeterPollMs, this);

    if(auto* dialogLayout = layout()) {
        dialogLayout->setSizeConstraint(QLayout::SetFixedSize);
    }
}

void LimiterSettingsWidget::loadSettings(const QByteArray& settings)
{
    LimiterDsp dsp;

    if(!settings.isEmpty()) {
        dsp.loadSettings(settings);
    }

    applyToUi(dsp.settings());
}

QByteArray LimiterSettingsWidget::saveSettings() const
{
    LimiterDsp dsp;
    dsp.setSettings(readFromUi());
    return dsp.saveSettings();
}

void LimiterSettingsWidget::restoreDefaults()
{
    applyToUi(LimiterDsp::defaultSettings());
    schedulePreview();
}

void LimiterSettingsWidget::timerEvent(QTimerEvent* event)
{
    if(event->timerId() == m_previewTimer.timerId()) {
        m_previewTimer.stop();
        publishPreviewSettings();
        return;
    }

    if(event->timerId() == m_meterPollTimer.timerId()) {
        updateGainReductionMeter();
        return;
    }

    DspSettingsDialog::timerEvent(event);
}

void LimiterSettingsWidget::connectSignals()
{
    const auto schedule = [this]() {
        schedulePreview();
    };

    QObject::connect(m_enableCheck, &QCheckBox::toggled, this, [this, schedule](bool enabled) {
        m_ceilingSpin->setEnabled(enabled);
        m_inputGainSpin->setEnabled(enabled);
        m_attackSpin->setEnabled(enabled);
        m_releaseSpin->setEnabled(enabled);
        m_lookaheadSpin->setEnabled(enabled);
        m_kneeSpin->setEnabled(enabled);
        m_truePeakCheck->setEnabled(enabled);

        if(!enabled) {
            m_gainReductionMeter->setValue(0.0F);
            m_gainReductionCurrentLabel->setText(tr("Now 0.0 dB"));
        }

        schedule();
    });

    QObject::connect(m_ceilingSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                     [schedule](double) { schedule(); });
    QObject::connect(m_inputGainSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                     [schedule](double) { schedule(); });
    QObject::connect(m_attackSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                     [schedule](double) { schedule(); });
    QObject::connect(m_releaseSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                     [schedule](double) { schedule(); });
    QObject::connect(m_lookaheadSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                     [schedule](double) { schedule(); });
    QObject::connect(m_kneeSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                     [schedule](double) { schedule(); });
    QObject::connect(m_truePeakCheck, &QCheckBox::toggled, this, [schedule](bool) { schedule(); });
}

void LimiterSettingsWidget::applyToUi(const LimiterDsp::Settings& settings)
{
    const QSignalBlocker blockEnabled{m_enableCheck};
    const QSignalBlocker blockCeiling{m_ceilingSpin};
    const QSignalBlocker blockInputGain{m_inputGainSpin};
    const QSignalBlocker blockAttack{m_attackSpin};
    const QSignalBlocker blockRelease{m_releaseSpin};
    const QSignalBlocker blockLookahead{m_lookaheadSpin};
    const QSignalBlocker blockKnee{m_kneeSpin};
    const QSignalBlocker blockTruePeak{m_truePeakCheck};

    m_enableCheck->setChecked(settings.enabled);
    m_ceilingSpin->setValue(settings.ceilingDb);
    m_inputGainSpin->setValue(settings.inputGainDb);
    m_attackSpin->setValue(settings.attackMs);
    m_releaseSpin->setValue(settings.releaseMs);
    m_lookaheadSpin->setValue(settings.lookaheadMs);
    m_kneeSpin->setValue(settings.kneeDb);
    m_truePeakCheck->setChecked(settings.truePeak);

    m_ceilingSpin->setEnabled(settings.enabled);
    m_inputGainSpin->setEnabled(settings.enabled);
    m_attackSpin->setEnabled(settings.enabled);
    m_releaseSpin->setEnabled(settings.enabled);
    m_lookaheadSpin->setEnabled(settings.enabled);
    m_kneeSpin->setEnabled(settings.enabled);
    m_truePeakCheck->setEnabled(settings.enabled);
}

LimiterDsp::Settings LimiterSettingsWidget::readFromUi() const
{
    LimiterDsp::Settings settings;

    settings.enabled     = m_enableCheck->isChecked();
    settings.ceilingDb   = LimiterDsp::clampCeiling(m_ceilingSpin->value());
    settings.inputGainDb = LimiterDsp::clampInputGain(m_inputGainSpin->value());
    settings.attackMs    = LimiterDsp::clampAttack(m_attackSpin->value());
    settings.releaseMs   = LimiterDsp::clampRelease(m_releaseSpin->value());
    settings.lookaheadMs = LimiterDsp::clampLookahead(m_lookaheadSpin->value());
    settings.kneeDb      = LimiterDsp::clampKnee(m_kneeSpin->value());
    settings.truePeak    = m_truePeakCheck->isChecked();

    return settings;
}

void LimiterSettingsWidget::schedulePreview()
{
    m_previewTimer.start(PreviewDebounceMs, this);
}

void LimiterSettingsWidget::updateGainReductionMeter()
{
    const float gr = m_enableCheck->isChecked() ? LimiterDsp::globalGainReductionDb() : 0.0F;

    m_gainReductionMeter->setValue(gr);
    m_gainReductionCurrentLabel->setText(tr("Now %1").arg(formatGainReduction(m_gainReductionMeter->valueDb())));
    m_gainReductionMaxLabel->setText(tr("Max %1").arg(formatGainReduction(m_gainReductionMeter->maxDb())));
}

QString LimiterSettingsProvider::id() const
{
    return QStringLiteral("fooyin.dsp.limiter");
}

QString LimiterSettingsProvider::displayName() const
{
    return QCoreApplication::translate("Fooyin::Limiter::LimiterSettingsProvider", "Limiter");
}

QString LimiterSettingsProvider::viewMenuText() const
{
    return QCoreApplication::translate("Fooyin::Limiter::LimiterSettingsProvider", "Limiter");
}

QString LimiterSettingsProvider::viewMenuStatusTip() const
{
    return QCoreApplication::translate("Fooyin::Limiter::LimiterSettingsProvider", "Open Limiter settings");
}

bool LimiterSettingsProvider::showInViewMenu() const
{
    return true;
}

bool LimiterSettingsProvider::showAsLayoutWidget() const
{
    return false;
}

DspSettingsDialog* LimiterSettingsProvider::createSettingsWidget(QWidget* parent)
{
    return new LimiterSettingsWidget(parent);
}
} // namespace Fooyin::Limiter

#include "limitersettingswidget.moc"
