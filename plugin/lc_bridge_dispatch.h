/*****************************************************************************/
/*  lc_bridge_dispatch.h - named operations over Document_Interface          */
/*                                                                           */
/*  The dispatch layer the Python side will eventually talk to. Requests and  */
/*  responses are JSON objects, so this is already the wire format; the       */
/*  socket transport added later only has to move bytes.                     */
/*                                                                           */
/*  Licensed under the GNU General Public License, version 2 or later.       */
/*****************************************************************************/

#ifndef LC_BRIDGE_DISPATCH_H
#define LC_BRIDGE_DISPATCH_H

#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>

class Document_Interface;
class Plug_Entity;

namespace lcbridge {

/**
 * The DPI::ETYPE of \a entity.
 *
 * Not Plug_Entity::getEntityType(), which cannot be used: LibreCAD's
 * Plugin_Entity does not derive from Plug_Entity -- it is a separate class
 * with a hand-matched vtable, reached by reinterpret_cast -- and its
 * getEntityType() is declared to return RS2::EntityType, a different
 * enumeration with a different order. Calling it through the plugin interface
 * yields an RS2 value that happens to be a valid-looking DPI::ETYPE: a POINT
 * reads back as IMAGE, a LINE as OVERLAYBOX, a CIRCLE as INSERT.
 *
 * The type reported in the attribute hash under the DPI::ETYPE key is the
 * reliable one, so this reads that instead.
 */
int entityType(Plug_Entity *entity);

//! Name of a DPI::ETYPE value, e.g. "LINE". "UNKNOWN" for anything unrecognised.
QString entityTypeName(int type);

/**
 * Executes named operations against a LibreCAD document.
 *
 * Request:  {"op": "add_line", "args": {...}, "id": 7}
 * Success:  {"ok": true,  "id": 7, "result": ...}
 * Failure:  {"ok": false, "id": 7, "error": {"code": "...", "message": "..."}}
 *
 * The "id" field is echoed back when present and omitted otherwise. Every
 * failure is reported in the response; dispatch() does not throw.
 *
 * Conventions shared by all operations:
 *   - Points are two-element arrays, [x, y].
 *   - Polyline vertices are [x, y] or [x, y, bulge].
 *   - **Angles are radians everywhere**, including add_arc. The underlying
 *     Document_Interface::addArc() takes degrees while every other angle in
 *     the API is radians; the conversion happens here so that callers never
 *     have to know.
 *   - Colors are integers: -1 ByLayer, -2 ByBlock, otherwise 24-bit RGB.
 *
 * A Dispatcher borrows its Document_Interface and must not outlive it. That
 * pointer is only valid for the duration of one QC_PluginInterface::execComm()
 * call, so a Dispatcher belongs on the stack inside execComm().
 */
class Dispatcher
{
public:
    explicit Dispatcher(Document_Interface *doc);
    ~Dispatcher();

    Dispatcher(const Dispatcher &) = delete;
    Dispatcher &operator=(const Dispatcher &) = delete;

    //! Execute one request and return its response.
    QJsonObject dispatch(const QJsonObject &request);

    //! Names of every supported operation, sorted.
    static QStringList operations();

private:
    // Handlers return the value that becomes "result", and signal failure by
    // throwing RequestError. Registered in the table built by handlers().
    using Handler = QJsonValue (Dispatcher::*)(const QJsonObject &);
    static const QHash<QString, Handler> &handlers();

    // Entity handles. getAllEntities() hands out heap-allocated Plug_Entity
    // wrappers that the caller owns; they are kept here behind small integer
    // handles so the protocol never has to carry a pointer, and released
    // together when the Dispatcher goes away.
    int registerEntity(Plug_Entity *entity);
    Plug_Entity *lookupEntity(const QJsonObject &args) const;
    void invalidateEntity(const QJsonObject &args);

    QJsonValue opPing(const QJsonObject &args);
    QJsonValue opOperations(const QJsonObject &args);
    QJsonValue opBatch(const QJsonObject &args);
    QJsonValue opUpdateView(const QJsonObject &args);

    QJsonValue opAddPoint(const QJsonObject &args);
    QJsonValue opAddLine(const QJsonObject &args);
    QJsonValue opAddLines(const QJsonObject &args);
    QJsonValue opAddPolyline(const QJsonObject &args);
    QJsonValue opAddSplinePoints(const QJsonObject &args);
    QJsonValue opAddCircle(const QJsonObject &args);
    QJsonValue opAddArc(const QJsonObject &args);
    QJsonValue opAddEllipse(const QJsonObject &args);
    QJsonValue opAddText(const QJsonObject &args);
    QJsonValue opAddInsert(const QJsonObject &args);
    QJsonValue opAddBlockFromFile(const QJsonObject &args);

    QJsonValue opGetCurrentLayer(const QJsonObject &args);
    QJsonValue opSetLayer(const QJsonObject &args);
    QJsonValue opGetLayers(const QJsonObject &args);
    QJsonValue opDeleteLayer(const QJsonObject &args);
    QJsonValue opGetLayerProperties(const QJsonObject &args);
    QJsonValue opSetLayerProperties(const QJsonObject &args);

    QJsonValue opGetBlocks(const QJsonObject &args);

    QJsonValue opGetEntities(const QJsonObject &args);
    QJsonValue opReleaseHandles(const QJsonObject &args);
    QJsonValue opUnselect(const QJsonObject &args);

    QJsonValue opEntityData(const QJsonObject &args);
    QJsonValue opEntityUpdate(const QJsonObject &args);
    QJsonValue opEntityPolyline(const QJsonObject &args);
    QJsonValue opEntitySetPolyline(const QJsonObject &args);
    QJsonValue opEntityMove(const QJsonObject &args);
    QJsonValue opEntityRotate(const QJsonObject &args);
    QJsonValue opEntityMoveRotate(const QJsonObject &args);
    QJsonValue opEntityScale(const QJsonObject &args);
    QJsonValue opEntityRemove(const QJsonObject &args);

    QJsonValue opGetVariable(const QJsonObject &args);
    QJsonValue opSetVariable(const QJsonObject &args);

    QJsonValue opRealToString(const QJsonObject &args);

    Document_Interface *m_doc {nullptr};
    QHash<int, Plug_Entity *> m_entities;
    int m_nextHandle {1};
    int m_batchDepth {0};
};

} // namespace lcbridge

#endif // LC_BRIDGE_DISPATCH_H
