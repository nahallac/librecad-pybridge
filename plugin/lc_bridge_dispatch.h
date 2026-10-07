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
#include <QJsonArray>
#include <QSet>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>

class Document_Interface;
class Plug_Entity;

namespace lcbridge {

class NativeBridge;

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
    //! \a native provides the in-process operations (command injection,
    //! selection, real dimensions, hatches); nullptr turns those operations
    //! into "unavailable" errors, which is how the offline test stub runs.
    explicit Dispatcher(Document_Interface *doc, NativeBridge *native = nullptr);
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
    //! Operations that never change the drawing. Everything else reopens the
    //! undo cycle first (NativeBridge::ensureUndoCycle), which is what
    //! discards redo history -- so a query after undo() keeps redo possible.
    static const QSet<QString> &readOnlyOperations();

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

    QJsonValue opNativeStatus(const QJsonObject &args);
    QJsonValue opExecCommand(const QJsonObject &args);
    QJsonValue opSelectEntities(const QJsonObject &args);
    QJsonValue opEntitySelected(const QJsonObject &args);
    QJsonValue opEntityBbox(const QJsonObject &args);
    QJsonValue opGetBbox(const QJsonObject &args);
    QJsonValue opModOffset(const QJsonObject &args);
    QJsonValue opModMirror(const QJsonObject &args);
    QJsonValue opModExplode(const QJsonObject &args);
    QJsonValue opModTrim(const QJsonObject &args);
    QJsonValue opFileInfo(const QJsonObject &args);
    QJsonValue opFileSave(const QJsonObject &args);
    QJsonValue opFileSaveAs(const QJsonObject &args);
    QJsonValue opUndoCheckpoint(const QJsonObject &args);
    QJsonValue opUndo(const QJsonObject &args);
    QJsonValue opRedo(const QJsonObject &args);
    QJsonValue opCmdDim(const QJsonObject &args);
    QJsonValue opCmdHatch(const QJsonObject &args);
    // Modify tools: the rest of RS_Modification.
    QJsonValue opModMove(const QJsonObject &args);
    QJsonValue opModRotate(const QJsonObject &args);
    QJsonValue opModScale(const QJsonObject &args);
    QJsonValue opModMoveRotate(const QJsonObject &args);
    QJsonValue opModRotate2(const QJsonObject &args);
    QJsonValue opModStretch(const QJsonObject &args);
    QJsonValue opModRound(const QJsonObject &args);
    QJsonValue opModBevel(const QJsonObject &args);
    QJsonValue opModCut(const QJsonObject &args);
    QJsonValue opModChangeAttributes(const QJsonObject &args);
    QJsonValue opModRevertDirection(const QJsonObject &args);

    QJsonValue opGetLayerState(const QJsonObject &args);
    QJsonValue opSetLayerState(const QJsonObject &args);
    QJsonValue opRenameLayer(const QJsonObject &args);
    QJsonValue opGetLayerStates(const QJsonObject &args);
    QJsonValue opBlockDefine(const QJsonObject &args);
    QJsonValue opBlockRename(const QJsonObject &args);
    QJsonValue opBlockRemove(const QJsonObject &args);
    QJsonValue opBlockEntities(const QJsonObject &args);
    QJsonValue opEntityLength(const QJsonObject &args);
    QJsonValue opEntityArea(const QJsonObject &args);
    QJsonValue opIntersections(const QJsonObject &args);
    QJsonValue opNearestEntity(const QJsonObject &args);
    QJsonValue opNearestPoint(const QJsonObject &args);
    QJsonValue opPointInside(const QJsonObject &args);
    QJsonValue opEntityId(const QJsonObject &args);
    QJsonValue opFindEntity(const QJsonObject &args);
    // View control, document windows, export (roadmap item 4). Switching
    // and closing documents end the session, so those are in BridgeServer.
    QJsonValue opZoomAuto(const QJsonObject &args);
    QJsonValue opZoomWindow(const QJsonObject &args);
    QJsonValue opZoomIn(const QJsonObject &args);
    QJsonValue opZoomOut(const QJsonObject &args);
    QJsonValue opZoomPan(const QJsonObject &args);
    QJsonValue opZoomPrevious(const QJsonObject &args);
    QJsonValue opZoomPage(const QJsonObject &args);
    QJsonValue opGetView(const QJsonObject &args);
    QJsonValue opSetView(const QJsonObject &args);
    QJsonValue opListDocuments(const QJsonObject &args);
    QJsonValue opExportImage(const QJsonObject &args);
    QJsonValue opExportPdf(const QJsonObject &args);
    void requireView() const;
    QJsonValue zoomBy(const QJsonObject &args, bool out);

