/*****************************************************************************/
/*  lc_pybridge.cpp - LibreCAD scripting bridge plugin                       */
/*                                                                           */
/*  Licensed under the GNU General Public License, version 2 or later, to    */
/*  match the LibreCAD plugin interface this links against.                  */
/*****************************************************************************/

#include "lc_pybridge.h"

#include "lc_bridge_dispatch.h"
#include "lc_bridge_selftest.h"
#include "lc_bridge_server.h"

#include "document_interface.h"

#include <QHash>
#include <QList>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointF>
#include <QString>
#include <QDialog>
#include <QFontDatabase>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QStringList>
#include <QVBoxLayout>
#include <QVariant>

#include <vector>

namespace {

const char *const kPluginTitle = "Python Bridge";

//! Menu entry labels. Registered as separate actions to discover what
//! LibreCAD passes to execComm() as cmd for each one.
const char *const kActionReport = "Python Bridge: Report document";
const char *const kActionUndoProbe = "Python Bridge: Undo probe";
const char *const kActionSelfTest = "Python Bridge: Dispatch self-test";
const char *const kActionBridge = "Python Bridge: Start bridge session";

} // namespace

QString LC_PyBridge::name() const
{
    return tr(kPluginTitle);
}

PluginCapabilities LC_PyBridge::getCapabilities() const
{
    PluginCapabilities capabilities;
    capabilities.menuEntryPoints
        << PluginMenuLocation(QStringLiteral("plugins_menu"), tr(kActionReport))
        << PluginMenuLocation(QStringLiteral("plugins_menu"), tr(kActionUndoProbe))
        << PluginMenuLocation(QStringLiteral("plugins_menu"), tr(kActionSelfTest))
        << PluginMenuLocation(QStringLiteral("plugins_menu"), tr(kActionBridge));
    return capabilities;
}

void LC_PyBridge::execComm(Document_Interface *doc, QWidget *parent, QString cmd)
{
    if (!doc) {
        QMessageBox::warning(parent, tr(kPluginTitle),
                             tr("No document interface; open a drawing first."));
        return;
    }

    // Stock plugins all ignore cmd, so whether it identifies the invoked menu
    // action is unverified. Dispatch on it when it matches, and fall back to
    // the report so a single unidentified entry point still does something.
    if (cmd == tr(kActionUndoProbe))
        drawUndoProbe(doc, parent);
    else if (cmd == tr(kActionSelfTest))
        runDispatchSelfTest(doc, parent);
    else if (cmd == tr(kActionBridge))
        runBridgeSession(doc, parent);
    else
        reportDocument(doc, parent, cmd);
}

void LC_PyBridge::reportDocument(Document_Interface *doc, QWidget *parent,
                                 const QString &cmd)
{
    QStringList lines;

    lines << tr("execComm cmd: \"%1\"").arg(cmd.isEmpty() ? tr("<empty>") : cmd);
    lines << QString();

    lines << tr("Current layer: %1").arg(doc->getCurrentLayer());

    const QStringList layers = doc->getAllLayer();
    lines << tr("Layers (%1): %2").arg(layers.size()).arg(layers.join(QStringLiteral(", ")));

    const QStringList blocks = doc->getAllBlocks();
    lines << tr("Blocks (%1): %2")
                 .arg(blocks.size())
                 .arg(blocks.isEmpty() ? tr("<none>") : blocks.join(QStringLiteral(", ")));

    int color = 0;
    QString width;
    QString lineType;
    doc->getCurrentLayerProperties(&color, &width, &lineType);
    lines << tr("Layer properties: color=%1 width=\"%2\" linetype=\"%3\"")
                 .arg(color).arg(width, lineType);

    // Entity census. Both lists and the Plug_Entity objects are owned by the
    // caller, so each one is deleted before returning.
    QList<Plug_Entity *> all;
    if (doc->getAllEntities(&all, false)) {
        QHash<int, int> byType;
        for (Plug_Entity *entity : all) {
            if (!entity)
                continue;
            // lcbridge::entityType(), not Plug_Entity::getEntityType(): the
            // latter reports an RS2::EntityType and so mislabels everything.
            byType[lcbridge::entityType(entity)] += 1;
        }

        lines << QString();
        lines << tr("Entities: %1").arg(all.size());
        for (auto it = byType.constBegin(); it != byType.constEnd(); ++it)
            lines << tr("  %1: %2")
                         .arg(lcbridge::entityTypeName(it.key())).arg(it.value());
    } else {
        lines << tr("getAllEntities() failed");
    }
    qDeleteAll(all);
    all.clear();

    // No selection report here on purpose. getSelect(), getSelectByType(), and
    // getEnt() do not read the existing selection: each starts an interactive
    // LibreCAD action, calls killAllActions(), and spins a nested event loop
    // waiting for the user to pick. There is no non-interactive way to see
    // what is currently selected.
    lines << QString();
    lines << tr("Selection: not readable without prompting the user "
                "(see docs/findings.md, risk 6)");

    int unitsCode = 0;
    const bool haveUnits = doc->getVariableInt(QStringLiteral("$INSUNITS"), &unitsCode);
    lines << QString();
    lines << tr("$INSUNITS: %1").arg(haveUnits ? QString::number(unitsCode) : tr("<unset>"));

    QMessageBox::information(parent, tr(kPluginTitle), lines.join(QStringLiteral("\n")));
}

