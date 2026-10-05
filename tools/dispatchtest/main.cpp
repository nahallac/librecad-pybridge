// Runs the dispatch layer's self-test sequence against a stub document, with
// no LibreCAD process involved. Exits non-zero if anything fails, so it works
// as a build-time check.
//
// Usage: dispatchtest [--verbose]
//   --verbose  also print every Document_Interface call the stub recorded.

#include "fake_document.h"

#include "lc_bridge_dispatch.h"
#include "lc_bridge_selftest.h"

#include <QCoreApplication>
#include <QString>

#include <cstdio>

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        if (QString::fromLocal8Bit(argv[i]) == QLatin1String("--verbose"))
            verbose = true;
    }

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
