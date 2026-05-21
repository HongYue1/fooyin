/*
 * Fooyin Limiter Plugin
 */

#include "limiterplugin.h"

#include "limiterdsp.h"
#include "limitersettingswidget.h"

namespace Fooyin::Limiter {

std::vector<DspNode::Entry> LimiterPlugin::dspCreators() const
{
    return {{
        .id      = QStringLiteral("fooyin.dsp.limiter"),
        .name    = QStringLiteral("Limiter"),
        .factory = [] { return std::make_unique<LimiterDsp>(); },
    }};
}

std::vector<std::unique_ptr<DspSettingsProvider>> LimiterPlugin::settingsProviders() const
{
    std::vector<std::unique_ptr<DspSettingsProvider>> providers;
    providers.emplace_back(std::make_unique<LimiterSettingsProvider>());
    return providers;
}

} // namespace Fooyin::Limiter
