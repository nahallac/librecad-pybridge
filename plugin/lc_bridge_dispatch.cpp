/*****************************************************************************/
/*  lc_bridge_dispatch.cpp - named operations over Document_Interface        */
/*                                                                           */
/*  Licensed under the GNU General Public License, version 2 or later.       */
/*****************************************************************************/

#include "lc_bridge_dispatch.h"

#include "lc_bridge_native.h"

#include "document_interface.h"

#include <QFileInfo>
#include <QImageWriter>
#include <QJsonArray>
#include <QList>
#include <QPointF>
#include <QVariant>
#include <QtMath>

#include <exception>
#include <functional>
#include <utility>
#include <vector>

namespace lcbridge {
namespace {

// --------------------------------------------------------------------------
// Errors
// --------------------------------------------------------------------------

//! Thrown by argument parsing and by handlers; turned into an error response
//! by Dispatcher::dispatch().
class RequestError : public std::exception
{
public:
    RequestError(QString code, QString message)
        : m_code(std::move(code))
        , m_message(std::move(message))
        , m_what(m_message.toUtf8())
    {}

    const QString &code() const { return m_code; }
    const QString &message() const { return m_message; }
    const char *what() const noexcept override { return m_what.constData(); }

private:
    QString m_code;
    QString m_message;
    QByteArray m_what;
};

[[noreturn]] void badArgs(const QString &message)
{
    throw RequestError(QStringLiteral("bad_args"), message);
}

// --------------------------------------------------------------------------
// Entity type names
// --------------------------------------------------------------------------

// The field is a plain int, not DPI::ETYPE: namespace DPI declares both an
// enum type named ETYPE and an EDATA enumerator named ETYPE, and the
// enumerator wins ordinary name lookup. Spelling the type requires the
// elaborated form "enum DPI::ETYPE", which is why Document_Interface itself
// writes it that way.
struct TypeName {
    int type;
    const char *name;
};

// Covers every value of DPI::ETYPE. Types LibreCAD cannot create through the
// plugin API still appear, because getAllEntities() can return them.
const TypeName kTypeNames[] = {
    {DPI::POINT,            "POINT"},
    {DPI::LINE,             "LINE"},
    {DPI::CONSTRUCTIONLINE, "CONSTRUCTIONLINE"},
    {DPI::CIRCLE,           "CIRCLE"},
    {DPI::ARC,              "ARC"},
    {DPI::ELLIPSE,          "ELLIPSE"},
    {DPI::IMAGE,            "IMAGE"},
    {DPI::OVERLAYBOX,       "OVERLAYBOX"},
    {DPI::SOLID,            "SOLID"},
    {DPI::MTEXT,            "MTEXT"},
    {DPI::TEXT,             "TEXT"},
    {DPI::INSERT,           "INSERT"},
    {DPI::POLYLINE,         "POLYLINE"},
    {DPI::SPLINE,           "SPLINE"},
    {DPI::SPLINEPOINTS,     "SPLINEPOINTS"},
    {DPI::HATCH,            "HATCH"},
    {DPI::DIMLEADER,        "DIMLEADER"},
    {DPI::DIMALIGNED,       "DIMALIGNED"},
    {DPI::DIMLINEAR,        "DIMLINEAR"},
    {DPI::DIMRADIAL,        "DIMRADIAL"},
    {DPI::DIMDIAMETRIC,     "DIMDIAMETRIC"},
    {DPI::DIMANGULAR,       "DIMANGULAR"},
    {DPI::UNKNOWN,          "UNKNOWN"},
};

QString typeToName(int type)
{
    for (const TypeName &entry : kTypeNames) {
        if (entry.type == type)
            return QString::fromLatin1(entry.name);
    }
    return QStringLiteral("UNKNOWN");
}

//! Reads the DPI::ETYPE out of an entity's attribute hash. See entityType().
int readEntityType(Plug_Entity *entity)
{
    if (!entity)
        return DPI::UNKNOWN;

    QHash<int, QVariant> data;
    entity->getData(&data);

    const auto it = data.constFind(DPI::ETYPE);
    return it == data.constEnd() ? DPI::UNKNOWN : it->toInt();
}

//! Returns false if \a name is not a known entity type.
bool nameToType(const QString &name, int *type)
{
    for (const TypeName &entry : kTypeNames) {
        if (name.compare(QLatin1String(entry.name), Qt::CaseInsensitive) == 0) {
            *type = entry.type;
            return true;
        }
    }
    return false;
}

// --------------------------------------------------------------------------
// Entity attributes
// --------------------------------------------------------------------------

// Plug_Entity::getData() reports attributes as DPI::EDATA integer keys, and
// those keys are not unique across entity types: CLOSEPOLY and COLCOUNT are
// both 70, STARTZ and ENDZ are both 30. So names are mapped per entity type
// rather than through one global table.
//
// Writability follows Plugin_Entity::updateData(), which ignores keys it does
// not handle for the entity's type. EID and VISIBLE are read-only there.

struct AttrDef {
    int key;
    const char *name;
    enum Kind { Double, Int, Str, Bool } kind;
    bool writable;
};

const AttrDef kCommonAttrs[] = {
    {DPI::EID,     "id",         AttrDef::Int,  false},
    {DPI::LAYER,   "layer",      AttrDef::Str,  true},
    {DPI::LTYPE,   "linetype",   AttrDef::Str,  true},
    {DPI::LWIDTH,  "lineweight", AttrDef::Str,  true},
    {DPI::COLOR,   "color",      AttrDef::Int,  true},
    {DPI::VISIBLE, "visible",    AttrDef::Bool, false},
};

const AttrDef kPointAttrs[] = {
    {DPI::STARTX, "x", AttrDef::Double, true},
    {DPI::STARTY, "y", AttrDef::Double, true},
};

const AttrDef kLineAttrs[] = {
    {DPI::STARTX, "start_x", AttrDef::Double, true},
    {DPI::STARTY, "start_y", AttrDef::Double, true},
    {DPI::ENDX,   "end_x",   AttrDef::Double, true},
    {DPI::ENDY,   "end_y",   AttrDef::Double, true},
};

const AttrDef kCircleAttrs[] = {
    {DPI::STARTX, "center_x", AttrDef::Double, true},
    {DPI::STARTY, "center_y", AttrDef::Double, true},
    {DPI::RADIUS, "radius",   AttrDef::Double, true},
};

const AttrDef kArcAttrs[] = {
    {DPI::STARTX,     "center_x",    AttrDef::Double, true},
    {DPI::STARTY,     "center_y",    AttrDef::Double, true},
    {DPI::RADIUS,     "radius",      AttrDef::Double, true},
    {DPI::STARTANGLE, "start_angle", AttrDef::Double, true},
    {DPI::ENDANGLE,   "end_angle",   AttrDef::Double, true},
    {DPI::REVERSED,   "reversed",    AttrDef::Bool,   false},
};

const AttrDef kEllipseAttrs[] = {
    {DPI::STARTX,     "center_x",    AttrDef::Double, true},
    {DPI::STARTY,     "center_y",    AttrDef::Double, true},
    {DPI::ENDX,       "major_x",     AttrDef::Double, true},
    {DPI::ENDY,       "major_y",     AttrDef::Double, true},
    {DPI::HEIGHT,     "ratio",       AttrDef::Double, true},
    {DPI::STARTANGLE, "start_angle", AttrDef::Double, true},
    {DPI::ENDANGLE,   "end_angle",   AttrDef::Double, true},
    {DPI::REVERSED,   "reversed",    AttrDef::Bool,   false},
};

const AttrDef kTextAttrs[] = {
    {DPI::STARTX,      "x",      AttrDef::Double, true},
    {DPI::STARTY,      "y",      AttrDef::Double, true},
    {DPI::STARTANGLE,  "angle",  AttrDef::Double, true},
    {DPI::HEIGHT,      "height", AttrDef::Double, true},
    {DPI::TEXTCONTENT, "text",   AttrDef::Str,    true},
};

const AttrDef kInsertAttrs[] = {
    {DPI::STARTX,     "x",       AttrDef::Double, true},
    {DPI::STARTY,     "y",       AttrDef::Double, true},
    {DPI::BLKNAME,    "block",   AttrDef::Str,    false},
    {DPI::STARTANGLE, "angle",   AttrDef::Double, true},
    {DPI::XSCALE,     "scale_x", AttrDef::Double, true},
    {DPI::YSCALE,     "scale_y", AttrDef::Double, true},
};

const AttrDef kPolylineAttrs[] = {
    {DPI::CLOSEPOLY, "closed", AttrDef::Bool, true},
};

const AttrDef kImageAttrs[] = {
    {DPI::STARTX,   "x",      AttrDef::Double, true},
    {DPI::STARTY,   "y",      AttrDef::Double, true},
    {DPI::ENDX,     "u_x",    AttrDef::Double, true},
    {DPI::ENDY,     "u_y",    AttrDef::Double, true},
    {DPI::VVECTORX, "v_x",    AttrDef::Double, true},
    {DPI::VVECTORY, "v_y",    AttrDef::Double, true},
    {DPI::SIZEU,    "size_u", AttrDef::Double, true},
    {DPI::SIZEV,    "size_v", AttrDef::Double, true},
    {DPI::BLKNAME,  "file",   AttrDef::Str,    false},
};

//! Attributes specific to \a type, or an empty range for types whose getData()
//! reports nothing beyond the common attributes.
template <size_t N>
constexpr std::pair<const AttrDef *, size_t> span(const AttrDef (&table)[N])
{
    return {table, N};
}

std::pair<const AttrDef *, size_t> typeAttrs(int type)
{
    switch (type) {
    case DPI::POINT:    return span(kPointAttrs);
    case DPI::LINE:     return span(kLineAttrs);
    case DPI::CIRCLE:   return span(kCircleAttrs);
    case DPI::ARC:      return span(kArcAttrs);
    case DPI::ELLIPSE:  return span(kEllipseAttrs);
    case DPI::TEXT:
    case DPI::MTEXT:    return span(kTextAttrs);
    case DPI::INSERT:   return span(kInsertAttrs);
    case DPI::POLYLINE: return span(kPolylineAttrs);
    case DPI::IMAGE:    return span(kImageAttrs);
    default:            return {nullptr, 0};
    }
}

QJsonValue variantToJson(const QVariant &value, AttrDef::Kind kind)
{
    switch (kind) {
    case AttrDef::Double: return value.toDouble();
    case AttrDef::Int:    return static_cast<double>(value.toLongLong());
    case AttrDef::Str:    return value.toString();
    case AttrDef::Bool:   return value.toBool();
    }
    return QJsonValue();
}

//! Converts one entity's attribute hash into named JSON fields. Keys absent
//! from \a data are left out rather than reported as null.
QJsonObject entityDataToJson(int type, const QHash<int, QVariant> &data)
{
    QJsonObject out;

    const auto appendAttr = [&](const AttrDef &attr) {
        const auto it = data.constFind(attr.key);
        if (it != data.constEnd())
            out.insert(QString::fromLatin1(attr.name), variantToJson(*it, attr.kind));
    };

    for (const AttrDef &attr : kCommonAttrs)
        appendAttr(attr);

    const auto attrs = typeAttrs(type);
    for (size_t i = 0; i < attrs.second; ++i)
        appendAttr(attrs.first[i]);

    return out;
}

//! Inverse of entityDataToJson() for the writable attributes. Unknown or
//! read-only names are rejected so that a typo is not silently ignored.
QHash<int, QVariant> jsonToEntityData(int type, const QJsonObject &fields)
{
    QHash<int, QVariant> data;

    const auto attrs = typeAttrs(type);

    for (auto it = fields.constBegin(); it != fields.constEnd(); ++it) {
        const AttrDef *match = nullptr;

        for (const AttrDef &attr : kCommonAttrs) {
            if (it.key() == QLatin1String(attr.name)) {
                match = &attr;
                break;
            }
        }
        for (size_t i = 0; !match && i < attrs.second; ++i) {
            if (it.key() == QLatin1String(attrs.first[i].name))
                match = &attrs.first[i];
        }

        if (!match) {
            badArgs(QStringLiteral("unknown attribute \"%1\" for a %2")
                        .arg(it.key(), typeToName(type)));
        }
        if (!match->writable) {
            badArgs(QStringLiteral("attribute \"%1\" is read-only")
                        .arg(it.key()));
        }

        switch (match->kind) {
        case AttrDef::Double: data.insert(match->key, it.value().toDouble()); break;
        case AttrDef::Int:    data.insert(match->key, it.value().toInt()); break;
        case AttrDef::Str:    data.insert(match->key, it.value().toString()); break;
        case AttrDef::Bool:   data.insert(match->key, it.value().toBool()); break;
        }
    }

    return data;
}

// --------------------------------------------------------------------------
// Argument parsing
// --------------------------------------------------------------------------

QJsonValue requireValue(const QJsonObject &args, const QString &name)
{
    const auto it = args.constFind(name);
    if (it == args.constEnd())
        badArgs(QStringLiteral("missing required argument \"%1\"").arg(name));
    return *it;
}

double requireNumber(const QJsonObject &args, const QString &name)
{
    const QJsonValue value = requireValue(args, name);
    if (!value.isDouble())
        badArgs(QStringLiteral("\"%1\" must be a number").arg(name));
    return value.toDouble();
}

double optionalNumber(const QJsonObject &args, const QString &name, double fallback)
{
    return args.contains(name) ? requireNumber(args, name) : fallback;
}

QString requireString(const QJsonObject &args, const QString &name)
{
    const QJsonValue value = requireValue(args, name);
    if (!value.isString())
        badArgs(QStringLiteral("\"%1\" must be a string").arg(name));
    return value.toString();
}

QString optionalString(const QJsonObject &args, const QString &name,
                       const QString &fallback)
{
    return args.contains(name) ? requireString(args, name) : fallback;
}

bool optionalBool(const QJsonObject &args, const QString &name, bool fallback)
{
    if (!args.contains(name))
        return fallback;
    const QJsonValue value = args.value(name);
    if (!value.isBool())
        badArgs(QStringLiteral("\"%1\" must be a boolean").arg(name));
    return value.toBool();
}

QJsonObject optionalObject(const QJsonObject &args, const QString &name)
{
    if (!args.contains(name))
        return QJsonObject();
    const QJsonValue value = args.value(name);
    if (!value.isObject())
        badArgs(QStringLiteral("\"%1\" must be an object").arg(name));
    return value.toObject();
}

QPointF pointFromJson(const QJsonValue &value, const QString &context)
{
    const QJsonArray array = value.toArray();
    if (!value.isArray() || array.size() != 2
        || !array.at(0).isDouble() || !array.at(1).isDouble()) {
        badArgs(QStringLiteral("%1 must be a point, [x, y]").arg(context));
    }
    return QPointF(array.at(0).toDouble(), array.at(1).toDouble());
}

QPointF requirePoint(const QJsonObject &args, const QString &name)
{
    return pointFromJson(requireValue(args, name), QStringLiteral("\"%1\"").arg(name));
}

QJsonArray requireArray(const QJsonObject &args, const QString &name, int minimum)
{
    const QJsonValue value = requireValue(args, name);
    if (!value.isArray())
        badArgs(QStringLiteral("\"%1\" must be an array").arg(name));
    const QJsonArray array = value.toArray();
    if (array.size() < minimum) {
        badArgs(QStringLiteral("\"%1\" needs at least %2 entries, got %3")
                    .arg(name).arg(minimum).arg(array.size()));
    }
    return array;
}

std::vector<QPointF> requirePoints(const QJsonObject &args, const QString &name,
                                   int minimum)
{
    const QJsonArray array = requireArray(args, name, minimum);

    std::vector<QPointF> points;
    points.reserve(static_cast<size_t>(array.size()));
    for (int i = 0; i < array.size(); ++i) {
        points.push_back(pointFromJson(
            array.at(i), QStringLiteral("\"%1\"[%2]").arg(name).arg(i)));
    }
    return points;
}

//! Vertices are [x, y] or [x, y, bulge]; a missing bulge is 0.
std::vector<Plug_VertexData> requireVertices(const QJsonObject &args,
                                             const QString &name, int minimum)
{
    const QJsonArray array = requireArray(args, name, minimum);

    std::vector<Plug_VertexData> vertices;
    vertices.reserve(static_cast<size_t>(array.size()));
    for (int i = 0; i < array.size(); ++i) {
        const QJsonArray vertex = array.at(i).toArray();
        if (!array.at(i).isArray() || vertex.size() < 2 || vertex.size() > 3
            || !vertex.at(0).isDouble() || !vertex.at(1).isDouble()
            || (vertex.size() == 3 && !vertex.at(2).isDouble())) {
            badArgs(QStringLiteral("\"%1\"[%2] must be [x, y] or [x, y, bulge]")
                        .arg(name).arg(i));
        }
        vertices.emplace_back(QPointF(vertex.at(0).toDouble(), vertex.at(1).toDouble()),
                              vertex.size() == 3 ? vertex.at(2).toDouble() : 0.0);
    }
    return vertices;
}

DPI::Disposition dispositionFromJson(const QJsonObject &args)
{
    return optionalBool(args, QStringLiteral("keep_original"), false)
               ? DPI::KEEP_ORIGINAL
               : DPI::DELETE_ORIGINAL;
}

DPI::HAlign halignFromJson(const QJsonObject &args)
{
    const QString name = optionalString(args, QStringLiteral("halign"),
                                        QStringLiteral("left")).toLower();
    if (name == QLatin1String("left"))   return DPI::HAlignLeft;
    if (name == QLatin1String("center")) return DPI::HAlignCenter;
    if (name == QLatin1String("right"))  return DPI::HAlignRight;
    badArgs(QStringLiteral("\"halign\" must be left, center, or right"));
}

DPI::VAlign valignFromJson(const QJsonObject &args)
{
    const QString name = optionalString(args, QStringLiteral("valign"),
                                        QStringLiteral("bottom")).toLower();
    if (name == QLatin1String("top"))    return DPI::VAlignTop;
    if (name == QLatin1String("middle")) return DPI::VAlignMiddle;
    if (name == QLatin1String("bottom")) return DPI::VAlignBottom;
    badArgs(QStringLiteral("\"valign\" must be top, middle, or bottom"));
}

QJsonArray toJsonArray(const QStringList &values)
{
    QJsonArray array;
    for (const QString &value : values)
        array.append(value);
    return array;
}

} // namespace

int entityType(Plug_Entity *entity)
{
    return readEntityType(entity);
}

QString entityTypeName(int type)
{
    return typeToName(type);
}

// --------------------------------------------------------------------------
// Dispatcher
// --------------------------------------------------------------------------

Dispatcher::Dispatcher(Document_Interface *doc, NativeBridge *native)
    : m_doc(doc)
    , m_native(native)
{}

Dispatcher::~Dispatcher()
{
    // Plug_Entity wrappers handed out by getAllEntities() are caller-owned and
    // do not own the underlying RS_Entity, so deleting them here is correct and
    // leaves the drawing alone.
    qDeleteAll(m_entities);
}

const QHash<QString, Dispatcher::Handler> &Dispatcher::handlers()
{
    static const QHash<QString, Handler> table{
        {QStringLiteral("ping"),                 &Dispatcher::opPing},
        {QStringLiteral("operations"),           &Dispatcher::opOperations},
        {QStringLiteral("batch"),                &Dispatcher::opBatch},
        {QStringLiteral("update_view"),          &Dispatcher::opUpdateView},

        {QStringLiteral("add_point"),            &Dispatcher::opAddPoint},
        {QStringLiteral("add_line"),             &Dispatcher::opAddLine},
        {QStringLiteral("add_lines"),            &Dispatcher::opAddLines},
        {QStringLiteral("add_polyline"),         &Dispatcher::opAddPolyline},
        {QStringLiteral("add_spline_points"),    &Dispatcher::opAddSplinePoints},
        {QStringLiteral("add_circle"),           &Dispatcher::opAddCircle},
        {QStringLiteral("add_arc"),              &Dispatcher::opAddArc},
        {QStringLiteral("add_ellipse"),          &Dispatcher::opAddEllipse},
        {QStringLiteral("add_text"),             &Dispatcher::opAddText},
        {QStringLiteral("add_insert"),           &Dispatcher::opAddInsert},
        {QStringLiteral("add_block_from_file"),  &Dispatcher::opAddBlockFromFile},

        {QStringLiteral("get_current_layer"),    &Dispatcher::opGetCurrentLayer},
        {QStringLiteral("set_layer"),            &Dispatcher::opSetLayer},
        {QStringLiteral("get_layers"),           &Dispatcher::opGetLayers},
        {QStringLiteral("delete_layer"),         &Dispatcher::opDeleteLayer},
        {QStringLiteral("get_layer_properties"), &Dispatcher::opGetLayerProperties},
        {QStringLiteral("set_layer_properties"), &Dispatcher::opSetLayerProperties},

        {QStringLiteral("get_blocks"),           &Dispatcher::opGetBlocks},

        {QStringLiteral("get_entities"),         &Dispatcher::opGetEntities},
        {QStringLiteral("release_handles"),      &Dispatcher::opReleaseHandles},
        {QStringLiteral("unselect"),             &Dispatcher::opUnselect},

        {QStringLiteral("entity_data"),          &Dispatcher::opEntityData},
        {QStringLiteral("entity_update"),        &Dispatcher::opEntityUpdate},
        {QStringLiteral("entity_polyline"),      &Dispatcher::opEntityPolyline},
        {QStringLiteral("entity_set_polyline"),  &Dispatcher::opEntitySetPolyline},
        {QStringLiteral("entity_move"),          &Dispatcher::opEntityMove},
        {QStringLiteral("entity_rotate"),        &Dispatcher::opEntityRotate},
        {QStringLiteral("entity_move_rotate"),   &Dispatcher::opEntityMoveRotate},
        {QStringLiteral("entity_scale"),         &Dispatcher::opEntityScale},
        {QStringLiteral("entity_remove"),        &Dispatcher::opEntityRemove},

        {QStringLiteral("get_variable"),         &Dispatcher::opGetVariable},
        {QStringLiteral("set_variable"),         &Dispatcher::opSetVariable},

        {QStringLiteral("real_to_string"),       &Dispatcher::opRealToString},

        {QStringLiteral("native_status"),        &Dispatcher::opNativeStatus},
        {QStringLiteral("exec_command"),         &Dispatcher::opExecCommand},
        {QStringLiteral("select_entities"),      &Dispatcher::opSelectEntities},
        {QStringLiteral("entity_selected"),      &Dispatcher::opEntitySelected},
        {QStringLiteral("entity_bbox"),          &Dispatcher::opEntityBbox},
        {QStringLiteral("get_bbox"),             &Dispatcher::opGetBbox},
        {QStringLiteral("mod_offset"),           &Dispatcher::opModOffset},
        {QStringLiteral("mod_mirror"),           &Dispatcher::opModMirror},
        {QStringLiteral("mod_explode"),          &Dispatcher::opModExplode},
        {QStringLiteral("mod_trim"),             &Dispatcher::opModTrim},
        {QStringLiteral("file_info"),            &Dispatcher::opFileInfo},
        {QStringLiteral("file_save"),            &Dispatcher::opFileSave},
        {QStringLiteral("file_save_as"),         &Dispatcher::opFileSaveAs},
        {QStringLiteral("undo_checkpoint"),      &Dispatcher::opUndoCheckpoint},
        {QStringLiteral("undo"),                 &Dispatcher::opUndo},
        {QStringLiteral("redo"),                 &Dispatcher::opRedo},
        {QStringLiteral("cmd_dim"),              &Dispatcher::opCmdDim},
        {QStringLiteral("cmd_hatch"),            &Dispatcher::opCmdHatch},
        {QStringLiteral("get_layer_state"), &Dispatcher::opGetLayerState},
        {QStringLiteral("set_layer_state"), &Dispatcher::opSetLayerState},
        {QStringLiteral("rename_layer"), &Dispatcher::opRenameLayer},
        {QStringLiteral("get_layer_states"), &Dispatcher::opGetLayerStates},
        {QStringLiteral("block_define"), &Dispatcher::opBlockDefine},
        {QStringLiteral("block_rename"), &Dispatcher::opBlockRename},
        {QStringLiteral("block_remove"), &Dispatcher::opBlockRemove},
        {QStringLiteral("block_entities"), &Dispatcher::opBlockEntities},
        {QStringLiteral("entity_length"), &Dispatcher::opEntityLength},
        {QStringLiteral("entity_area"), &Dispatcher::opEntityArea},
        {QStringLiteral("intersections"), &Dispatcher::opIntersections},
        {QStringLiteral("nearest_entity"), &Dispatcher::opNearestEntity},
        {QStringLiteral("nearest_point"), &Dispatcher::opNearestPoint},
        {QStringLiteral("point_inside"), &Dispatcher::opPointInside},
        {QStringLiteral("entity_id"), &Dispatcher::opEntityId},
        {QStringLiteral("find_entity"), &Dispatcher::opFindEntity},
        {QStringLiteral("zoom_auto"),            &Dispatcher::opZoomAuto},
        {QStringLiteral("zoom_window"),          &Dispatcher::opZoomWindow},
        {QStringLiteral("zoom_in"),              &Dispatcher::opZoomIn},
        {QStringLiteral("zoom_out"),             &Dispatcher::opZoomOut},
        {QStringLiteral("zoom_pan"),             &Dispatcher::opZoomPan},
        {QStringLiteral("zoom_previous"),        &Dispatcher::opZoomPrevious},
        {QStringLiteral("zoom_page"),            &Dispatcher::opZoomPage},
        {QStringLiteral("get_view"),             &Dispatcher::opGetView},
        {QStringLiteral("set_view"),             &Dispatcher::opSetView},
        {QStringLiteral("list_documents"),       &Dispatcher::opListDocuments},
        {QStringLiteral("export_image"),         &Dispatcher::opExportImage},
        {QStringLiteral("export_pdf"),           &Dispatcher::opExportPdf},
        {QStringLiteral("mod_move"),             &Dispatcher::opModMove},
        {QStringLiteral("mod_rotate"),           &Dispatcher::opModRotate},
        {QStringLiteral("mod_scale"),            &Dispatcher::opModScale},
        {QStringLiteral("mod_move_rotate"),      &Dispatcher::opModMoveRotate},
        {QStringLiteral("mod_rotate2"),          &Dispatcher::opModRotate2},
        {QStringLiteral("mod_stretch"),          &Dispatcher::opModStretch},
        {QStringLiteral("mod_round"),            &Dispatcher::opModRound},
        {QStringLiteral("mod_bevel"),            &Dispatcher::opModBevel},
        {QStringLiteral("mod_cut"),              &Dispatcher::opModCut},
        {QStringLiteral("mod_change_attributes"), &Dispatcher::opModChangeAttributes},
        {QStringLiteral("mod_revert_direction"), &Dispatcher::opModRevertDirection},
        {QStringLiteral("prompt_point"),         &Dispatcher::opPromptPoint},
        {QStringLiteral("prompt_select"),        &Dispatcher::opPromptSelect},
        {QStringLiteral("prompt_int"),           &Dispatcher::opPromptInt},
        {QStringLiteral("prompt_real"),          &Dispatcher::opPromptReal},
        {QStringLiteral("prompt_string"),        &Dispatcher::opPromptString},
        {QStringLiteral("add_mtext"),            &Dispatcher::opAddMText},
        {QStringLiteral("add_image"),            &Dispatcher::opAddImage},
        {QStringLiteral("dim_aligned"),          &Dispatcher::opDimAligned},
        {QStringLiteral("dim_linear"),           &Dispatcher::opDimLinear},
        {QStringLiteral("dim_radial"),           &Dispatcher::opDimRadial},
        {QStringLiteral("dim_diametric"),        &Dispatcher::opDimDiametric},
        {QStringLiteral("dim_angular"),          &Dispatcher::opDimAngular},
        {QStringLiteral("dim_leader"),           &Dispatcher::opDimLeader},
        {QStringLiteral("add_hatch"),            &Dispatcher::opAddHatch},
    };
    return table;
}

const QSet<QString> &Dispatcher::readOnlyOperations()
{
    static const QSet<QString> table{
        QStringLiteral("ping"),            QStringLiteral("operations"),
        QStringLiteral("batch"),           QStringLiteral("update_view"),
        QStringLiteral("get_current_layer"), QStringLiteral("get_layers"),
        QStringLiteral("get_layer_properties"), QStringLiteral("get_blocks"),
        QStringLiteral("get_entities"),    QStringLiteral("release_handles"),
        QStringLiteral("unselect"),        QStringLiteral("entity_data"),
        QStringLiteral("entity_polyline"), QStringLiteral("get_variable"),
        QStringLiteral("real_to_string"),  QStringLiteral("native_status"),
        QStringLiteral("select_entities"), QStringLiteral("entity_selected"),
        QStringLiteral("entity_bbox"),     QStringLiteral("get_bbox"),
        QStringLiteral("file_info"),       QStringLiteral("file_save"),
        QStringLiteral("file_save_as"),    QStringLiteral("undo_checkpoint"),
        QStringLiteral("undo"),            QStringLiteral("redo"),
        QStringLiteral("get_layer_state"),
        QStringLiteral("get_layer_states"),
        QStringLiteral("block_entities"),
        QStringLiteral("entity_length"),
        QStringLiteral("entity_area"),
        QStringLiteral("intersections"),
        QStringLiteral("nearest_entity"),
        QStringLiteral("nearest_point"),
        QStringLiteral("point_inside"),
        QStringLiteral("entity_id"),
        QStringLiteral("find_entity"),
        // The view is not the drawing: none of these add to the undo stack.
        QStringLiteral("zoom_auto"),       QStringLiteral("zoom_window"),
        QStringLiteral("zoom_in"),         QStringLiteral("zoom_out"),
        QStringLiteral("zoom_pan"),        QStringLiteral("zoom_previous"),
        QStringLiteral("zoom_page"),       QStringLiteral("get_view"),
        QStringLiteral("set_view"),        QStringLiteral("list_documents"),
        QStringLiteral("export_image"),    QStringLiteral("export_pdf"),
        // Prompts read an answer; whatever the user does meanwhile is
        // LibreCAD's own business, with its own undo handling.
        QStringLiteral("prompt_point"),    QStringLiteral("prompt_select"),
        QStringLiteral("prompt_int"),      QStringLiteral("prompt_real"),
        QStringLiteral("prompt_string"),
    };
    return table;
}

QStringList Dispatcher::operations()
{
    QStringList names = handlers().keys();
    names.sort();
    return names;
}

QJsonObject Dispatcher::dispatch(const QJsonObject &request)
{
    QJsonObject response;
    if (request.contains(QStringLiteral("id")))
        response.insert(QStringLiteral("id"), request.value(QStringLiteral("id")));

    const auto fail = [&response](const QString &code, const QString &message) {
        QJsonObject error;
        error.insert(QStringLiteral("code"), code);
        error.insert(QStringLiteral("message"), message);
        response.insert(QStringLiteral("ok"), false);
        response.insert(QStringLiteral("error"), error);
        return response;
    };

    if (!m_doc) {
        return fail(QStringLiteral("no_document"),
                    QStringLiteral("no document interface available"));
    }

    const QJsonValue opValue = request.value(QStringLiteral("op"));
    if (!opValue.isString()) {
        return fail(QStringLiteral("bad_request"),
                    QStringLiteral("request needs a string \"op\" field"));
    }
    const QString op = opValue.toString();

    const QJsonValue argsValue = request.value(QStringLiteral("args"));
    if (!argsValue.isUndefined() && !argsValue.isNull() && !argsValue.isObject()) {
        return fail(QStringLiteral("bad_request"),
                    QStringLiteral("\"args\" must be an object when present"));
    }
    const QJsonObject args = argsValue.toObject();

    const auto handler = handlers().constFind(op);
    if (handler == handlers().constEnd()) {
        return fail(QStringLiteral("unknown_op"),
                    QStringLiteral("no such operation \"%1\"").arg(op));
    }

    try {
        if (m_native && !readOnlyOperations().contains(op))
            m_native->ensureUndoCycle();
        const Handler function = *handler;
        const QJsonValue result = (this->*function)(args);
        response.insert(QStringLiteral("ok"), true);
        response.insert(QStringLiteral("result"), result);
        return response;
    } catch (const RequestError &error) {
        return fail(error.code(), error.message());
    } catch (const std::exception &error) {
        return fail(QStringLiteral("internal_error"),
                    QString::fromUtf8(error.what()));
    }
}

// --------------------------------------------------------------------------
// Entity handles
// --------------------------------------------------------------------------

int Dispatcher::registerEntity(Plug_Entity *entity)
{
    const int handle = m_nextHandle++;
    m_entities.insert(handle, entity);
    return handle;
}

Plug_Entity *Dispatcher::lookupEntity(const QJsonObject &args) const
{
    const QJsonValue value = requireValue(args, QStringLiteral("handle"));
    if (!value.isDouble())
        badArgs(QStringLiteral("\"handle\" must be a number"));

    const int handle = value.toInt();
    const auto it = m_entities.constFind(handle);
    if (it == m_entities.constEnd()) {
        throw RequestError(QStringLiteral("no_such_handle"),
                           QStringLiteral("no entity handle %1; it may have been "
                                          "released or invalidated by an edit")
                               .arg(handle));
    }
    return *it;
}

void Dispatcher::invalidateEntity(const QJsonObject &args)
{
    const int handle = args.value(QStringLiteral("handle")).toInt();
    delete m_entities.take(handle);
}

// --------------------------------------------------------------------------
// Meta
// --------------------------------------------------------------------------

QJsonValue Dispatcher::opPing(const QJsonObject &args)
{
    Q_UNUSED(args)
    QJsonObject result;
    result.insert(QStringLiteral("protocol"), 1);
    result.insert(QStringLiteral("plugin"), QStringLiteral("lc_pybridge"));
    return result;
}

QJsonValue Dispatcher::opOperations(const QJsonObject &args)
{
    Q_UNUSED(args)
    return toJsonArray(operations());
}

QJsonValue Dispatcher::opBatch(const QJsonObject &args)
{
    // Bulk geometry is the main reason this exists: one message carrying many
    // requests instead of a round trip per entity.
    if (m_batchDepth > 0) {
        throw RequestError(QStringLiteral("bad_request"),
                           QStringLiteral("batch cannot be nested"));
    }

    const QJsonArray requests = requireArray(args, QStringLiteral("requests"), 0);
    const bool stopOnError = optionalBool(args, QStringLiteral("stop_on_error"), true);

    ++m_batchDepth;
    QJsonArray responses;
    for (int i = 0; i < requests.size(); ++i) {
        if (!requests.at(i).isObject()) {
            QJsonObject error;
            error.insert(QStringLiteral("code"), QStringLiteral("bad_request"));
            error.insert(QStringLiteral("message"),
                         QStringLiteral("\"requests\"[%1] must be an object").arg(i));
            QJsonObject response;
            response.insert(QStringLiteral("ok"), false);
            response.insert(QStringLiteral("error"), error);
            responses.append(response);
            if (stopOnError)
                break;
            continue;
        }

        const QJsonObject response = dispatch(requests.at(i).toObject());
        responses.append(response);
        if (stopOnError && !response.value(QStringLiteral("ok")).toBool())
            break;
    }
    --m_batchDepth;

    QJsonObject result;
    result.insert(QStringLiteral("responses"), responses);
    result.insert(QStringLiteral("completed"), responses.size());
    result.insert(QStringLiteral("requested"), requests.size());
    return result;
}

QJsonValue Dispatcher::opUpdateView(const QJsonObject &args)
{
    Q_UNUSED(args)
    m_doc->updateView();
    return QJsonValue();
}

// --------------------------------------------------------------------------
// Creation
// --------------------------------------------------------------------------

QJsonValue Dispatcher::opAddPoint(const QJsonObject &args)
{
    QPointF at = requirePoint(args, QStringLiteral("at"));
    m_doc->addPoint(&at);
    return QJsonValue();
}

QJsonValue Dispatcher::opAddLine(const QJsonObject &args)
{
    QPointF start = requirePoint(args, QStringLiteral("start"));
    QPointF end = requirePoint(args, QStringLiteral("end"));
    m_doc->addLine(&start, &end);
    return QJsonValue();
}

QJsonValue Dispatcher::opAddLines(const QJsonObject &args)
{
    const std::vector<QPointF> points = requirePoints(args, QStringLiteral("points"), 2);
    m_doc->addLines(points, optionalBool(args, QStringLiteral("closed"), false));
    return QJsonValue();
}

QJsonValue Dispatcher::opAddPolyline(const QJsonObject &args)
{
    const std::vector<Plug_VertexData> vertices =
        requireVertices(args, QStringLiteral("vertices"), 2);
    m_doc->addPolyline(vertices, optionalBool(args, QStringLiteral("closed"), false));
    return QJsonValue();
}

QJsonValue Dispatcher::opAddSplinePoints(const QJsonObject &args)
{
    const std::vector<QPointF> points = requirePoints(args, QStringLiteral("points"), 2);
    m_doc->addSplinePoints(points, optionalBool(args, QStringLiteral("closed"), false));
    return QJsonValue();
}

QJsonValue Dispatcher::opAddCircle(const QJsonObject &args)
{
    QPointF center = requirePoint(args, QStringLiteral("center"));
    m_doc->addCircle(&center, requireNumber(args, QStringLiteral("radius")));
    return QJsonValue();
}

QJsonValue Dispatcher::opAddArc(const QJsonObject &args)
{
    QPointF center = requirePoint(args, QStringLiteral("center"));

    // Document_Interface::addArc() is the one call in the API that takes
    // degrees; everything else, including the angles reported back by
    // entity_data for an ARC, is radians. Convert here so the protocol is
    // uniform.
    const double startDegrees = qRadiansToDegrees(
        requireNumber(args, QStringLiteral("start_angle")));
    const double endDegrees = qRadiansToDegrees(
        requireNumber(args, QStringLiteral("end_angle")));

    m_doc->addArc(&center, requireNumber(args, QStringLiteral("radius")),
                  startDegrees, endDegrees);
    return QJsonValue();
}

QJsonValue Dispatcher::opAddEllipse(const QJsonObject &args)
{
    QPointF center = requirePoint(args, QStringLiteral("center"));
    QPointF major = requirePoint(args, QStringLiteral("major"));
    m_doc->addEllipse(&center, &major, requireNumber(args, QStringLiteral("ratio")),
                      optionalNumber(args, QStringLiteral("start_angle"), 0.0),
                      optionalNumber(args, QStringLiteral("end_angle"), 0.0));
    return QJsonValue();
}

QJsonValue Dispatcher::opAddText(const QJsonObject &args)
{
    QPointF at = requirePoint(args, QStringLiteral("at"));
    m_doc->addText(requireString(args, QStringLiteral("text")),
                   optionalString(args, QStringLiteral("style"),
                                  QStringLiteral("standard")),
                   &at, requireNumber(args, QStringLiteral("height")),
                   optionalNumber(args, QStringLiteral("angle"), 0.0),
                   halignFromJson(args), valignFromJson(args));
    return QJsonValue();
}

QJsonValue Dispatcher::opAddInsert(const QJsonObject &args)
{
    const QPointF at = requirePoint(args, QStringLiteral("at"));

    QPointF scale(1.0, 1.0);
    if (args.contains(QStringLiteral("scale")))
        scale = requirePoint(args, QStringLiteral("scale"));

    m_doc->addInsert(requireString(args, QStringLiteral("block")), at, scale,
                     optionalNumber(args, QStringLiteral("angle"), 0.0));
    return QJsonValue();
}

QJsonValue Dispatcher::opAddBlockFromFile(const QJsonObject &args)
{
    // Note: this is the one creation call with no undo section of its own, so
    // a block definition imported this way survives undo.
    const QString name =
        m_doc->addBlockfromFromdisk(requireString(args, QStringLiteral("path")));
    if (name.isEmpty()) {
        throw RequestError(QStringLiteral("failed"),
                           QStringLiteral("could not load a block from \"%1\"")
                               .arg(args.value(QStringLiteral("path")).toString()));
    }
    return name;
}

// --------------------------------------------------------------------------
// Layers and blocks
// --------------------------------------------------------------------------

QJsonValue Dispatcher::opGetCurrentLayer(const QJsonObject &args)
{
    Q_UNUSED(args)
    return m_doc->getCurrentLayer();
}

QJsonValue Dispatcher::opSetLayer(const QJsonObject &args)
{
    // Creates the layer when it does not exist. That creation is not undoable:
    // see docs/findings.md, risk 1.
    m_doc->setLayer(requireString(args, QStringLiteral("name")));
    return QJsonValue();
}

QJsonValue Dispatcher::opGetLayers(const QJsonObject &args)
{
    Q_UNUSED(args)
    return toJsonArray(m_doc->getAllLayer());
}

QJsonValue Dispatcher::opDeleteLayer(const QJsonObject &args)
{
    return m_doc->deleteLayer(requireString(args, QStringLiteral("name")));
}

QJsonValue Dispatcher::opGetLayerProperties(const QJsonObject &args)
{
    Q_UNUSED(args)

    int color = 0;
    QString lineweight;
    QString linetype;
    m_doc->getCurrentLayerProperties(&color, &lineweight, &linetype);

    QJsonObject result;
    result.insert(QStringLiteral("color"), color);
    result.insert(QStringLiteral("lineweight"), lineweight);
    result.insert(QStringLiteral("linetype"), linetype);
    return result;
}

QJsonValue Dispatcher::opSetLayerProperties(const QJsonObject &args)
{
    // The string overload is used so the values round-trip with
    // get_layer_properties, which also reports strings.
    int color = 0;
    QString lineweight;
    QString linetype;
    m_doc->getCurrentLayerProperties(&color, &lineweight, &linetype);

    if (args.contains(QStringLiteral("color")))
        color = static_cast<int>(requireNumber(args, QStringLiteral("color")));
    lineweight = optionalString(args, QStringLiteral("lineweight"), lineweight);
    linetype = optionalString(args, QStringLiteral("linetype"), linetype);

    m_doc->setCurrentLayerProperties(color, lineweight, linetype);
    return QJsonValue();
}

QJsonValue Dispatcher::opGetBlocks(const QJsonObject &args)
{
    Q_UNUSED(args)
    // The plugin API lists blocks that were removed and are only kept for
    // undo; the native layer knows which those are.
    QStringList names;
    if (m_native && m_native->blockNames(&names))
        return toJsonArray(names);
    return toJsonArray(m_doc->getAllBlocks());
}

// --------------------------------------------------------------------------
// Query
// --------------------------------------------------------------------------

QJsonValue Dispatcher::opGetEntities(const QJsonObject &args)
{
    // This is the only non-interactive query in the API. getSelect(),
    // getSelectByType(), and getEnt() all prompt the user and spin a nested
    // event loop; getSelect() is exposed separately as the blocking
    // prompt_select. See docs/findings.md, risk 6.
    QList<int> wanted;
    if (args.contains(QStringLiteral("types"))) {
        const QJsonArray types = requireArray(args, QStringLiteral("types"), 1);
        for (int i = 0; i < types.size(); ++i) {
            int type = DPI::UNKNOWN;
            if (!types.at(i).isString() || !nameToType(types.at(i).toString(), &type)) {
                badArgs(QStringLiteral("\"types\"[%1] is not a known entity type")
                            .arg(i));
            }
            wanted.append(type);
        }
    }

    const bool visibleOnly = optionalBool(args, QStringLiteral("visible_only"), false);
    const bool includeData = optionalBool(args, QStringLiteral("include_data"), true);
    // The selection flag is not in the attribute hash (only VISIBLE is), so
    // filtering on it goes through the native layer; see risk 8.
    const bool selectedOnly = optionalBool(args, QStringLiteral("selected_only"), false);
    if (selectedOnly)
        requireNativeEntityAccess();

    QList<Plug_Entity *> entities;
    if (!m_doc->getAllEntities(&entities, visibleOnly)) {
        throw RequestError(QStringLiteral("failed"),
                           QStringLiteral("getAllEntities() failed"));
    }

    QJsonArray result;
    for (Plug_Entity *entity : entities) {
        if (!entity)
            continue;

        // One getData() call supplies both the type and the reported
        // attributes; the type has to come from the hash rather than from
        // getEntityType(). See entityType() in the header.
        QHash<int, QVariant> data;
        entity->getData(&data);
        const auto typeField = data.constFind(DPI::ETYPE);
        const int type = typeField == data.constEnd() ? DPI::UNKNOWN
                                                      : typeField->toInt();

        if ((!wanted.isEmpty() && !wanted.contains(type)) || isUndone(entity)) {
            delete entity;
            continue;
        }
        if (selectedOnly) {
            bool selected = false;
            if (!m_native->isSelected(entity, &selected) || !selected) {
                delete entity;
                continue;
            }
        }

        QJsonObject item;
        item.insert(QStringLiteral("handle"), registerEntity(entity));
        item.insert(QStringLiteral("type"), typeToName(type));
        if (includeData)
            item.insert(QStringLiteral("data"), rowData(entity, type, data));
        result.append(item);
    }
    return result;
}

QJsonValue Dispatcher::opReleaseHandles(const QJsonObject &args)
{
    int released = 0;

    if (args.contains(QStringLiteral("handles"))) {
        const QJsonArray handles = requireArray(args, QStringLiteral("handles"), 0);
        for (int i = 0; i < handles.size(); ++i) {
            if (!handles.at(i).isDouble())
                badArgs(QStringLiteral("\"handles\"[%1] must be a number").arg(i));
            if (Plug_Entity *entity = m_entities.take(handles.at(i).toInt())) {
                delete entity;
                ++released;
            }
        }
    } else {
        released = m_entities.size();
        qDeleteAll(m_entities);
        m_entities.clear();
    }

    return released;
}

QJsonValue Dispatcher::opUnselect(const QJsonObject &args)
{
    Q_UNUSED(args)
    m_doc->unselectEntities();
    return QJsonValue();
}

// --------------------------------------------------------------------------
// Entity access
// --------------------------------------------------------------------------

QJsonValue Dispatcher::opEntityData(const QJsonObject &args)
{
    Plug_Entity *entity = lookupEntity(args);

    QHash<int, QVariant> data;
    entity->getData(&data);

    const auto typeField = data.constFind(DPI::ETYPE);
    const int type = typeField == data.constEnd() ? DPI::UNKNOWN : typeField->toInt();

    QJsonObject result;
    result.insert(QStringLiteral("type"), typeToName(type));
    result.insert(QStringLiteral("data"), rowData(entity, type, data));
    return result;
}

QJsonValue Dispatcher::opEntityUpdate(const QJsonObject &args)
{
    Plug_Entity *entity = lookupEntity(args);
    const QJsonObject fields = optionalObject(args, QStringLiteral("data"));
    if (fields.isEmpty())
        badArgs(QStringLiteral("\"data\" must name at least one attribute"));

    QHash<int, QVariant> data = jsonToEntityData(readEntityType(entity), fields);
    entity->updateData(&data);

    // Plugin_Entity::updateData() clones the entity, registers the clone, and
    // marks the original undone -- but, unlike move/rotate/scale, it does not
    // re-point the wrapper at the clone. The handle now refers to an entity
    // that is no longer in the drawing, so it is dropped here; re-query with
    // get_entities to get a usable handle.
    invalidateEntity(args);
    return QJsonValue();
}

QJsonValue Dispatcher::opEntityPolyline(const QJsonObject &args)
{
    Plug_Entity *entity = lookupEntity(args);
    const int type = readEntityType(entity);
    if (type != DPI::POLYLINE) {
        badArgs(QStringLiteral("entity is a %1, not a POLYLINE").arg(typeToName(type)));
    }

    QList<Plug_VertexData> vertices;
    entity->getPolylineData(&vertices);

    QJsonArray result;
    for (const Plug_VertexData &vertex : vertices) {
        QJsonArray item;
        item.append(vertex.point.x());
        item.append(vertex.point.y());
        item.append(vertex.bulge);
        result.append(item);
    }
    return result;
}

QJsonValue Dispatcher::opEntitySetPolyline(const QJsonObject &args)
{
    Plug_Entity *entity = lookupEntity(args);
    const int type = readEntityType(entity);
    if (type != DPI::POLYLINE) {
        badArgs(QStringLiteral("entity is a %1, not a POLYLINE").arg(typeToName(type)));
    }

    const std::vector<Plug_VertexData> vertices =
        requireVertices(args, QStringLiteral("vertices"), 2);

    QList<Plug_VertexData> list;
    for (const Plug_VertexData &vertex : vertices)
        list.append(vertex);
    entity->updatePolylineData(&list);

    // updatePolylineData() edits the polyline in place, registering nothing
    // with the undo system and not refreshing the view. Inside execComm() the
    // enclosing undo cycle covers nothing of this edit, so it cannot be undone;
    // prefer deleting the polyline and adding a new one. update_view() is
    // called here because the API will not do it.
    m_doc->updateView();
    return QJsonValue();
}

QJsonValue Dispatcher::opEntityMove(const QJsonObject &args)
{
    // move/rotate/scale/move_rotate re-point the wrapper at the modified
    // clone, so the handle stays valid afterwards.
    lookupEntity(args)->move(requirePoint(args, QStringLiteral("offset")),
                             dispositionFromJson(args));
    return QJsonValue();
}

QJsonValue Dispatcher::opEntityRotate(const QJsonObject &args)
{
    lookupEntity(args)->rotate(requirePoint(args, QStringLiteral("center")),
                               requireNumber(args, QStringLiteral("angle")),
                               dispositionFromJson(args));
    return QJsonValue();
}

QJsonValue Dispatcher::opEntityMoveRotate(const QJsonObject &args)
{
    lookupEntity(args)->moveRotate(requirePoint(args, QStringLiteral("offset")),
                                   requirePoint(args, QStringLiteral("center")),
                                   requireNumber(args, QStringLiteral("angle")),
                                   dispositionFromJson(args));
    return QJsonValue();
}

QJsonValue Dispatcher::opEntityScale(const QJsonObject &args)
{
    lookupEntity(args)->scale(requirePoint(args, QStringLiteral("center")),
                              requirePoint(args, QStringLiteral("factor")),
                              dispositionFromJson(args));
    return QJsonValue();
}

QJsonValue Dispatcher::opEntityRemove(const QJsonObject &args)
{
    Plug_Entity *entity = lookupEntity(args);
    m_doc->removeEntity(entity);
    invalidateEntity(args);
    return QJsonValue();
}

// --------------------------------------------------------------------------
// Drawing variables
// --------------------------------------------------------------------------

QJsonValue Dispatcher::opGetVariable(const QJsonObject &args)
{
    const QString key = requireString(args, QStringLiteral("key"));
    const QString type = optionalString(args, QStringLiteral("type"),
                                        QStringLiteral("double")).toLower();

    if (type == QLatin1String("int")) {
        int value = 0;
        if (!m_doc->getVariableInt(key, &value))
            return QJsonValue();
        return value;
    }
    if (type == QLatin1String("double")) {
        double value = 0.0;
        if (!m_doc->getVariableDouble(key, &value))
            return QJsonValue();
        return value;
    }
    badArgs(QStringLiteral("\"type\" must be int or double"));
}

QJsonValue Dispatcher::opSetVariable(const QJsonObject &args)
{
    const QString key = requireString(args, QStringLiteral("key"));
    const QString type = optionalString(args, QStringLiteral("type"),
                                        QStringLiteral("double")).toLower();
    const double value = requireNumber(args, QStringLiteral("value"));

    // "code" is the DXF group code the variable is stored under; the defaults
    // match Document_Interface's own (70 for ints, 40 for doubles).
    if (type == QLatin1String("int")) {
        const int code = static_cast<int>(
            optionalNumber(args, QStringLiteral("code"), 70));
        return m_doc->addVariable(key, static_cast<int>(value), code);
    }
    if (type == QLatin1String("double")) {
        const int code = static_cast<int>(
            optionalNumber(args, QStringLiteral("code"), 40));
        return m_doc->addVariable(key, value, code);
    }
    badArgs(QStringLiteral("\"type\" must be int or double"));
}

QJsonValue Dispatcher::opRealToString(const QJsonObject &args)
{
    // units: 0 as configured, 1 scientific, 2 decimal, 3 engineering,
    // 4 architectural, 5 fractional, 6 architectural metric.
    return m_doc->realToStr(requireNumber(args, QStringLiteral("value")),
                            static_cast<int>(
                                optionalNumber(args, QStringLiteral("units"), 0)),
                            static_cast<int>(
                                optionalNumber(args, QStringLiteral("precision"), 0)));
}

// --------------------------------------------------------------------------
// Native operations: LibreCAD's command line and selection, from in-process.
// Real dimensions and hatches are made of these -- the plugin API cannot
// create either entity kind, so these operations drive the same actions the
// user would, through the command widget. See lc_bridge_native.h.
// --------------------------------------------------------------------------

void Dispatcher::nativeUnavailable() const
{
    throw RequestError(QStringLiteral("unavailable"),
                       m_native ? QStringLiteral("native access unavailable: %1")
                                      .arg(m_native->reason())
                                : QStringLiteral("no native bridge in this "
                                                 "server (offline stub?)"));
}

void Dispatcher::requireNative() const
{
    if (!m_native || !m_native->commandsAvailable()
        || !m_native->selectionAvailable()) {
        nativeUnavailable();
    }
}

void Dispatcher::requireNativeEntityAccess() const
{
    if (!m_native || !m_native->selectionAvailable())
        nativeUnavailable();
}

void Dispatcher::requireModification() const
{
    if (!m_native || !m_native->modificationAvailable())
        nativeUnavailable();
}

int Dispatcher::selectHandles(const QJsonArray &handles)
{
    selectAll(false);
    int selected = 0;
    for (int i = 0; i < handles.size(); ++i) {
        if (!handles.at(i).isDouble())
            badArgs(QStringLiteral("\"handles\"[%1] must be a number").arg(i));
        QJsonObject lookup;
        lookup.insert(QStringLiteral("handle"), handles.at(i));
        if (m_native->setSelected(lookupEntity(lookup), true))
            ++selected;
    }
    return selected;
}

bool Dispatcher::isUndone(Plug_Entity *entity) const
{
    bool undone = false;
    return m_native && m_native->isUndone(entity, &undone) && undone;
}

QSet<const void *> Dispatcher::entityKeys()
{
    QSet<const void *> keys;
    QList<Plug_Entity *> entities;
    if (m_doc->getAllEntities(&entities, false)) {
        for (Plug_Entity *entity : entities)
            keys.insert(m_native->entityKey(entity));
    }
    qDeleteAll(entities);
    return keys;
}

QJsonArray Dispatcher::newEntitiesSince(const QSet<const void *> &before)
{
    QJsonArray result;
    QList<Plug_Entity *> entities;
    if (!m_doc->getAllEntities(&entities, false))
        return result;
    for (Plug_Entity *entity : entities) {
        if (!entity || before.contains(m_native->entityKey(entity))
            || isUndone(entity)) {
            delete entity;
            continue;
        }
        QHash<int, QVariant> data;
        entity->getData(&data);
        const auto typeField = data.constFind(DPI::ETYPE);
        const int type = typeField == data.constEnd() ? DPI::UNKNOWN
                                                      : typeField->toInt();
        QJsonObject item;
        item.insert(QStringLiteral("handle"), registerEntity(entity));
        item.insert(QStringLiteral("type"), typeToName(type));
        item.insert(QStringLiteral("data"), rowData(entity, type, data));
        result.append(item);
    }
    return result;
}

void Dispatcher::invalidateHandles(const QJsonArray &handles)
{
    for (const QJsonValue &handle : handles) {
        QJsonObject lookup;
        lookup.insert(QStringLiteral("handle"), handle);
        invalidateEntity(lookup);
    }
}

namespace {

QJsonArray pointJson(const QPointF &point)
{
    return QJsonArray{point.x(), point.y()};
}

QJsonObject bboxJson(const QPointF &min, const QPointF &max)
{
    QJsonObject box;
    box.insert(QStringLiteral("min"), pointJson(min));
    box.insert(QStringLiteral("max"), pointJson(max));
    return box;
}

} // namespace

int Dispatcher::countEntitiesOfType(int dpiType)
{
    QList<Plug_Entity *> entities;
    if (!m_doc->getAllEntities(&entities, false))
        return -1;

    int count = 0;
    for (Plug_Entity *entity : entities) {
        if (readEntityType(entity) == dpiType && !isUndone(entity))
            ++count;
    }
    qDeleteAll(entities);
    return count;
}

int Dispatcher::selectAll(bool selected)
{
    QList<Plug_Entity *> entities;
    if (!m_doc->getAllEntities(&entities, false))
        return 0;

    int touched = 0;
    for (Plug_Entity *entity : entities) {
        if (!isUndone(entity) && m_native->setSelected(entity, selected))
            ++touched;
    }
    qDeleteAll(entities);
    return touched;
}

QJsonValue Dispatcher::opNativeStatus(const QJsonObject &args)
{
    Q_UNUSED(args)
    QJsonObject result;
    result.insert(QStringLiteral("commands"),
                  m_native && m_native->commandsAvailable());
    result.insert(QStringLiteral("selection"),
                  m_native && m_native->selectionAvailable());
    result.insert(QStringLiteral("reason"),
                  m_native ? m_native->reason() : QStringLiteral("no native bridge"));
    if (m_native) {
        result.insert(QStringLiteral("built_against"), NativeBridge::builtAgainst());
        result.insert(QStringLiteral("running"), NativeBridge::running());
    }
    return result;
}

QJsonValue Dispatcher::opExecCommand(const QJsonObject &args)
{
    if (!m_native || !m_native->commandsAvailable())
        nativeUnavailable();
    if (!m_native->execCommand(requireString(args, QStringLiteral("command")))) {
        throw RequestError(QStringLiteral("failed"),
                           QStringLiteral("the command widget rejected the call"));
    }
    return QJsonValue();
}

QJsonValue Dispatcher::opSelectEntities(const QJsonObject &args)
{
    requireNative();

    if (optionalBool(args, QStringLiteral("deselect_others"), true))
        selectAll(false);

    int selected = 0;
    const QJsonArray handles = requireArray(args, QStringLiteral("handles"), 0);
    for (int i = 0; i < handles.size(); ++i) {
        if (!handles.at(i).isDouble())
            badArgs(QStringLiteral("\"handles\"[%1] must be a number").arg(i));
        QJsonObject lookup;
        lookup.insert(QStringLiteral("handle"), handles.at(i));
        Plug_Entity *entity = lookupEntity(lookup);
        if (m_native->setSelected(entity, true))
            ++selected;
    }
    return selected;
}

QJsonValue Dispatcher::opEntitySelected(const QJsonObject &args)
{
    requireNativeEntityAccess();
    bool selected = false;
    if (!m_native->isSelected(lookupEntity(args), &selected)) {
        throw RequestError(QStringLiteral("failed"),
                           QStringLiteral("could not read the selection flag"));
    }
    return selected;
}

QJsonValue Dispatcher::opEntityBbox(const QJsonObject &args)
{
    requireNativeEntityAccess();
    QPointF min, max;
    if (!m_native->boundingBox(lookupEntity(args), &min, &max)) {
        throw RequestError(QStringLiteral("failed"),
                           QStringLiteral("the entity has no valid extent"));
    }
    return bboxJson(min, max);
}

QJsonValue Dispatcher::opGetBbox(const QJsonObject &args)
{
    // Union of the boxes of "handles", or of every entity in the drawing when
    // no handles are given. Entities without a valid extent are skipped; null
    // when nothing contributed.
    requireNativeEntityAccess();

    QList<Plug_Entity *> owned;   // fetched here, released before returning
    QList<Plug_Entity *> entities;
    if (args.contains(QStringLiteral("handles"))) {
        const QJsonArray handles = requireArray(args, QStringLiteral("handles"), 0);
        for (int i = 0; i < handles.size(); ++i) {
            if (!handles.at(i).isDouble())
                badArgs(QStringLiteral("\"handles\"[%1] must be a number").arg(i));
            QJsonObject lookup;
            lookup.insert(QStringLiteral("handle"), handles.at(i));
            entities.append(lookupEntity(lookup));
        }
    } else {
        if (!m_doc->getAllEntities(&owned, false)) {
            throw RequestError(QStringLiteral("failed"),
                               QStringLiteral("getAllEntities() failed"));
        }
        entities = owned;
    }

    bool any = false;
    QPointF lo, hi;
    for (Plug_Entity *entity : entities) {
        QPointF min, max;
        if (isUndone(entity) || !m_native->boundingBox(entity, &min, &max))
            continue;
        if (!any) {
            lo = min;
            hi = max;
            any = true;
        } else {
            lo = QPointF(qMin(lo.x(), min.x()), qMin(lo.y(), min.y()));
            hi = QPointF(qMax(hi.x(), max.x()), qMax(hi.y(), max.y()));
        }
    }
    qDeleteAll(owned);
    if (!any)
        return QJsonValue();
    return bboxJson(lo, hi);
}

// Engine modifications. Each returns the entities the operation created, in
// the get_entities row format, found by diffing the drawing's entity
// identities before and after. Handles of entities the engine removed are
// forgotten here so a later use reports "no such handle" rather than acting
// on an undone entity.

QJsonValue Dispatcher::opModOffset(const QJsonObject &args)
{
    requireModification();
    const QJsonArray handles = requireArray(args, QStringLiteral("handles"), 1);
    const double distance = requireNumber(args, QStringLiteral("distance"));
    const QPointF side = requirePoint(args, QStringLiteral("side"));
    const int count = static_cast<int>(optionalNumber(args, QStringLiteral("count"), 1));
    const bool keepOriginal = optionalBool(args, QStringLiteral("keep_original"), true);
    const bool useCurrentLayer = optionalBool(args, QStringLiteral("use_current_layer"), false);
    const bool useCurrentAttributes =
        optionalBool(args, QStringLiteral("use_current_attributes"), false);
    if (count < 1)
        badArgs(QStringLiteral("\"count\" must be at least 1"));

    if (!keepOriginal)
        isolateReplacement(handles);
    const QSet<const void *> before = entityKeys();
    selectHandles(handles);
    if (!m_native->offset(side, distance, count, keepOriginal,
                          useCurrentLayer, useCurrentAttributes)) {
        throw RequestError(QStringLiteral("failed"), m_native->lastError());
    }
    if (!keepOriginal)
        invalidateHandles(handles);
    return newEntitiesSince(before);
}

QJsonValue Dispatcher::opModMirror(const QJsonObject &args)
{
    requireModification();
    const QJsonArray handles = requireArray(args, QStringLiteral("handles"), 1);
    const QPointF p1 = requirePoint(args, QStringLiteral("axis_p1"));
    const QPointF p2 = requirePoint(args, QStringLiteral("axis_p2"));
    const bool copy = optionalBool(args, QStringLiteral("copy"), false);
    if (p1 == p2)
        badArgs(QStringLiteral("the mirror axis needs two distinct points"));

    if (!copy)
        isolateReplacement(handles);
    const QSet<const void *> before = entityKeys();
    selectHandles(handles);
    if (!m_native->mirror(p1, p2, copy))
        throw RequestError(QStringLiteral("failed"), m_native->lastError());
    if (!copy)
        invalidateHandles(handles);
    return newEntitiesSince(before);
}

QJsonValue Dispatcher::opModExplode(const QJsonObject &args)
{
    requireModification();
    const QJsonArray handles = requireArray(args, QStringLiteral("handles"), 1);
    const bool remove = optionalBool(args, QStringLiteral("remove"), true);

    if (remove)
        isolateReplacement(handles);
    const QSet<const void *> before = entityKeys();
    selectHandles(handles);
    if (!m_native->explode(remove))
        throw RequestError(QStringLiteral("failed"), m_native->lastError());
    if (remove)
        invalidateHandles(handles);
    return newEntitiesSince(before);
}

QJsonValue Dispatcher::opModTrim(const QJsonObject &args)
{
    requireModification();
    Plug_Entity *toTrim = lookupEntity(args);
    const QPointF trimPoint = requirePoint(args, QStringLiteral("trim_point"));
    QJsonObject limitLookup;
    limitLookup.insert(QStringLiteral("handle"),
                       requireValue(args, QStringLiteral("limit_handle")));
    Plug_Entity *limit = lookupEntity(limitLookup);
    const QPointF limitPoint = requirePoint(args, QStringLiteral("limit_point"));
    const bool both = optionalBool(args, QStringLiteral("both"), false);

    m_native->isolateReplacement(both ? QList<Plug_Entity *>{toTrim, limit}
                                      : QList<Plug_Entity *>{toTrim});
    const QSet<const void *> before = entityKeys();
    if (!m_native->trim(toTrim, trimPoint, limit, limitPoint, both))
        throw RequestError(QStringLiteral("failed"), m_native->lastError());
    // trim() replaces the trimmed entity with a clone (and the limit entity
    // too when both); the handles now point at undone entities.
    QJsonArray replaced{args.value(QStringLiteral("handle"))};
    if (both)
        replaced.append(args.value(QStringLiteral("limit_handle")));
    invalidateHandles(replaced);
    return newEntitiesSince(before);
}

// Document file state and undo cycles. file_open and file_new are not here:
// they end the session (see BridgeServer).

QJsonValue Dispatcher::opFileInfo(const QJsonObject &args)
{
    Q_UNUSED(args)
    requireModification();
    QString path;
    bool modified = false;
    m_native->fileInfo(&path, &modified);
    QJsonObject result;
    result.insert(QStringLiteral("path"), path);
    result.insert(QStringLiteral("modified"), modified);
    return result;
}

QJsonValue Dispatcher::opFileSave(const QJsonObject &args)
{
    Q_UNUSED(args)
    requireModification();
    QString path;
    bool modified = false;
    m_native->fileInfo(&path, &modified);
    if (path.isEmpty()) {
        throw RequestError(QStringLiteral("no_filename"),
                           QStringLiteral("the drawing has no file name yet; "
                                          "use file_save_as"));
    }
    if (!m_native->saveAs(path, QString()))
        throw RequestError(QStringLiteral("failed"), m_native->lastError());
    return opFileInfo(args);
}

QJsonValue Dispatcher::opFileSaveAs(const QJsonObject &args)
{
    requireModification();
    const QString path = requireString(args, QStringLiteral("path"));
    const QString format = optionalString(args, QStringLiteral("format"), QString());
    if (!m_native->saveAs(path, format))
        throw RequestError(QStringLiteral("failed"), m_native->lastError());
    return opFileInfo(args);
}

QJsonValue Dispatcher::opUndoCheckpoint(const QJsonObject &args)
{
    Q_UNUSED(args)
    requireModification();
    m_native->undoCheckpoint();
    return QJsonValue();
}

QJsonValue Dispatcher::opUndo(const QJsonObject &args)
{
    requireModification();
    const int steps = static_cast<int>(optionalNumber(args, QStringLiteral("steps"), 1));
    if (steps < 1)
        badArgs(QStringLiteral("\"steps\" must be at least 1"));
    int done = 0;
    m_native->undo(steps, false, &done);
    m_doc->updateView();
    return done;
}

QJsonValue Dispatcher::opRedo(const QJsonObject &args)
{
    requireModification();
    const int steps = static_cast<int>(optionalNumber(args, QStringLiteral("steps"), 1));
    if (steps < 1)
        badArgs(QStringLiteral("\"steps\" must be at least 1"));
    int done = 0;
    m_native->undo(steps, true, &done);
    m_doc->updateView();
    return done;
}

QJsonValue Dispatcher::opCmdDim(const QJsonObject &args)
{
    requireNative();

    const QString kind = requireString(args, QStringLiteral("kind")).toLower();
    QString command;
    int resultType = DPI::DIMALIGNED;
    if (kind == QLatin1String("aligned")) {
        command = QStringLiteral("dimaligned");
        resultType = DPI::DIMALIGNED;
    } else if (kind == QLatin1String("linear")) {
        command = QStringLiteral("dimlinear");
        resultType = DPI::DIMLINEAR;
    } else if (kind == QLatin1String("horizontal")) {
        command = QStringLiteral("dimhorizontal");
        resultType = DPI::DIMLINEAR;
    } else if (kind == QLatin1String("vertical")) {
        command = QStringLiteral("dimvertical");
        resultType = DPI::DIMLINEAR;
    } else {
        badArgs(QStringLiteral("\"kind\" must be aligned, linear, horizontal, "
                               "or vertical"));
    }

    const QPointF p1 = requirePoint(args, QStringLiteral("p1"));
    const QPointF p2 = requirePoint(args, QStringLiteral("p2"));
    const QPointF dimLine = requirePoint(args, QStringLiteral("dimline"));

    const auto coordinate = [](const QPointF &point) {
        return QStringLiteral("%1,%2")
            .arg(QString::number(point.x(), 'f', 10),
                 QString::number(point.y(), 'f', 10));
    };

    const int before = countEntitiesOfType(resultType);

    // The dimension actions take exactly the three points the GUI asks for:
    // two extension line origins, then the dimension line location. The
    // leading escapes clear any pending action; the trailing one ends the
    // action's loop (it restarts at the first point after each dimension).
    m_native->execCommand(QStringLiteral("escape"));
    m_native->execCommand(QStringLiteral("escape"));
    m_native->execCommand(command);
    m_native->execCommand(coordinate(p1));
    m_native->execCommand(coordinate(p2));
    m_native->execCommand(coordinate(dimLine));
    m_native->execCommand(QStringLiteral("escape"));

    const int after = countEntitiesOfType(resultType);
    if (before < 0 || after != before + 1) {
        throw RequestError(QStringLiteral("failed"),
                           QStringLiteral("the %1 action did not create a "
                                          "dimension (entity count %2 -> %3)")
                               .arg(command).arg(before).arg(after));
    }
    m_doc->updateView();
    return QJsonValue();
}

QJsonValue Dispatcher::opCmdHatch(const QJsonObject &args)
{
    requireNative();

    const double angleDegrees =
        qRadiansToDegrees(optionalNumber(args, QStringLiteral("angle"), 0.0));
    const QString pattern = optionalString(args, QStringLiteral("pattern"),
                                           QStringLiteral("ANSI31"));
    const double scale = optionalNumber(args, QStringLiteral("scale"), 1.0);
    const bool solid = optionalBool(args, QStringLiteral("solid"), false);

    const int before = countEntitiesOfType(DPI::HATCH);

    // The hatch action consumes the current selection as the boundary and
    // opens a modal dialog for the pattern; armHatchDialog() fills and
    // accepts that dialog from a timer inside its event loop.
    opSelectEntities(args);
    m_native->execCommand(QStringLiteral("escape"));
    m_native->execCommand(QStringLiteral("escape"));
    m_native->armHatchDialog(pattern, scale, angleDegrees, solid);
    m_native->execCommand(QStringLiteral("hatch"));
    m_native->disarmHatchDialog();

    selectAll(false);

    const int after = countEntitiesOfType(DPI::HATCH);
    if (!m_native->hatchDialogHandled()) {
        throw RequestError(QStringLiteral("failed"),
                           QStringLiteral("the hatch dialog never appeared; "
                                          "is the boundary selectable?"));
    }
    if (before < 0 || after != before + 1) {
        throw RequestError(QStringLiteral("failed"),
                           QStringLiteral("the hatch action accepted the dialog "
                                          "but created no hatch (count %1 -> %2); "
                                          "the boundary is probably not closed")
                               .arg(before).arg(after));
    }
    m_doc->updateView();
    return QJsonValue();
}

// --------------------------------------------------------------------------
// Layer state, block definition, geometry queries; modify tools: the rest
// of RS_Modification; interactive prompts.
//
// Same shape as mod_offset: the entity identities are snapshotted, the engine
// runs its own modify code, entities it replaced lose their handles, and the
// entities it created come back as get_entities rows. "copies" is
// RS_Modification's "number": 0 transforms the originals (the engine replaces
// them with transformed clones, so their handles die); n >= 1 keeps them and
// adds n copies at 1x, 2x, ... nx the transformation.
// Interactive prompts. Document_Interface's getPoint/getSelect install a
// LibreCAD action and spin processEvents() until it completes; getInt/
// getReal/getString open a modal QInputDialog. Either way the call blocks,
// so the server handles nothing else until the user answers -- clients call
// these with no socket timeout. Every prompt replies
//   {"cancelled": true[, "timed_out": true]}  or
//   {"cancelled": false, <"point" | "entities" | "value">: ...}.
// The NativeBridge prompt watcher supplies the optional timeout_ms (and the
// dialogs' default values); without the native layer prompts still work,
// only without a way out other than the user.
// --------------------------------------------------------------------------

namespace {

QJsonObject layerStateJson(const QString &name,
                           const NativeBridge::LayerState &state)
{
    QJsonObject object;
    if (!name.isNull())
        object.insert(QStringLiteral("name"), name);
    object.insert(QStringLiteral("frozen"), state.frozen);
    object.insert(QStringLiteral("locked"), state.locked);
    object.insert(QStringLiteral("print"), state.print);
    object.insert(QStringLiteral("construction"), state.construction);
    // What the user sees: a frozen layer is hidden.
    object.insert(QStringLiteral("visible"), !state.frozen);
    return object;
}

//! The optional "types" argument as DPI::ETYPE values (empty: any type).
QList<int> typesFromArgs(const QJsonObject &args)
{
    QList<int> wanted;
    if (!args.contains(QStringLiteral("types")))
        return wanted;
    const QJsonArray types = requireArray(args, QStringLiteral("types"), 1);
    for (int i = 0; i < types.size(); ++i) {
        int type = DPI::UNKNOWN;
        if (!types.at(i).isString() || !nameToType(types.at(i).toString(), &type)) {
            badArgs(QStringLiteral("\"types\"[%1] is not a known entity type")
                        .arg(i));
        }
        wanted.append(type);
    }
    return wanted;
}

int copiesArg(const QJsonObject &args)
{
    const QJsonValue value = args.value(QStringLiteral("copies"));
    if (value.isUndefined() || value.isNull())
        return 0;
    if (!value.isDouble() || value.toDouble() != qFloor(value.toDouble())
        || value.toDouble() < 0) {
        badArgs(QStringLiteral("\"copies\" must be a whole number >= 0"));
    }
    return value.toInt();
}

//! A color as entity data spells it (-1 ByLayer, -2 ByBlock, else 24-bit
//! RGB), or the names "bylayer" / "byblock".
int colorArg(const QJsonValue &value)
{
    if (value.isDouble()) {
        const double number = value.toDouble();
        if (number != qFloor(number) || number < -2 || number > 0xFFFFFF)
            badArgs(QStringLiteral("\"color\" must be -1, -2, or 0..0xFFFFFF"));
        return static_cast<int>(number);
    }
    if (value.isString()) {
        const QString name = value.toString();
        if (name.compare(QLatin1String("bylayer"), Qt::CaseInsensitive) == 0)
            return -1;
        if (name.compare(QLatin1String("byblock"), Qt::CaseInsensitive) == 0)
            return -2;
    }
    badArgs(QStringLiteral("\"color\" must be an RGB integer, -1, -2, "
                           "\"bylayer\", or \"byblock\""));
}

QJsonObject promptCancelled(bool timedOut)
{
    QJsonObject result;
    result.insert(QStringLiteral("cancelled"), true);
    if (timedOut)
        result.insert(QStringLiteral("timed_out"), true);
    return result;
}

QJsonObject promptAnswer(const QString &key, const QJsonValue &value)
{
    QJsonObject result;
    result.insert(QStringLiteral("cancelled"), false);
    result.insert(key, value);
    return result;
}

} // namespace

//! Map a native refusal onto the protocol's error codes.
[[noreturn]] static void throwNativeResult(NativeBridge::Result result,
                                           const QString &message)
{
    switch (result) {
    case NativeBridge::Result::NotFound:
        throw RequestError(QStringLiteral("not_found"), message);
    case NativeBridge::Result::Refused:
        throw RequestError(QStringLiteral("bad_request"), message);
    default:
        throw RequestError(QStringLiteral("failed"), message);
    }
}

QJsonObject Dispatcher::entityRow(Plug_Entity *entity)
{
    QHash<int, QVariant> data;
    entity->getData(&data);
    const auto typeField = data.constFind(DPI::ETYPE);
    const int type = typeField == data.constEnd() ? DPI::UNKNOWN
                                                  : typeField->toInt();
    QJsonObject item;
    item.insert(QStringLiteral("handle"), registerEntity(entity));
    item.insert(QStringLiteral("type"), typeToName(type));
    item.insert(QStringLiteral("data"), entityDataToJson(type, data));
    return item;
}

QJsonValue Dispatcher::opGetLayerState(const QJsonObject &args)
{
    requireModification();
    const QString name = requireString(args, QStringLiteral("name"));
    NativeBridge::LayerState state;
    if (!m_native->layerState(name, &state)) {
        throw RequestError(QStringLiteral("not_found"),
                           QStringLiteral("no layer \"%1\"").arg(name));
    }
    return layerStateJson(QString(), state);
}

QJsonValue Dispatcher::opGetLayerStates(const QJsonObject &args)
{
    Q_UNUSED(args)
    requireModification();
    QJsonArray result;
    for (const QString &name : m_doc->getAllLayer()) {
        NativeBridge::LayerState state;
        if (m_native->layerState(name, &state))
            result.append(layerStateJson(name, state));
    }
    return result;
}

QJsonValue Dispatcher::opSetLayerState(const QJsonObject &args)
{
    requireModification();
    const QString name = requireString(args, QStringLiteral("name"));
    NativeBridge::LayerStatePatch patch;
    const auto flag = [&](const char *key) -> std::optional<bool> {
        const QString field = QString::fromLatin1(key);
        if (!args.contains(field))
            return std::nullopt;
        return optionalBool(args, field, false);
    };
    patch.frozen = flag("frozen");
    patch.locked = flag("locked");
    patch.print = flag("print");
    patch.construction = flag("construction");
    if (!patch.frozen && !patch.locked && !patch.print && !patch.construction) {
        badArgs(QStringLiteral("give at least one of frozen, locked, print, "
                               "construction"));
    }

    const NativeBridge::Result result = m_native->setLayerState(name, patch);
    if (result != NativeBridge::Result::Done)
        throwNativeResult(result, m_native->lastError());
    m_doc->updateView();

    NativeBridge::LayerState state;
    m_native->layerState(name, &state);
    return layerStateJson(QString(), state);
}

QJsonValue Dispatcher::opRenameLayer(const QJsonObject &args)
{
    requireModification();
    const QString oldName = requireString(args, QStringLiteral("old"));
    const QString newName = requireString(args, QStringLiteral("new"));
    const NativeBridge::Result result = m_native->renameLayer(oldName, newName);
    if (result != NativeBridge::Result::Done)
        throwNativeResult(result, m_native->lastError());
    m_doc->updateView();

    NativeBridge::LayerState state;
    m_native->layerState(newName, &state);
    return layerStateJson(newName, state);
}

QJsonValue Dispatcher::opBlockDefine(const QJsonObject &args)
{
    requireModification();
    const QString name = requireString(args, QStringLiteral("name"));
    const QPointF base = requirePoint(args, QStringLiteral("base_point"));
    const QJsonArray handles = requireArray(args, QStringLiteral("handles"), 1);
    const bool remove = optionalBool(args, QStringLiteral("remove"), true);
    const bool insert = optionalBool(args, QStringLiteral("insert"), false);

    const QSet<const void *> before = entityKeys();
    selectHandles(handles);
    int selected = 0;
    const NativeBridge::Result result =
        m_native->defineBlock(name, base, remove, &selected);
    // createBlock() deselects what it takes, but a refusal leaves the
    // selection as selectHandles() set it.
    selectAll(false);
    if (result != NativeBridge::Result::Done)
        throwNativeResult(result, m_native->lastError());
    if (remove)
        invalidateHandles(handles);

    QJsonObject out;
    out.insert(QStringLiteral("name"), name);
    out.insert(QStringLiteral("count"), selected);
    out.insert(QStringLiteral("insert"), QJsonValue());
    if (insert) {
        // At the base point, unscaled and unrotated: the geometry appears
        // exactly where it was, as after Create Block.
        m_doc->addInsert(name, base, QPointF(1.0, 1.0), 0.0);
        const QJsonArray rows = newEntitiesSince(before);
        if (!rows.isEmpty())
            out.insert(QStringLiteral("insert"), rows.first());
    }
    m_doc->updateView();
    return out;
}

QJsonValue Dispatcher::opBlockRename(const QJsonObject &args)
{
    requireModification();
    const QString oldName = requireString(args, QStringLiteral("old"));
    const QString newName = requireString(args, QStringLiteral("new"));
    const NativeBridge::Result result = m_native->renameBlock(oldName, newName);
    if (result != NativeBridge::Result::Done)
        throwNativeResult(result, m_native->lastError());
    m_doc->updateView();
    return newName;
}

QJsonValue Dispatcher::opBlockRemove(const QJsonObject &args)
{
    requireModification();
    const NativeBridge::Result result =
        m_native->removeBlock(requireString(args, QStringLiteral("name")));
    if (result != NativeBridge::Result::Done)
        throwNativeResult(result, m_native->lastError());
    m_doc->updateView();
    return QJsonValue();
}

QJsonValue Dispatcher::opBlockEntities(const QJsonObject &args)
{
    requireModification();
    QList<Plug_Entity *> entities;
    const NativeBridge::Result result = m_native->blockEntities(
        requireString(args, QStringLiteral("name")), m_doc, &entities);
    if (result != NativeBridge::Result::Done)
        throwNativeResult(result, m_native->lastError());
    QJsonArray rows;
    for (Plug_Entity *entity : entities)
        rows.append(entityRow(entity));
    return rows;
}

QJsonValue Dispatcher::opEntityLength(const QJsonObject &args)
{
    requireNativeEntityAccess();
    double length = 0.0;
    QJsonObject result;
    if (m_native->entityLength(lookupEntity(args), &length))
        result.insert(QStringLiteral("length"), length);
    else
        result.insert(QStringLiteral("length"), QJsonValue());   // text, hatch...
    return result;
}

QJsonValue Dispatcher::opEntityArea(const QJsonObject &args)
{
    requireNativeEntityAccess();
    double area = 0.0;
    bool meaningful = false;
    if (!m_native->entityArea(lookupEntity(args), &area, &meaningful)) {
        throw RequestError(QStringLiteral("failed"),
                           QStringLiteral("could not read the entity"));
    }
    QJsonObject result;
    result.insert(QStringLiteral("area"), area);
    result.insert(QStringLiteral("closed"), meaningful);
    return result;
}

QJsonValue Dispatcher::opIntersections(const QJsonObject &args)
{
    requireNativeEntityAccess();
    Plug_Entity *a = lookupEntity(QJsonObject{
        {QStringLiteral("handle"), requireValue(args, QStringLiteral("a"))}});
    Plug_Entity *b = lookupEntity(QJsonObject{
        {QStringLiteral("handle"), requireValue(args, QStringLiteral("b"))}});
    const bool onEntities = optionalBool(args, QStringLiteral("on_entities"), true);
    QList<QPointF> points;
    if (!m_native->intersections(a, b, onEntities, &points)) {
        throw RequestError(QStringLiteral("failed"),
                           QStringLiteral("could not intersect the entities"));
    }
    QJsonArray result;
    for (const QPointF &point : points)
        result.append(pointJson(point));
    return result;
}

QJsonValue Dispatcher::opNearestEntity(const QJsonObject &args)
{
    requireNativeEntityAccess();
    const QPointF point = requirePoint(args, QStringLiteral("point"));
    const QList<int> wanted = typesFromArgs(args);
    const bool limited = args.contains(QStringLiteral("max_distance"))
                         && !args.value(QStringLiteral("max_distance")).isNull();
    const double maxDistance = limited ? requireNumber(args, QStringLiteral("max_distance"))
                                       : 0.0;

    QList<Plug_Entity *> entities;
    if (!m_doc->getAllEntities(&entities, false)) {
        throw RequestError(QStringLiteral("failed"),
                           QStringLiteral("getAllEntities() failed"));
    }

    // The nearest of the drawing's visible entities, by each entity's own
    // distance function: the metric RS_EntityContainer::getNearestEntity
    // uses, with the type filter and the distance cap applied here (the
    // engine's function can do neither, and an unfiltered nearest hit would
    // hide the nearest *wanted* one). The later entity wins a tie, as in the
    // engine, which prefers what was drawn last.
    Plug_Entity *best = nullptr;
    double bestDistance = 0.0;
    for (Plug_Entity *entity : entities) {
        double distance = 0.0;
        if (!entity
            || (!wanted.isEmpty() && !wanted.contains(readEntityType(entity)))
            || !m_native->entityDistance(entity, point, &distance)
            || (limited && distance > maxDistance)
            || (best && distance > bestDistance)) {
            continue;
        }
        best = entity;
        bestDistance = distance;
    }
    if (!best) {
        qDeleteAll(entities);
        return QJsonValue();
    }
    entities.removeOne(best);
    qDeleteAll(entities);

    QJsonObject row = entityRow(best);
    row.insert(QStringLiteral("distance"), bestDistance);
    return row;
}

QJsonValue Dispatcher::opNearestPoint(const QJsonObject &args)
{
    requireNativeEntityAccess();
    Plug_Entity *entity = lookupEntity(args);
    const QPointF point = requirePoint(args, QStringLiteral("point"));
    const bool onEntity = optionalBool(args, QStringLiteral("on_entity"), true);
    QPointF nearest;
    double distance = 0.0;
    if (!m_native->nearestPoint(entity, point, onEntity, &nearest, &distance))
        return QJsonValue();
    QJsonObject result;
    result.insert(QStringLiteral("point"), pointJson(nearest));
    result.insert(QStringLiteral("distance"), distance);
    return result;
}

QJsonValue Dispatcher::opPointInside(const QJsonObject &args)
{
    requireNativeEntityAccess();
    Plug_Entity *entity = lookupEntity(args);
    const QPointF point = requirePoint(args, QStringLiteral("point"));
    bool inside = false;
    bool onContour = false;
    const NativeBridge::Result result =
        m_native->pointInside(entity, point, &inside, &onContour);
    if (result != NativeBridge::Result::Done)
        throwNativeResult(result, m_native->lastError());
    return inside;
}

QJsonValue Dispatcher::opEntityId(const QJsonObject &args)
{
    requireNativeEntityAccess();
    QHash<int, QVariant> data;
    lookupEntity(args)->getData(&data);
    QJsonObject result;
    result.insert(QStringLiteral("id"),
                  static_cast<double>(data.value(DPI::EID).toULongLong()));
    return result;
}

QJsonValue Dispatcher::opFindEntity(const QJsonObject &args)
{
    requireNativeEntityAccess();
    const QJsonValue idValue = requireValue(args, QStringLiteral("id"));
    if (!idValue.isDouble())
        badArgs(QStringLiteral("\"id\" must be a number"));
    const qulonglong wanted = static_cast<qulonglong>(idValue.toDouble());

    QList<Plug_Entity *> entities;
    if (!m_doc->getAllEntities(&entities, false)) {
        throw RequestError(QStringLiteral("failed"),
                           QStringLiteral("getAllEntities() failed"));
    }
    Plug_Entity *found = nullptr;
    for (Plug_Entity *entity : entities) {
        QHash<int, QVariant> data;
        entity->getData(&data);
        if (!isUndone(entity) && data.value(DPI::EID).toULongLong() == wanted) {
            found = entity;
            break;
        }
    }
    if (!found) {
        qDeleteAll(entities);
        throw RequestError(QStringLiteral("not_found"),
                           QStringLiteral("no entity with id %1 in the "
                                          "drawing").arg(wanted));
    }
    entities.removeOne(found);
    qDeleteAll(entities);
    return entityRow(found);
}

// View control, document windows, export. All read-only for undo: they move
// the view or write files, never entities. activate_document and file_close
// end the session and live in BridgeServer.
// --------------------------------------------------------------------------

void Dispatcher::requireView() const
{
    if (!m_native || !m_native->viewAvailable())
        nativeUnavailable();
}

QJsonValue Dispatcher::opGetView(const QJsonObject &args)
{
    Q_UNUSED(args)
    requireView();
    NativeBridge::ViewState state;
    if (!m_native->viewState(&state))
        throw RequestError(QStringLiteral("failed"), QStringLiteral("no view"));
    QJsonObject result;
    // A single number while the axes agree, which is always unless
    // zoom_auto/zoom_window ran with keep_aspect false.
    result.insert(QStringLiteral("factor"), state.factorX);
    if (state.factorY != state.factorX) {
        QJsonArray factors{state.factorX, state.factorY};
        result.insert(QStringLiteral("factor_xy"), factors);
    }
    result.insert(QStringLiteral("offset"), QJsonArray{state.offsetX, state.offsetY});
    result.insert(QStringLiteral("size"), QJsonArray{state.width, state.height});
    result.insert(QStringLiteral("visible"), bboxJson(state.min, state.max));
    result.insert(QStringLiteral("center"),
                  pointJson((state.min + state.max) / 2.0));
    return result;
}

QJsonValue Dispatcher::opZoomAuto(const QJsonObject &args)
{
    requireView();
    m_native->zoomAuto(optionalBool(args, QStringLiteral("keep_aspect"), true));
    return opGetView(args);
}

QJsonValue Dispatcher::opZoomWindow(const QJsonObject &args)
{
    requireView();
    const QPointF p1 = requirePoint(args, QStringLiteral("p1"));
    const QPointF p2 = requirePoint(args, QStringLiteral("p2"));
    if (qFuzzyCompare(p1.x(), p2.x()) && qFuzzyCompare(p1.y(), p2.y()))
        badArgs(QStringLiteral("\"p1\" and \"p2\" must differ"));
    m_native->zoomWindow(p1, p2, optionalBool(args, QStringLiteral("keep_aspect"), true));
    return opGetView(args);
}

QJsonValue Dispatcher::zoomBy(const QJsonObject &args, bool out)
{
    requireView();
    // LibreCAD's zoom-in/out actions step by 1.137 (RS_ActionZoomIn).
    const double factor = optionalNumber(args, QStringLiteral("factor"), 1.137);
    if (!(factor > 1e-6))
        badArgs(QStringLiteral("\"factor\" must be positive"));
    const bool hasCenter = args.contains(QStringLiteral("center"))
                           && !args.value(QStringLiteral("center")).isNull();
    const QPointF center = hasCenter ? requirePoint(args, QStringLiteral("center"))
                                     : QPointF();
    m_native->zoomIn(factor, hasCenter, center, out);
    return opGetView(args);
}

QJsonValue Dispatcher::opZoomIn(const QJsonObject &args)
{
    return zoomBy(args, false);
}

QJsonValue Dispatcher::opZoomOut(const QJsonObject &args)
{
    return zoomBy(args, true);
}

QJsonValue Dispatcher::opZoomPan(const QJsonObject &args)
{
    requireView();
    const double dx = requireNumber(args, QStringLiteral("dx"));
    const double dy = requireNumber(args, QStringLiteral("dy"));
    m_native->zoomPan(qRound(dx), qRound(dy));
    return opGetView(args);
}

QJsonValue Dispatcher::opZoomPrevious(const QJsonObject &args)
{
    requireView();
    m_native->zoomPrevious();
    return opGetView(args);
}

QJsonValue Dispatcher::opZoomPage(const QJsonObject &args)
{
    requireView();
    m_native->zoomPage();
    return opGetView(args);
}

QJsonValue Dispatcher::opSetView(const QJsonObject &args)
{
    requireView();
    const bool hasFactor = args.contains(QStringLiteral("factor"));
    const double factor = optionalNumber(args, QStringLiteral("factor"), 1.0);
    if (hasFactor && !(factor > 1e-9))
        badArgs(QStringLiteral("\"factor\" must be positive"));
    const bool hasOffset = args.contains(QStringLiteral("offset"));
    const bool hasCenter = args.contains(QStringLiteral("center"));
    if (hasOffset && hasCenter)
        badArgs(QStringLiteral("pass \"offset\" or \"center\", not both"));
    if (!hasFactor && !hasOffset && !hasCenter)
        badArgs(QStringLiteral("pass \"factor\", \"offset\" or \"center\""));
    const QPointF offset = hasOffset ? requirePoint(args, QStringLiteral("offset"))
                                     : QPointF();
    const QPointF center = hasCenter ? requirePoint(args, QStringLiteral("center"))
                                     : QPointF();
    m_native->setView(hasFactor, factor, hasOffset, qRound(offset.x()),
                      qRound(offset.y()), hasCenter, center);
    return opGetView(args);
}

QJsonValue Dispatcher::opListDocuments(const QJsonObject &args)
{
    Q_UNUSED(args)
    requireModification();
    QList<DocumentWindow> windows;
    if (!documentWindows(&windows)) {
        throw RequestError(QStringLiteral("failed"),
                           QStringLiteral("cannot read the document windows"));
    }
    QJsonArray rows;
    for (const DocumentWindow &window : windows) {
        QJsonObject row;
        row.insert(QStringLiteral("index"), window.index);
        row.insert(QStringLiteral("path"), window.path);
        row.insert(QStringLiteral("title"), window.title);
        row.insert(QStringLiteral("modified"), window.modified);
        row.insert(QStringLiteral("active"), window.active);
        row.insert(QStringLiteral("parent"), window.parent >= 0
                                                 ? QJsonValue(window.parent)
                                                 : QJsonValue());
        rows.append(row);
    }
    return rows;
}

QJsonValue Dispatcher::opExportImage(const QJsonObject &args)
{
    requireModification();
    const QString path = requireString(args, QStringLiteral("path"));
    const double width = requireNumber(args, QStringLiteral("width"));
    const double height = requireNumber(args, QStringLiteral("height"));
    if (width < 1 || height < 1 || width > 32768 || height > 32768)
        badArgs(QStringLiteral("\"width\" and \"height\" must be 1..32768 pixels"));
    QString format = optionalString(args, QStringLiteral("format"),
                                    QFileInfo(path).suffix()).toLower();
    if (format == QLatin1String("jpeg"))
        format = QStringLiteral("jpg");
    const bool svg = format == QLatin1String("svg");
    if (!svg && !QImageWriter::supportedImageFormats().contains(format.toLatin1())) {
        badArgs(QStringLiteral("unsupported image format \"%1\" (by extension or "
                               "\"format\"): png, jpg, bmp, svg, ...").arg(format));
    }
    const int border = static_cast<int>(optionalNumber(args, QStringLiteral("border"), 0));
    if (border < 0 || 2 * border >= qMin(width, height))
        badArgs(QStringLiteral("\"border\" must be >= 0 and leave room to draw"));
    const QString background = optionalString(args, QStringLiteral("background"),
                                              QStringLiteral("white"));
    if (background != QLatin1String("white") && background != QLatin1String("black"))
        badArgs(QStringLiteral("\"background\" must be \"white\" or \"black\""));
    const bool blackWhite = optionalBool(args, QStringLiteral("black_white"), false);
    const bool transparent = optionalBool(args, QStringLiteral("transparent"), false);
    if (transparent && (svg || background == QLatin1String("black") || blackWhite))
        badArgs(QStringLiteral("\"transparent\" works for raster formats on the "
                               "default white background only"));
    if (!m_native->exportImage(path, format, QSize(int(width), int(height)), border,
                               background == QLatin1String("black"), blackWhite,
                               transparent)) {
        throw RequestError(QStringLiteral("failed"), m_native->lastError());
    }
    QJsonObject result;
    result.insert(QStringLiteral("path"), QFileInfo(path).absoluteFilePath());
    result.insert(QStringLiteral("format"), format);
    result.insert(QStringLiteral("size"), QJsonArray{int(width), int(height)});
    return result;
}

QJsonValue Dispatcher::opExportPdf(const QJsonObject &args)
{
    requireModification();
    const QString path = requireString(args, QStringLiteral("path"));
    const QString paper = optionalString(args, QStringLiteral("paper"), QString());
    const int landscape = args.contains(QStringLiteral("landscape"))
        ? (optionalBool(args, QStringLiteral("landscape"), false) ? 1 : 0)
        : -1;
    const bool fit = optionalBool(args, QStringLiteral("fit_to_page"), true);
    int pages = 0;
    QSizeF paperMm;
    if (!m_native->exportPdf(path, paper, landscape, fit, &pages, &paperMm))
        throw RequestError(QStringLiteral("failed"), m_native->lastError());
    QJsonObject result;
    result.insert(QStringLiteral("path"), QFileInfo(path).absoluteFilePath());
    result.insert(QStringLiteral("pages"), pages);
    result.insert(QStringLiteral("paper_mm"),
                  QJsonArray{paperMm.width(), paperMm.height()});
    return result;
}

Plug_Entity *Dispatcher::lookupNamedEntity(const QJsonObject &args,
                                           const QString &name) const
{
    QJsonObject lookup;
    lookup.insert(QStringLiteral("handle"), requireValue(args, name));
    return lookupEntity(lookup);
}

void Dispatcher::isolateReplacement(const QJsonArray &handles)
{
    QList<Plug_Entity *> entities;
    for (int i = 0; i < handles.size(); ++i) {
        if (!handles.at(i).isDouble())
            badArgs(QStringLiteral("\"handles\"[%1] must be a number").arg(i));
        QJsonObject lookup;
        lookup.insert(QStringLiteral("handle"), handles.at(i));
        entities.append(lookupEntity(lookup));
    }
    m_native->isolateReplacement(entities);
}

template <typename Apply>
QJsonValue Dispatcher::runSelectionTransform(const QJsonObject &args,
                                             int copies, Apply apply)
{
    const QJsonArray handles = requireArray(args, QStringLiteral("handles"), 1);
    if (copies == 0)
        isolateReplacement(handles);
    const QSet<const void *> before = entityKeys();
    selectHandles(handles);
    if (!apply())
        throw RequestError(QStringLiteral("failed"), m_native->lastError());
    if (copies == 0)
        invalidateHandles(handles);
    return newEntitiesSince(before);
}

QJsonValue Dispatcher::opModMove(const QJsonObject &args)
{
    requireModification();
    const QPointF offset = requirePoint(args, QStringLiteral("offset"));
    const int copies = copiesArg(args);
    const bool layer = optionalBool(args, QStringLiteral("use_current_layer"), false);
    const bool attributes =
        optionalBool(args, QStringLiteral("use_current_attributes"), false);
    return runSelectionTransform(args, copies, [&] {
        return m_native->move(offset, copies, layer, attributes);
    });
}

QJsonValue Dispatcher::opModRotate(const QJsonObject &args)
{
    requireModification();
    const QPointF center = requirePoint(args, QStringLiteral("center"));
    const double angle = requireNumber(args, QStringLiteral("angle"));
    const int copies = copiesArg(args);
    const bool layer = optionalBool(args, QStringLiteral("use_current_layer"), false);
    const bool attributes =
        optionalBool(args, QStringLiteral("use_current_attributes"), false);
    return runSelectionTransform(args, copies, [&] {
        return m_native->rotate(center, angle, copies, layer, attributes);
    });
}

QJsonValue Dispatcher::opModScale(const QJsonObject &args)
{
    requireModification();
    const QPointF center = requirePoint(args, QStringLiteral("center"));
    // A single number scales uniformly; [sx, sy] scales per axis (which turns
    // circles and arcs into ellipses when sx != sy, as in the GUI).
    const QJsonValue factorValue = requireValue(args, QStringLiteral("factor"));
    const QPointF factor = factorValue.isDouble()
        ? QPointF(factorValue.toDouble(), factorValue.toDouble())
        : pointFromJson(factorValue, QStringLiteral("factor"));
    if (factor.x() == 0.0 || factor.y() == 0.0)
        badArgs(QStringLiteral("\"factor\" must not be zero"));
    const int copies = copiesArg(args);
    const bool layer = optionalBool(args, QStringLiteral("use_current_layer"), false);
    const bool attributes =
        optionalBool(args, QStringLiteral("use_current_attributes"), false);
    return runSelectionTransform(args, copies, [&] {
        return m_native->scale(center, factor, copies, layer, attributes);
    });
}

QJsonValue Dispatcher::opModMoveRotate(const QJsonObject &args)
{
    requireModification();
    const QPointF offset = requirePoint(args, QStringLiteral("offset"));
    const QPointF center = requirePoint(args, QStringLiteral("center"));
    const double angle = requireNumber(args, QStringLiteral("angle"));
    const int copies = copiesArg(args);
    const bool layer = optionalBool(args, QStringLiteral("use_current_layer"), false);
    const bool attributes =
        optionalBool(args, QStringLiteral("use_current_attributes"), false);
    return runSelectionTransform(args, copies, [&] {
        return m_native->moveRotate(offset, center, angle, copies, layer,
                                    attributes);
    });
}

QJsonValue Dispatcher::opModRotate2(const QJsonObject &args)
{
    requireModification();
    const QPointF center1 = requirePoint(args, QStringLiteral("center1"));
    const QPointF center2 = requirePoint(args, QStringLiteral("center2"));
    const double angle1 = requireNumber(args, QStringLiteral("angle1"));
    const double angle2 = requireNumber(args, QStringLiteral("angle2"));
    const int copies = copiesArg(args);
    const bool layer = optionalBool(args, QStringLiteral("use_current_layer"), false);
    const bool attributes =
        optionalBool(args, QStringLiteral("use_current_attributes"), false);
    return runSelectionTransform(args, copies, [&] {
        return m_native->rotate2(center1, center2, angle1, angle2, copies,
                                 layer, attributes);
    });
}

QJsonValue Dispatcher::opModStretch(const QJsonObject &args)
{
    requireModification();
    const QPointF first = requirePoint(args, QStringLiteral("first_corner"));
    const QPointF second = requirePoint(args, QStringLiteral("second_corner"));
    const QPointF offset = requirePoint(args, QStringLiteral("offset"));

    m_native->isolateStretch(first, second);
    // Stretch picks its entities by window, not by handle, and replaces every
    // one it touches with a stretched clone. Which handles died is read off
    // afterwards: the ones whose entity was live before and is undone now.
    QList<int> live;
    for (auto it = m_entities.constBegin(); it != m_entities.constEnd(); ++it) {
        if (!isUndone(it.value()))
            live.append(it.key());
    }
    const QSet<const void *> before = entityKeys();
    // RS_Modification::stretch selects what it clones and then removes
    // *everything* selected, so a leftover selection would be deleted.
    selectAll(false);
    if (!m_native->stretch(first, second, offset))
        throw RequestError(QStringLiteral("failed"), m_native->lastError());
    QJsonArray replaced;
    for (int handle : live) {
        if (isUndone(m_entities.value(handle)))
            replaced.append(handle);
    }
    invalidateHandles(replaced);
    return newEntitiesSince(before);
}

QJsonValue Dispatcher::opModRound(const QJsonObject &args)
{
    requireModification();
    Plug_Entity *entity1 = lookupNamedEntity(args, QStringLiteral("entity1"));
    Plug_Entity *entity2 = lookupNamedEntity(args, QStringLiteral("entity2"));
    const QPointF point1 = requirePoint(args, QStringLiteral("point1"));
    const QPointF point2 = requirePoint(args, QStringLiteral("point2"));
    const double radius = requireNumber(args, QStringLiteral("radius"));
    const bool trim = optionalBool(args, QStringLiteral("trim"), true);
    if (radius <= 0.0)
        badArgs(QStringLiteral("\"radius\" must be positive"));
    // RS_Modification::round places the arc on the side of both entities
    // where "corner" lies (it offsets each entity toward it). The GUI passes
    // its second click; the midpoint of the two picks is inside the corner
    // whenever the picks are on the kept parts, which is what they mean.
    const QPointF corner = args.contains(QStringLiteral("corner"))
        ? requirePoint(args, QStringLiteral("corner"))
        : (point1 + point2) / 2.0;

    const QJsonArray pair{args.value(QStringLiteral("entity1")),
                          args.value(QStringLiteral("entity2"))};
    if (trim)
        isolateReplacement(pair);
    const QSet<const void *> before = entityKeys();
    if (!m_native->round(entity1, point1, entity2, point2, corner, radius, trim))
        throw RequestError(QStringLiteral("failed"), m_native->lastError());
    if (trim)
        invalidateHandles(pair);
    return newEntitiesSince(before);
}

QJsonValue Dispatcher::opModBevel(const QJsonObject &args)
{
    requireModification();
    Plug_Entity *entity1 = lookupNamedEntity(args, QStringLiteral("entity1"));
    Plug_Entity *entity2 = lookupNamedEntity(args, QStringLiteral("entity2"));
    const QPointF point1 = requirePoint(args, QStringLiteral("point1"));
    const QPointF point2 = requirePoint(args, QStringLiteral("point2"));
    const double length1 = requireNumber(args, QStringLiteral("length1"));
    const double length2 = optionalNumber(args, QStringLiteral("length2"), length1);
    const bool trim = optionalBool(args, QStringLiteral("trim"), true);
    if (length1 < 0.0 || length2 < 0.0 || (length1 == 0.0 && length2 == 0.0))
        badArgs(QStringLiteral("chamfer lengths must be >= 0 and not both 0"));

    const QJsonArray pair{args.value(QStringLiteral("entity1")),
                          args.value(QStringLiteral("entity2"))};
    if (trim)
        isolateReplacement(pair);
    const QSet<const void *> before = entityKeys();
    if (!m_native->bevel(entity1, point1, entity2, point2, length1, length2, trim))
        throw RequestError(QStringLiteral("failed"), m_native->lastError());
    if (trim)
        invalidateHandles(pair);
    return newEntitiesSince(before);
}

QJsonValue Dispatcher::opModCut(const QJsonObject &args)
{
    requireModification();
    Plug_Entity *entity = lookupEntity(args);
    const QPointF point = requirePoint(args, QStringLiteral("point"));

    const QJsonArray replaced{args.value(QStringLiteral("handle"))};
    isolateReplacement(replaced);
    const QSet<const void *> before = entityKeys();
    if (!m_native->cut(entity, point))
        throw RequestError(QStringLiteral("failed"), m_native->lastError());
    invalidateHandles(replaced);
    return newEntitiesSince(before);
}

QJsonValue Dispatcher::opModChangeAttributes(const QJsonObject &args)
{
    requireModification();
    const QJsonArray handles = requireArray(args, QStringLiteral("handles"), 1);

    // Formats follow entity data (and so entity_update): color as an int,
    // "lineweight"-style width names, linetype names. "bylayer"/"byblock"
    // are accepted for color too.
    NativeBridge::AttributeChange change;
    if (args.contains(QStringLiteral("layer"))) {
        change.changeLayer = true;
        change.layer = requireString(args, QStringLiteral("layer"));
        // RS_Entity::setLayer(name) quietly leaves the entity on no layer
        // when the name is unknown.
        if (!m_doc->getAllLayer().contains(change.layer))
            badArgs(QStringLiteral("no layer named \"%1\"").arg(change.layer));
    }
    if (args.contains(QStringLiteral("color"))) {
        change.changeColor = true;
        change.color = colorArg(args.value(QStringLiteral("color")));
    }
    if (args.contains(QStringLiteral("linetype"))) {
        change.changeLineType = true;
        change.lineType = requireString(args, QStringLiteral("linetype"));
        if (!NativeBridge::isLineTypeName(change.lineType))
            badArgs(QStringLiteral("unknown linetype \"%1\"").arg(change.lineType));
    }
    if (args.contains(QStringLiteral("width"))) {
        change.changeWidth = true;
        change.width = requireString(args, QStringLiteral("width"));
        if (!NativeBridge::isLineWidthName(change.width))
            badArgs(QStringLiteral("unknown width \"%1\" (use e.g. \"0.25mm\", "
                                   "\"BYLAYER\")").arg(change.width));
    }
    if (!change.changeLayer && !change.changeColor && !change.changeLineType
        && !change.changeWidth) {
        badArgs(QStringLiteral("name at least one of layer, color, linetype, "
                               "width"));
    }

    isolateReplacement(handles);
    const QSet<const void *> before = entityKeys();
    selectHandles(handles);
    if (!m_native->changeAttributes(change))
        throw RequestError(QStringLiteral("failed"), m_native->lastError());
    // Every selected entity is replaced by a re-attributed clone.
    invalidateHandles(handles);
    return newEntitiesSince(before);
}

QJsonValue Dispatcher::opModRevertDirection(const QJsonObject &args)
{
    requireModification();
    const QJsonArray handles = requireArray(args, QStringLiteral("handles"), 1);
    isolateReplacement(handles);
    const QSet<const void *> before = entityKeys();
    selectHandles(handles);
    if (!m_native->revertDirection())
        throw RequestError(QStringLiteral("failed"), m_native->lastError());
    invalidateHandles(handles);
    return newEntitiesSince(before);
}

int Dispatcher::promptTimeout(const QJsonObject &args, bool needsView) const
{
    if (!args.contains(QStringLiteral("timeout_ms")))
        return 0;
    const double timeoutMs = requireNumber(args, QStringLiteral("timeout_ms"));
    if (timeoutMs < 1 || timeoutMs > 24.0 * 3600 * 1000)
        badArgs(QStringLiteral("\"timeout_ms\" must be between 1 and a day"));
    if (!m_native || (needsView && !m_native->modificationAvailable()))
        nativeUnavailable();
    return static_cast<int>(timeoutMs);
}

QJsonValue Dispatcher::opPromptPoint(const QJsonObject &args)
{
    const QString message = optionalString(args, QStringLiteral("message"), QString());
    const bool hasBase = args.contains(QStringLiteral("base"));
    QPointF base;
    if (hasBase)
        base = requirePoint(args, QStringLiteral("base"));
    const int timeoutMs = promptTimeout(args, true);

    // getPoint() ends whatever action the user had running, shows the
    // message on the command line, and draws a rubber band from "base".
    // A click or a coordinate typed on the command line answers it; a right
    // click cancels.
    QPointF point;
    if (m_native)
        m_native->armPrompt(NativeBridge::PointPrompt, timeoutMs, QVariant());
    const bool answered = m_doc->getPoint(&point, message, hasBase ? &base : nullptr);
    const bool cancelled = m_native && m_native->disarmPrompt();
    if (!answered || cancelled)
        return promptCancelled(cancelled);
    return promptAnswer(QStringLiteral("point"), pointJson(point));
}

QJsonValue Dispatcher::opPromptSelect(const QJsonObject &args)
{
    const QString message = optionalString(args, QStringLiteral("message"), QString());
    const int timeoutMs = promptTimeout(args, true);

    QList<Plug_Entity *> picked;
    if (m_native)
        m_native->armPrompt(NativeBridge::SelectPrompt, timeoutMs, QVariant());
    const bool answered = m_doc->getSelect(&picked, message);
    const bool cancelled = m_native && m_native->disarmPrompt();

    // In LibreCAD 2.2.1.5 getSelect() reports false however the selection
    // ends (Enter, Escape, right click): every way out finishes the action,
    // the action stack drops it, and the isValid() check that is meant to
    // tell "cancelled" from "done" never finds it (see docs/findings.md).
    // The user's selection is still in the drawing, though, so with the
    // native layer the answer is read from there; without it only a true
    // return can be trusted.
    QList<Plug_Entity *> chosen;
    if (!cancelled && m_native && m_native->selectionAvailable()) {
        qDeleteAll(picked);
        picked.clear();
        if (m_doc->getAllEntities(&picked, false)) {
            for (Plug_Entity *entity : picked) {
                bool selected = false;
                if (entity && !isUndone(entity)
                    && m_native->isSelected(entity, &selected) && selected) {
                    chosen.append(entity);
                } else {
                    delete entity;
                }
            }
        }
    } else if (!cancelled && answered) {
        for (Plug_Entity *entity : picked) {
            if (entity && !isUndone(entity))
                chosen.append(entity);
            else
                delete entity;
        }
    } else {
        qDeleteAll(picked);
    }
    picked.clear();

    if (chosen.isEmpty())
        return promptCancelled(cancelled);

    QJsonArray rows;
    for (Plug_Entity *entity : chosen) {
        QHash<int, QVariant> data;
        entity->getData(&data);
        const auto typeField = data.constFind(DPI::ETYPE);
        const int type = typeField == data.constEnd() ? DPI::UNKNOWN
                                                      : typeField->toInt();
        QJsonObject item;
        item.insert(QStringLiteral("handle"), registerEntity(entity));
        item.insert(QStringLiteral("type"), typeToName(type));
        item.insert(QStringLiteral("data"), entityDataToJson(type, data));
        rows.append(item);
    }
    return promptAnswer(QStringLiteral("entities"), rows);
}

QJsonValue Dispatcher::opPromptInt(const QJsonObject &args)
{
    const QString message = optionalString(args, QStringLiteral("message"), QString());
    const QString title = optionalString(args, QStringLiteral("title"), QString());
    QVariant fallback;
    if (args.contains(QStringLiteral("default"))) {
        const double value = requireNumber(args, QStringLiteral("default"));
        if (value != std::floor(value) || qAbs(value) > 2147483647.0)
            badArgs(QStringLiteral("\"default\" must be a 32-bit integer"));
        fallback = static_cast<int>(value);
    }
    const int timeoutMs = promptTimeout(args, false);

    int value = 0;
    if (m_native)
        m_native->armPrompt(NativeBridge::DialogPrompt, timeoutMs, fallback);
    const bool answered = m_doc->getInt(&value, message, title);
    const bool cancelled = m_native && m_native->disarmPrompt();
    if (!answered || cancelled)
        return promptCancelled(cancelled);
    return promptAnswer(QStringLiteral("value"), value);
}

QJsonValue Dispatcher::opPromptReal(const QJsonObject &args)
{
    const QString message = optionalString(args, QStringLiteral("message"), QString());
    const QString title = optionalString(args, QStringLiteral("title"), QString());
    QVariant fallback;
    if (args.contains(QStringLiteral("default")))
        fallback = requireNumber(args, QStringLiteral("default"));
    const int timeoutMs = promptTimeout(args, false);

    qreal value = 0.0;
    if (m_native)
        m_native->armPrompt(NativeBridge::DialogPrompt, timeoutMs, fallback);
    const bool answered = m_doc->getReal(&value, message, title);
    const bool cancelled = m_native && m_native->disarmPrompt();
    if (!answered || cancelled)
        return promptCancelled(cancelled);
    return promptAnswer(QStringLiteral("value"), value);
}

QJsonValue Dispatcher::opPromptString(const QJsonObject &args)
{
    const QString message = optionalString(args, QStringLiteral("message"), QString());
    const QString title = optionalString(args, QStringLiteral("title"), QString());
    QVariant fallback;
    if (args.contains(QStringLiteral("default")))
        fallback = requireString(args, QStringLiteral("default"));
    const int timeoutMs = promptTimeout(args, false);

    // getString() leaves the string alone when the user accepts an empty
    // field, so it starts empty: an accepted empty field answers "".
    QString value;
    if (m_native)
        m_native->armPrompt(NativeBridge::DialogPrompt, timeoutMs, fallback);
    const bool answered = m_doc->getString(&value, message, title);
    const bool cancelled = m_native && m_native->disarmPrompt();
    if (!answered || cancelled)
        return promptCancelled(cancelled);
    return promptAnswer(QStringLiteral("value"), value);
}

// Creation through the engine (roadmap item 2). The plugin API cannot make
// MTEXT (Doc_plugin_interface::addMText is not part of Document_Interface),
// IMAGE, HATCH, or any dimension; these build them in the native layer the
// way LibreCAD's own actions do, without the command line. Each returns the
// created entity as a one-row get_entities result.
// --------------------------------------------------------------------------

QJsonObject Dispatcher::rowData(Plug_Entity *entity, int type,
                                const QHash<int, QVariant> &data) const
{
    QJsonObject out = entityDataToJson(type, data);
    QVariantMap details;
    if (m_native && m_native->entityDetails(entity, &details)) {
        const QJsonObject extra = QJsonObject::fromVariantMap(details);
        for (auto it = extra.constBegin(); it != extra.constEnd(); ++it) {
            if (!out.contains(it.key()))
                out.insert(it.key(), it.value());
        }
    }
    return out;
}

Plug_Entity *Dispatcher::lookupEntityArg(const QJsonObject &args,
                                         const QString &name) const
{
    QJsonObject lookup;
    lookup.insert(QStringLiteral("handle"), requireValue(args, name));
    return lookupEntity(lookup);
}

namespace {

//! Dimension label override: absent or "" for the measured value.
QString dimensionText(const QJsonObject &args)
{
    return optionalString(args, QStringLiteral("text"), QString());
}

//! The two end points of a line argument: a handle of a LINE entity, or
//! [[x1, y1], [x2, y2]].
void lineArg(const QJsonObject &args, const QString &name,
             const std::function<Plug_Entity *(const QJsonObject &)> &lookup,
             QPointF *start, QPointF *end)
{
    const QJsonValue value = requireValue(args, name);
    if (value.isDouble()) {
        QJsonObject handle;
        handle.insert(QStringLiteral("handle"), value);
        Plug_Entity *entity = lookup(handle);
        QHash<int, QVariant> data;
        entity->getData(&data);
        const int type = data.value(DPI::ETYPE, DPI::UNKNOWN).toInt();
        if (type != DPI::LINE) {
            badArgs(QStringLiteral("\"%1\" is a %2, not a LINE")
                        .arg(name, typeToName(type)));
        }
        *start = QPointF(data.value(DPI::STARTX).toDouble(),
                         data.value(DPI::STARTY).toDouble());
        *end = QPointF(data.value(DPI::ENDX).toDouble(),
                       data.value(DPI::ENDY).toDouble());
        return;
    }
    const QJsonArray pair = value.toArray();
    if (!value.isArray() || pair.size() != 2) {
        badArgs(QStringLiteral("\"%1\" must be a LINE handle or "
                               "[[x1, y1], [x2, y2]]").arg(name));
    }
    *start = pointFromJson(pair.at(0), QStringLiteral("\"%1\"[0]").arg(name));
    *end = pointFromJson(pair.at(1), QStringLiteral("\"%1\"[1]").arg(name));
}

} // namespace

QJsonValue Dispatcher::opAddMText(const QJsonObject &args)
{
    requireModification();
    const QString text = requireString(args, QStringLiteral("text"));
    const QPointF at = requirePoint(args, QStringLiteral("at"));
    const double height = requireNumber(args, QStringLiteral("height"));
    const double angle = optionalNumber(args, QStringLiteral("angle"), 0.0);
    const QString style = optionalString(args, QStringLiteral("style"),
                                         QStringLiteral("standard"));
    const double width = optionalNumber(args, QStringLiteral("width"), 100.0);
    const double lineSpacing =
        optionalNumber(args, QStringLiteral("line_spacing"), 1.0);
    const DPI::HAlign halign = halignFromJson(args);
    // Same names as add_text, but top by default: an MTEXT hangs from its
    // insertion point, as LibreCAD's MText tool places it.
    const DPI::VAlign valign = args.contains(QStringLiteral("valign"))
                                   ? valignFromJson(args) : DPI::VAlignTop;
    if (height <= 0.0)
        badArgs(QStringLiteral("\"height\" must be positive"));
    if (width <= 0.0)
        badArgs(QStringLiteral("\"width\" must be positive"));
    if (lineSpacing <= 0.0)
        badArgs(QStringLiteral("\"line_spacing\" must be positive"));

    const QSet<const void *> before = entityKeys();
    if (!m_native->addMText(text, style, at, height, width, angle, halign,
                            valign, lineSpacing)) {
        throw RequestError(QStringLiteral("failed"), m_native->lastError());
    }
    return newEntitiesSince(before);
}

QJsonValue Dispatcher::opAddImage(const QJsonObject &args)
{
    requireModification();
    const QString path = requireString(args, QStringLiteral("path"));
    const QPointF at = requirePoint(args, QStringLiteral("at"));
    const double angle = optionalNumber(args, QStringLiteral("angle"), 0.0);
    const int brightness =
        static_cast<int>(optionalNumber(args, QStringLiteral("brightness"), 50));
    const int contrast =
        static_cast<int>(optionalNumber(args, QStringLiteral("contrast"), 50));
    const int fade = static_cast<int>(optionalNumber(args, QStringLiteral("fade"), 0));
    if (QFileInfo(path).isRelative()) {
        // LibreCAD's working directory is not the client's.
        badArgs(QStringLiteral("\"path\" must be absolute"));
    }
    for (int value : {brightness, contrast, fade}) {
        if (value < 0 || value > 100)
            badArgs(QStringLiteral("brightness, contrast and fade are 0..100"));
    }

    // Size: drawing units per pixel ("scale", the image action's factor,
    // default 1), or the width or height the whole image should have.
    int sizing = 0;
    for (const char *name : {"scale", "width", "height"})
        sizing += args.contains(QLatin1String(name)) ? 1 : 0;
    if (sizing > 1)
        badArgs(QStringLiteral("give at most one of \"scale\", \"width\", \"height\""));
    QSize pixels;
    if (!m_native->imagePixelSize(path, &pixels)) {
        throw RequestError(QStringLiteral("failed"),
                           QStringLiteral("cannot read an image from \"%1\"")
                               .arg(path));
    }
    double scale = optionalNumber(args, QStringLiteral("scale"), 1.0);
    if (args.contains(QStringLiteral("width")))
        scale = requireNumber(args, QStringLiteral("width")) / pixels.width();
    if (args.contains(QStringLiteral("height")))
        scale = requireNumber(args, QStringLiteral("height")) / pixels.height();
    if (!(scale > 0.0))
        badArgs(QStringLiteral("the image size must be positive"));

    const QSet<const void *> before = entityKeys();
    if (!m_native->addImage(path, at, scale, angle, brightness, contrast, fade))
        throw RequestError(QStringLiteral("failed"), m_native->lastError());
    return newEntitiesSince(before);
}

QJsonValue Dispatcher::opDimAligned(const QJsonObject &args)
{
    requireModification();
    const QPointF p1 = requirePoint(args, QStringLiteral("p1"));
    const QPointF p2 = requirePoint(args, QStringLiteral("p2"));
    const QPointF dimLine = requirePoint(args, QStringLiteral("dimline"));
    if (p1 == p2)
        badArgs(QStringLiteral("\"p1\" and \"p2\" must differ"));

    const QSet<const void *> before = entityKeys();
    if (!m_native->dimAligned(p1, p2, dimLine, dimensionText(args)))
        throw RequestError(QStringLiteral("failed"), m_native->lastError());
    return newEntitiesSince(before);
}

QJsonValue Dispatcher::opDimLinear(const QJsonObject &args)
{
    requireModification();
    const QPointF p1 = requirePoint(args, QStringLiteral("p1"));
    const QPointF p2 = requirePoint(args, QStringLiteral("p2"));
    const QPointF dimLine = requirePoint(args, QStringLiteral("dimline"));
    const double angle = optionalNumber(args, QStringLiteral("angle"), 0.0);

    const QSet<const void *> before = entityKeys();
    if (!m_native->dimLinear(p1, p2, dimLine, angle, dimensionText(args)))
        throw RequestError(QStringLiteral("failed"), m_native->lastError());
    return newEntitiesSince(before);
}

QJsonValue Dispatcher::radialDimension(const QJsonObject &args, bool diametric)
{
    requireModification();
    // The circle: a CIRCLE or ARC entity, or a centre and radius.
    QPointF center;
    double radius = 0.0;
    if (args.contains(QStringLiteral("entity"))) {
        Plug_Entity *entity = lookupEntityArg(args, QStringLiteral("entity"));
        QHash<int, QVariant> data;
        entity->getData(&data);
        const int type = data.value(DPI::ETYPE, DPI::UNKNOWN).toInt();
        if (type != DPI::CIRCLE && type != DPI::ARC) {
            badArgs(QStringLiteral("\"entity\" is a %1, not a CIRCLE or ARC")
                        .arg(typeToName(type)));
        }
        center = QPointF(data.value(DPI::STARTX).toDouble(),
                         data.value(DPI::STARTY).toDouble());
        radius = data.value(DPI::RADIUS).toDouble();
    } else {
        center = requirePoint(args, QStringLiteral("center"));
        radius = requireNumber(args, QStringLiteral("radius"));
    }
    if (radius <= 0.0)
        badArgs(QStringLiteral("the radius must be positive"));
    const double angle = optionalNumber(args, QStringLiteral("angle"), M_PI / 4.0);

    const QSet<const void *> before = entityKeys();
    if (!m_native->dimRadial(center, radius, angle, dimensionText(args), diametric))
        throw RequestError(QStringLiteral("failed"), m_native->lastError());
    return newEntitiesSince(before);
}

QJsonValue Dispatcher::opDimRadial(const QJsonObject &args)
{
    return radialDimension(args, false);
}

QJsonValue Dispatcher::opDimDiametric(const QJsonObject &args)
{
    return radialDimension(args, true);
}

QJsonValue Dispatcher::opDimAngular(const QJsonObject &args)
{
    requireModification();
    const auto lookup = [this](const QJsonObject &handle) {
        return lookupEntity(handle);
    };
    QPointF l1a, l1b, l2a, l2b;
    lineArg(args, QStringLiteral("line1"), lookup, &l1a, &l1b);
    lineArg(args, QStringLiteral("line2"), lookup, &l2a, &l2b);
    const QPointF dimLine = requirePoint(args, QStringLiteral("dimline"));

    const QSet<const void *> before = entityKeys();
    if (!m_native->dimAngular(l1a, l1b, l2a, l2b, dimLine, dimensionText(args)))
        throw RequestError(QStringLiteral("failed"), m_native->lastError());
    return newEntitiesSince(before);
}

QJsonValue Dispatcher::opDimLeader(const QJsonObject &args)
{
    requireModification();
    const std::vector<QPointF> points = requirePoints(args, QStringLiteral("points"), 2);
    const bool arrow = optionalBool(args, QStringLiteral("arrow"), true);

    QList<QPointF> list;
    for (const QPointF &point : points)
        list.append(point);
    const QSet<const void *> before = entityKeys();
    if (!m_native->dimLeader(list, arrow))
        throw RequestError(QStringLiteral("failed"), m_native->lastError());
    return newEntitiesSince(before);
}

QJsonValue Dispatcher::opAddHatch(const QJsonObject &args)
{
    requireModification();
    const QJsonArray handles = requireArray(args, QStringLiteral("handles"), 1);
    const QString pattern = optionalString(args, QStringLiteral("pattern"),
                                           QStringLiteral("ANSI31"));
    const double scale = optionalNumber(args, QStringLiteral("scale"), 1.0);
    const double angle = optionalNumber(args, QStringLiteral("angle"), 0.0);
    const bool solid = optionalBool(args, QStringLiteral("solid"), false);
    if (!(scale > 0.0))
        badArgs(QStringLiteral("\"scale\" must be positive"));

    // The hatch action drops texts, points, dimensions and hatches from the
    // selection before it builds the loop; only outline geometry is taken.
    static const QList<int> boundaryTypes{
        DPI::LINE, DPI::ARC, DPI::CIRCLE, DPI::ELLIPSE, DPI::POLYLINE,
        DPI::SPLINE, DPI::SPLINEPOINTS};
    QList<Plug_Entity *> boundary;
    for (int i = 0; i < handles.size(); ++i) {
        if (!handles.at(i).isDouble())
            badArgs(QStringLiteral("\"handles\"[%1] must be a number").arg(i));
        QJsonObject lookup;
        lookup.insert(QStringLiteral("handle"), handles.at(i));
        Plug_Entity *entity = lookupEntity(lookup);
        const int type = readEntityType(entity);
        if (!boundaryTypes.contains(type)) {
            badArgs(QStringLiteral("\"handles\"[%1] is a %2, which cannot bound "
                                   "a hatch").arg(i).arg(typeToName(type)));
        }
        boundary.append(entity);
    }

    const QSet<const void *> before = entityKeys();
    if (!m_native->addHatch(boundary, pattern, scale, angle, solid))
        throw RequestError(QStringLiteral("failed"), m_native->lastError());
    return newEntitiesSince(before);
}

} // namespace lcbridge
