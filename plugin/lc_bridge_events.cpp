/*****************************************************************************/
/*  lc_bridge_events.cpp - push events from the session to its client        */
/*                                                                           */
/*  Licensed under the GNU General Public License, version 2 or later.       */
/*****************************************************************************/

#include "lc_bridge_events.h"

#include "lc_bridge_native.h"

#include "document_interface.h"

#include <QJsonArray>
#include <QList>
#include <QMetaObject>
#include <QTimer>
#include <QWidget>

namespace lcbridge {
namespace {

const char *const kDocumentModified = "document_modified";
const char *const kSelectionChanged = "selection_changed";
const char *const kEntityCountChanged = "entity_count_changed";
const char *const kLayerChanged = "layer_changed";
const char *const kViewChanged = "view_changed";
const char *const kGridChanged = "grid_changed";
const char *const kWindowsChanged = "windows_changed";
const char *const kSessionEnding = "session_ending";

//! Events the timer serves; the others come from a signal or the server.
const char *const kPolled[] = {
    kDocumentModified, kSelectionChanged, kEntityCountChanged,
    kLayerChanged, kViewChanged, kGridChanged,
};

} // namespace

EventMonitor::EventMonitor(Document_Interface *doc, NativeBridge *native,
                           QObject *parent)
    : QObject(parent)
    , m_doc(doc)
    , m_native(native)
    , m_timer(new QTimer(this))
{
    m_timer->setInterval(100);
    connect(m_timer, &QTimer::timeout, this, &EventMonitor::onTimer);

    // QC_ApplicationWindow's own signal, by name. The string form looks the
    // signal up in the object's meta-object at run time, so nothing about
    // QC_ApplicationWindow is referenced from this file; on another LibreCAD
    // version a missing signal just means the event never fires. Checked
    // first so a missing one does not print a connect() warning. It fires
    // when the active drawing window changes (another drawing opened or
    // activated by the user, the last one closed); the session stays bound
    // to its own drawing either way.
    QWidget *window = m_native ? m_native->mainWindow() : nullptr;
    if (window && window->metaObject()->indexOfSignal("windowsChanged(bool)") >= 0) {
        connect(window, SIGNAL(windowsChanged(bool)),
                this, SLOT(onWindowsChanged(bool)));
    }
}

EventMonitor::~EventMonitor() = default;

QStringList EventMonitor::eventNames()
{
    QStringList names{
        QString::fromLatin1(kDocumentModified), QString::fromLatin1(kSelectionChanged),
        QString::fromLatin1(kEntityCountChanged), QString::fromLatin1(kLayerChanged),
        QString::fromLatin1(kViewChanged), QString::fromLatin1(kGridChanged),
        QString::fromLatin1(kWindowsChanged),
        QString::fromLatin1(kSessionEnding),
    };
    names.sort();
    return names;
}

bool EventMonitor::subscribe(const QStringList &names, QString *error)
{
    const QStringList known = eventNames();
    QSet<QString> wanted;
    for (const QString &name : names) {
        if (name == QLatin1String("*")) {
            for (const QString &each : known)
                wanted.insert(each);
        } else if (known.contains(name)) {
            wanted.insert(name);
        } else {
            *error = QStringLiteral("unknown event \"%1\"; known: %2")
                         .arg(name, known.join(QStringLiteral(", ")));
            return false;
        }
    }

    // Report what the existing subscriptions have pending, then widen: the
    // poll right after establishes baselines for the newly watched state.
    poll();
    m_subscribed.unite(wanted);
    poll();
    updateTimer();
    return true;
}

void EventMonitor::unsubscribe(const QStringList &names)
{
    if (names.isEmpty() || names.contains(QStringLiteral("*"))) {
        m_subscribed.clear();
    } else {
        for (const QString &name : names)
            m_subscribed.remove(name);
    }
    // Forget baselines of state nobody watches any more, so a later
    // subscription starts from a fresh snapshot.
    m_last = snapshot();
    updateTimer();
}

QStringList EventMonitor::subscriptions() const
{
    QStringList names = m_subscribed.values();
    names.sort();
    return names;
}

bool EventMonitor::isSubscribed(const QString &name) const
{
    return m_subscribed.contains(name);
}

void EventMonitor::updateTimer()
{
    bool polled = false;
    for (const char *name : kPolled)
        polled = polled || m_subscribed.contains(QString::fromLatin1(name));
    if (polled && !m_timer->isActive())
        m_timer->start();
    else if (!polled)
        m_timer->stop();
}

void EventMonitor::onTimer()
{
    if (!m_suspended)
        poll();
}

EventMonitor::State EventMonitor::snapshot() const
{
    State state;
    if (!m_doc)
        return state;

    const bool wantCount = isSubscribed(QString::fromLatin1(kEntityCountChanged));
    const bool wantSelection = isSubscribed(QString::fromLatin1(kSelectionChanged));
    if (wantCount || wantSelection) {
        int live = 0;
        int selected = 0;
        if (m_native && m_native->census(&live, &selected)) {
            // One pass over the engine's list, no wrappers allocated.
            state.hasCount = true;
            state.live = live;
            state.hasSelection = true;
            state.selected = selected;
        } else if (wantCount) {
            // Plugin API: a wrapper per entity, and undone entities can only
            // be told apart with the native layer. This is the stub's path.
            QList<Plug_Entity *> entities;
            if (m_doc->getAllEntities(&entities, false)) {
                for (Plug_Entity *entity : entities) {
                    bool undone = false;
                    if (!(m_native && m_native->isUndone(entity, &undone) && undone))
                        ++live;
                }
                state.hasCount = true;
                state.live = live;
            }
            qDeleteAll(entities);
        }
    }

    if (isSubscribed(QString::fromLatin1(kLayerChanged))) {
        state.hasLayers = true;
        state.currentLayer = m_doc->getCurrentLayer();
        state.layers = m_doc->getAllLayer();
    }

    if (isSubscribed(QString::fromLatin1(kDocumentModified)) && m_native) {
        QString path;
        bool modified = false;
        if (m_native->fileInfo(&path, &modified)) {
            state.hasModified = true;
            state.modified = modified;
        }
    }

    if (isSubscribed(QString::fromLatin1(kViewChanged)) && m_native) {
        state.hasView = m_native->viewState(&state.factor, &state.offsetX,
                                            &state.offsetY);
    }

    if (isSubscribed(QString::fromLatin1(kGridChanged)) && m_native)
        state.hasGrid = m_native->gridState(&state.gridOn);
    return state;
}

void EventMonitor::poll()
{
    if (m_subscribed.isEmpty())
        return;
    const State now = snapshot();
    const State &was = m_last;

    if (now.hasCount && was.hasCount && now.live != was.live
        && isSubscribed(QString::fromLatin1(kEntityCountChanged))) {
        QJsonObject data;
        data.insert(QStringLiteral("count"), now.live);
        data.insert(QStringLiteral("previous"), was.live);
        raise(QString::fromLatin1(kEntityCountChanged), data);
    }
    if (now.hasSelection && was.hasSelection && now.selected != was.selected
        && isSubscribed(QString::fromLatin1(kSelectionChanged))) {
        QJsonObject data;
        data.insert(QStringLiteral("count"), now.selected);
        data.insert(QStringLiteral("previous"), was.selected);
        raise(QString::fromLatin1(kSelectionChanged), data);
    }
    if (now.hasLayers && was.hasLayers
        && (now.currentLayer != was.currentLayer || now.layers != was.layers)) {
        QJsonObject data;
        data.insert(QStringLiteral("current"), now.currentLayer);
        data.insert(QStringLiteral("layers"), QJsonArray::fromStringList(now.layers));
        raise(QString::fromLatin1(kLayerChanged), data);
    }
    if (now.hasModified && was.hasModified && now.modified != was.modified) {
        QJsonObject data;
        data.insert(QStringLiteral("modified"), now.modified);
        raise(QString::fromLatin1(kDocumentModified), data);
    }
    if (now.hasView && was.hasView
        && (now.factor != was.factor || now.offsetX != was.offsetX
            || now.offsetY != was.offsetY)) {
        QJsonObject data;
        data.insert(QStringLiteral("factor"), now.factor);
        data.insert(QStringLiteral("offset"), QJsonArray{now.offsetX, now.offsetY});
        raise(QString::fromLatin1(kViewChanged), data);
    }
    if (now.hasGrid && was.hasGrid && now.gridOn != was.gridOn) {
        QJsonObject data;
        data.insert(QStringLiteral("on"), now.gridOn);
        raise(QString::fromLatin1(kGridChanged), data);
    }

    m_last = now;
}

void EventMonitor::raise(const QString &name, const QJsonObject &data)
{
    if (m_subscribed.contains(name))
        emit eventRaised(name, data);
}

void EventMonitor::onWindowsChanged(bool windowsLeft)
{
    QJsonObject data;
    data.insert(QStringLiteral("windows_left"), windowsLeft);
    raise(QString::fromLatin1(kWindowsChanged), data);
}

} // namespace lcbridge