    // Interactive prompts, as blocking operations: each one waits until the
    // user answers in LibreCAD (or the optional timeout_ms cancels it), so
    // the server handles nothing else meanwhile.
    QJsonValue opPromptPoint(const QJsonObject &args);
    QJsonValue opPromptSelect(const QJsonObject &args);
    QJsonValue opPromptInt(const QJsonObject &args);
    QJsonValue opPromptReal(const QJsonObject &args);
    QJsonValue opPromptString(const QJsonObject &args);
    //! The "timeout_ms" argument, 0 when absent. A timeout needs the native
    //! layer to cancel the prompt; \a needsView for point and select
    //! prompts, which are cancelled through the graphic view.
    int promptTimeout(const QJsonObject &args, bool needsView) const;

    //! Entities of \a dpiType currently in the drawing.
    int countEntitiesOfType(int dpiType);
    //! Set every entity's selection flag; count of entities touched.
    int selectAll(bool selected);
    [[noreturn]] void nativeUnavailable() const;
    void requireNative() const;
    //! Reads (selection flag, bounding box) need the engine but not the
    //! command widget.
    void requireNativeEntityAccess() const;
    void requireModification() const;
    //! Set the drawing selection to exactly these handles; count selected.
    int selectHandles(const QJsonArray &handles);
    //! True when the entity was removed and only survives for undo. False
    //! without the native layer, which cannot tell (see findings: the
    //! plugin API's getAllEntities() reports undone entities).
    bool isUndone(Plug_Entity *entity) const;
    //! Identities of every entity in the drawing right now.
    QSet<const void *> entityKeys();
    //! Entities not in  before, registered as fresh handles, in the
    //! get_entities row format.
    QJsonArray newEntitiesSince(const QSet<const void *> &before);
    //! Forget the handles in  handles (the entities were replaced).
    void invalidateHandles(const QJsonArray &handles);
    //! Register \a entity under a fresh handle and describe it in the
    //! get_entities row format ({handle, type, data}). Takes ownership.
    QJsonObject entityRow(Plug_Entity *entity);
    //! The entity behind the handle in args[\a name] (lookupEntity() reads
    //! "handle"; the two-entity modify ops name theirs).
    Plug_Entity *lookupNamedEntity(const QJsonObject &args, const QString &name) const;
    //! Before replacing the entities behind \a handles: give the operation
    //! its own undo step if any was created in the current one (see
    //! NativeBridge::isolateReplacement).
    void isolateReplacement(const QJsonArray &handles);
    //! Shared tail of the selection transforms (move, rotate, ...): select
    //! "handles", run \a apply, forget the handles when the originals were
    //! replaced (\a copies == 0), and return the new entities.
    template <typename Apply>
    QJsonValue runSelectionTransform(const QJsonObject &args, int copies,
                                     Apply apply);

    Document_Interface *m_doc {nullptr};
    NativeBridge *m_native {nullptr};
    QHash<int, Plug_Entity *> m_entities;
    int m_nextHandle {1};
    int m_batchDepth {0};
};

} // namespace lcbridge

#endif // LC_BRIDGE_DISPATCH_H
