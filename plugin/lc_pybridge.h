/*****************************************************************************/
/*  lc_pybridge.h - LibreCAD scripting bridge plugin                         */
/*                                                                           */
/*  Registers one menu entry that serves the open drawing to Python clients  */
/*  over a local socket. See docs/findings.md for what was established about */
/*  the plugin API this is built on.                                         */
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
    //! Open the bridge socket and serve Python clients until the user stops
    //! the session or a client sends {"op": "shutdown"}.
    void runBridgeSession(Document_Interface *doc, QWidget *parent);
};

#endif // LC_PYBRIDGE_H
