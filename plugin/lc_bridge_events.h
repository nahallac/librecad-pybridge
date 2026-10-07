/*****************************************************************************/
/*  lc_bridge_events.h - push events from the session to its client          */
/*                                                                           */
/*  The client subscribes to named events; the server then writes            */
/*  unsolicited {"event": ..., "seq": n, "data": {...}} frames on the        */
/*  connection when they happen. Nothing is sent without a subscription.     */
/*                                                                           */
/*  Sources, chosen so that nothing binds a LibreCAD data symbol or derives  */
/*  from a LibreCAD listener class (either would stop the plugin loading      */
/*  outside LibreCAD):                                                       */
/*   - a 100 ms timer that diffs cheap state -- entity and selection counts, */
/*     the layer list, the modified flag, the view, the grid -- and only     */
/*     runs while something it serves is subscribed;                         */
/*   - QC_ApplicationWindow::windowsChanged(bool), connected by name with    */
/*     the string-based SIGNAL() form, which needs no symbol at all. (Its    */
/*     gridChanged signal is not used: it fires on window activation, not    */
/*     on a grid toggle; draftChanged is never emitted in 2.2.1.5.)          */
/*   - session_ending, raised by the server itself.                          */
/*                                                                           */
/*  Everything runs on the GUI thread, in the server's event loop.           */
/*                                                                           */
/*  Licensed under the GNU General Public License, version 2 or later.       */
/*****************************************************************************/

#ifndef LC_BRIDGE_EVENTS_H
#define LC_BRIDGE_EVENTS_H

#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

class Document_Interface;
class QTimer;

namespace lcbridge {

class NativeBridge;

class EventMonitor : public QObject
{
    Q_OBJECT

public:
    //! \a native may be nullptr (the offline stub): the counts then come
    //! from the plugin API and the native-only events never fire.
    EventMonitor(Document_Interface *doc, NativeBridge *native,
                 QObject *parent = nullptr);
    ~EventMonitor() override;

    //! Every event name a client can subscribe to, sorted.
    static QStringList eventNames();

    //! Add \a names ("*" for all) to the subscription. False, with \a error
    //! set and nothing changed, when a name is unknown. Newly watched state
    //! is snapshotted at once, so only later changes are reported.
    bool subscribe(const QStringList &names, QString *error);
    //! Remove \a names; an empty list or "*" removes everything.
    void unsubscribe(const QStringList &names);
    QStringList subscriptions() const;
    bool isSubscribed(const QString &name) const;

    //! While suspended (the server is handling a request) the timer skips
    //! its polls: a request may be running a nested event loop, and the
    //! state it is changing is only worth reporting once it is done.
    void setSuspended(bool suspended) { m_suspended = suspended; }

signals:
    //! A subscribed event happened. The server turns it into a frame.
    void eventRaised(const QString &name, const QJsonObject &data);

private slots:
    void onTimer();
    void onWindowsChanged(bool windowsLeft);

private:
    //! Polled state. A field is only filled when an event that needs it is
    //! subscribed and its source is available; "valid" says which were.
    struct State {
        bool hasCount {false};
        int live {0};
        bool hasSelection {false};
        int selected {0};
        bool hasLayers {false};
        QString currentLayer;
        QStringList layers;
        bool hasModified {false};
        bool modified {false};
        bool hasView {false};
        double factor {0.0};
        int offsetX {0};
        int offsetY {0};
        bool hasGrid {false};
        bool gridOn {false};
    };

    State snapshot() const;
    //! Take a snapshot, raise an event for every subscribed field that
    //! differs from the last one, keep it as the new baseline.
    void poll();
    void updateTimer();
    void raise(const QString &name, const QJsonObject &data);

    Document_Interface *m_doc {nullptr};
    NativeBridge *m_native {nullptr};
    QTimer *m_timer {nullptr};
    QSet<QString> m_subscribed;
    State m_last;
    bool m_suspended {false};
};

} // namespace lcbridge

#endif // LC_BRIDGE_EVENTS_H
