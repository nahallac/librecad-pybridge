// Runs the dispatch layer's self-test sequence against a stub document, with
// no LibreCAD process involved. Exits non-zero if anything fails, so it works
// as a build-time check.
//
// Usage: dispatchtest [--verbose]
//        dispatchtest --serve [socket]
//   --verbose  also print every Document_Interface call the stub recorded.
//   --serve    instead of the self-test, serve the stub document over the
//              bridge socket until a client sends {"op": "shutdown"}. Lets the
//              Python client be tested end to end without LibreCAD.

#include "fake_document.h"

#include "lc_bridge_dispatch.h"
#include "lc_bridge_selftest.h"
#include "lc_bridge_server.h"

#include <QCoreApplication>
#include <QString>

#include <cstdio>

namespace {

int serveStub(const QString &socketName)
{
    faketest::FakeDocument document;
    lcbridge::BridgeServer server(&document);

    if (!server.listen(socketName)) {
        std::fprintf(stderr, "listen failed on \"%s\": %s\n",
                     qUtf8Printable(socketName),
                     qUtf8Printable(server.errorString()));
        return 1;
    }

    std::printf("serving stub document on %s\n",
                qUtf8Printable(server.fullServerName()));
    std::fflush(stdout);

    const int handled = server.serve();
    std::printf("served %d requests\n", handled);
    return 0;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    bool verbose = false;
    bool serve = false;
    QString socketName = lcbridge::BridgeServer::defaultSocketName();
    for (int i = 1; i < argc; ++i) {
        const QString argument = QString::fromLocal8Bit(argv[i]);
        if (argument == QLatin1String("--verbose"))
            verbose = true;
        else if (argument == QLatin1String("--serve"))
            serve = true;
        else if (serve)
            socketName = argument;
    }

    if (serve)
        return serveStub(socketName);

    faketest::FakeDocument document;
    lcbridge::Dispatcher dispatcher(&document);

    const lcbridge::SelfTestResult result = lcbridge::runSelfTest(dispatcher);
    std::printf("%s\n", qUtf8Printable(result.report));
    std::printf("entities left in the stub drawing: %d\n", document.liveEntityCount());

    if (verbose) {
        std::printf("\nDocument_Interface calls:\n");
        const QStringList calls = document.calls();
        for (int i = 0; i < calls.size(); ++i)
            std::printf("  %3d. %s\n", i + 1, qUtf8Printable(calls.at(i)));
    }

    return result.ok() ? 0 : 1;
}
