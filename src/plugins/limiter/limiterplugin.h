/*
 * Fooyin Limiter Plugin
 */

#pragma once

#include <core/engine/dsp/dspplugin.h>
#include <core/plugins/plugin.h>
#include <gui/plugins/dspguiplugin.h>

#include <QObject>

namespace Fooyin::Limiter {

class LimiterPlugin : public QObject,
                      public Plugin,
                      public DspPlugin,
                      public DspGuiPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.fooyin.fooyin.plugin/1.0" FILE "limiter.json")
    Q_INTERFACES(Fooyin::Plugin Fooyin::DspPlugin Fooyin::DspGuiPlugin)

public:
    [[nodiscard]] std::vector<DspNode::Entry> dspCreators() const override;

    [[nodiscard]] std::vector<std::unique_ptr<DspSettingsProvider>> settingsProviders() const override;
};

} // namespace Fooyin::Limiter
