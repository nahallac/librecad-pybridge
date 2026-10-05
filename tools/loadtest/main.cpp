// Loads a LibreCAD plugin with QPluginLoader and reports its metadata.
// Usage: loadtest <path-to-plugin.so>
#include "qc_plugininterface.h"

#include <QApplication>
#include <QDebug>
#include <QJsonDocument>

#include <cstdio>
#include <QPluginLoader>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    if (argc < 2) {
        std::fprintf(stderr, "usage: loadtest <path-to-plugin.so>\n");
        return 2;
    }

    QPluginLoader loader(argv[1]);
    QObject *instance = loader.instance();
    if (!instance) {
        std::fprintf(stderr, "load failed: %s\n",
                     qUtf8Printable(loader.errorString()));
        return 1;
    }

    std::printf("metadata: %s\n",
                QJsonDocument(loader.metaData()).toJson(QJsonDocument::Compact).constData());

    auto *plugin = qobject_cast<QC_PluginInterface *>(instance);
    if (!plugin) {
        std::fprintf(stderr, "loaded, but does not implement QC_PluginInterface"
                             " (IID mismatch)\n");
        return 1;
    }

    std::printf("name(): %s\n", qUtf8Printable(plugin->name()));
    const PluginCapabilities capabilities = plugin->getCapabilities();
    for (const PluginMenuLocation &location : capabilities.menuEntryPoints) {
        std::printf("menu entry: %s -> %s\n",
                    qUtf8Printable(location.menuEntryPoint),
                    qUtf8Printable(location.menuEntryActionName));
    }
    std::printf("paint event priorities: %d\n",
                capabilities.paintEventPriorities.size());
    return 0;
}
