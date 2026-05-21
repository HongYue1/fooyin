create_fooyin_plugin_internal(
    limiter
    DEPENDS Fooyin::Core
            Fooyin::Gui
    SOURCES limiterdsp.cpp
            limiterdsp.h
            limiterplugin.cpp
            limiterplugin.h
            limitersettingswidget.cpp
            limitersettingswidget.h
)
