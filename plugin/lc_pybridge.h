/*****************************************************************************/
/*  lc_pybridge.h - LibreCAD scripting bridge plugin                         */
/*                                                                           */
/*  Milestone 1: prove the toolchain end to end and characterise how edits   */
/*  made through Document_Interface interact with LibreCAD's undo stack.     */
/*                                                                           */
/*  Licensed under the GNU General Public License, version 2 or later, to    */
/*  match the LibreCAD plugin interface this links against.                  */
/*****************************************************************************/

#ifndef LC_PYBRIDGE_H
#define LC_PYBRIDGE_H

#include "qc_plugininterface.h"

#include <QObject>
#include <QString>

class Document_Interface;
class QWidget;

/**
 * Diagnostic plugin for the scripting bridge.
 *
 * Registers two menu entries so that the value LibreCAD passes as \a cmd to
 * execComm() can be observed; the stock plugins all ignore it, so it is not
 * documented anywhere.
 */
class LC_PyBridge : public QObject, QC_PluginInterface
{
    Q_OBJECT
    Q_INTERFACES(QC_PluginInterface)
    Q_PLUGIN_METADATA(IID LC_DocumentInterface_iid FILE "lc_pybridge.json")

public:
    QString name() const override;
    PluginCapabilities getCapabilities() const override;
    void execComm(Document_Interface *doc, QWidget *parent, QString cmd) override;

private:
    //! Report the document state the bridge will need to expose: layers,
    //! blocks, entity counts, drawing variables, and the received \a cmd.
    void reportDocument(Document_Interface *doc, QWidget *parent, const QString &cmd);

    //! Draw a known figure using several different creation calls, so the
    //! number of Ctrl+Z presses needed to remove each part can be counted.
    void drawUndoProbe(Document_Interface *doc, QWidget *parent);

    //! Run the dispatch layer's self-test against the open drawing and show
    //! the report. Milestone 2 stands in for the socket transport this way.
    void runDispatchSelfTest(Document_Interface *doc, QWidget *parent);

    //! Open the bridge socket and serve Python clients until the user stops
    //! the session or a client sends {"op": "shutdown"}.
    void runBridgeSession(Document_Interface *doc, QWidget *parent);
};

#endif // LC_PYBRIDGE_H