void LC_PyBridge::drawUndoProbe(Document_Interface *doc, QWidget *parent)
{
    // Each figure below is drawn with a different creation call. Reading the
    // LibreCAD source predicts that all of them land in a single undo step:
    // QC_ApplicationWindow::execPlug() opens an LC_UndoSection around the whole
    // execComm() call, RS_Undo::startUndoCycle() is reference counted, and the
    // per-call LC_UndoSection inside each Document_Interface::add*() therefore
    // nests into that outer cycle instead of starting its own. This confirms
    // that against the running application.
    const QString originalLayer = doc->getCurrentLayer();
    doc->setLayer(QStringLiteral("LC_PYBRIDGE_PROBE"));

    // 1. Three separate addLine() calls: expected to be three undo steps.
    QPointF a(0.0, 0.0);
    QPointF b(50.0, 0.0);
    QPointF c(50.0, 30.0);
    QPointF d(0.0, 30.0);
    doc->addLine(&a, &b);
    doc->addLine(&b, &c);
    doc->addLine(&c, &d);

    // 2. One addLines() call of three segments: expected to be one undo step.
    const std::vector<QPointF> chain{
        QPointF(70.0, 0.0),
        QPointF(120.0, 0.0),
        QPointF(120.0, 30.0),
        QPointF(70.0, 30.0),
    };
    doc->addLines(chain, false);

    // 3. One addPolyline() call, closed, with a bulge on the last vertex:
    //    expected to be one undo step for one POLYLINE entity.
    const std::vector<Plug_VertexData> polyline{
        Plug_VertexData(QPointF(140.0, 0.0), 0.0),
        Plug_VertexData(QPointF(190.0, 0.0), 0.0),
        Plug_VertexData(QPointF(190.0, 30.0), 0.0),
        Plug_VertexData(QPointF(140.0, 30.0), 0.5),
    };
    doc->addPolyline(polyline, true);

    // 4. Circle, arc, and text, one undo step each. addArc() takes its angles
    //    in DEGREES: doc_plugin_interface.cpp applies deg2rad internally.
    QPointF circleCentre(210.0, 15.0);
    doc->addCircle(&circleCentre, 15.0);

    QPointF arcCentre(250.0, 15.0);
    doc->addArc(&arcCentre, 15.0, 0.0, 180.0);

    QPointF labelAt(0.0, 40.0);
    doc->addText(QStringLiteral("LC_PyBridge undo probe"), QStringLiteral("standard"),
                 &labelAt, 5.0, 0.0, DPI::HAlignLeft, DPI::VAlignBottom);

    doc->setLayer(originalLayer);
    doc->updateView();

    QMessageBox::information(
        parent, tr(kPluginTitle),
        tr("Drew 9 entities on layer LC_PYBRIDGE_PROBE in 8 add*() calls:\n"
           "\n"
           "  3 x addLine()     - 3 lines\n"
           "  1 x addLines()    - 3 lines\n"
           "  1 x addPolyline() - 1 closed polyline with one bulge\n"
           "  1 x addCircle(), 1 x addArc(), 1 x addText()\n"
           "\n"
           "Prediction from the LibreCAD source: a single undo step for the "
           "whole execComm() call, because execPlug() opens an outer "
           "LC_UndoSection and undo cycles nest by reference count.\n"
           "\n"
           "So ONE press of Ctrl+Z should remove all 9 entities. Press Ctrl+Z "
           "once and check, then record the result in docs/findings.md.\n"
           "\n"
           "Note that the LC_PYBRIDGE_PROBE layer is not removed by undo: "
           "setLayer() creates layers outside the undo system."));
}

void LC_PyBridge::runDispatchSelfTest(Document_Interface *doc, QWidget *parent)
{
    // The Dispatcher lives on the stack: the Document_Interface it borrows is
    // only valid until execComm() returns.
    lcbridge::Dispatcher dispatcher(doc);
    const lcbridge::SelfTestResult result = lcbridge::runSelfTest(dispatcher);

    doc->updateView();

    QDialog dialog(parent);
    dialog.setWindowTitle(result.ok()
                              ? tr("Dispatch self-test: %1 passed").arg(result.passed)
                              : tr("Dispatch self-test: %1 failed").arg(result.failed));
    dialog.resize(900, 600);

    auto *layout = new QVBoxLayout(&dialog);

    auto *output = new QPlainTextEdit(&dialog);
    output->setReadOnly(true);
    output->setLineWrapMode(QPlainTextEdit::NoWrap);
    output->document()->setDefaultFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    output->setPlainText(result.report);
    layout->addWidget(output);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    layout->addWidget(buttons);

    dialog.exec();
}

void LC_PyBridge::runBridgeSession(Document_Interface *doc, QWidget *parent)
{
    // Server and dispatcher live on the stack: the Document_Interface they
    // borrow dies when execComm() returns, and serve() keeps execComm() on the
    // stack for the whole session. Everything a session draws is one undo
    // step, because execPlug() wraps this call in an LC_UndoSection.
    lcbridge::BridgeServer server(doc);

    if (!server.listen(lcbridge::BridgeServer::defaultSocketName())) {
        QMessageBox::warning(parent, tr(kPluginTitle),
                             tr("Could not open the bridge socket:\n%1")
                                 .arg(server.errorString()));
        return;
    }

    // Non-modal status window. LibreCAD stays usable during the session; this
    // just shows activity and offers the only way to end it from the GUI.
    QDialog dialog(parent);
    dialog.setWindowTitle(tr("Python bridge session"));

    auto *layout = new QVBoxLayout(&dialog);
    auto *status = new QLabel(tr("Listening on %1\n\n"
                                 "Waiting for a client. The whole session will "
                                 "be one undo step.")
                                  .arg(server.fullServerName()),
                              &dialog);
    status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(status);

    auto *stopButton = new QPushButton(tr("Stop session"), &dialog);
    layout->addWidget(stopButton);

    connect(stopButton, &QPushButton::clicked, &server, &lcbridge::BridgeServer::stop);
    // Closing the window must also end the nested loop, or the session would
    // keep running with no way to reach it.
    connect(&dialog, &QDialog::finished, &server, &lcbridge::BridgeServer::stop);

    const QString socketPath = server.fullServerName();
    connect(&server, &lcbridge::BridgeServer::clientChanged, status,
            [status, socketPath](bool connected) {
                status->setText(connected
                                    ? tr("Client connected on %1").arg(socketPath)
                                    : tr("Client disconnected; still listening "
                                         "on %1").arg(socketPath));
            });
    connect(&server, &lcbridge::BridgeServer::requestHandled, status,
            [status](int total) {
                status->setText(tr("Requests handled: %1").arg(total));
            });

    dialog.show();

    const int handled = server.serve();

    dialog.hide();
    doc->updateView();
    QMessageBox::information(parent, tr(kPluginTitle),
                             tr("Bridge session ended after %1 requests.")
                                 .arg(handled));
}
