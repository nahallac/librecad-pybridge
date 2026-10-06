/*****************************************************************************/
/*  lc_bridge_dispatch.cpp - named operations over Document_Interface        */
/*                                                                           */
/*  Licensed under the GNU General Public License, version 2 or later.       */
/*****************************************************************************/

#include "lc_bridge_dispatch.h"

#include "lc_bridge_native.h"

#include "document_interface.h"

#include <QJsonArray>
#include <QList>
#include <QPointF>
#include <QVariant>
#include <QtMath>

#include <exception>
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
        {QStringLiteral("cmd_dim"),              &Dispatcher::opCmdDim},
        {QStringLiteral("cmd_hatch"),            &Dispatcher::opCmdHatch},
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
    return toJsonArray(m_doc->getAllBlocks());
}

// --------------------------------------------------------------------------
// Query
// --------------------------------------------------------------------------

QJsonValue Dispatcher::opGetEntities(const QJsonObject &args)
{
    // This is the only non-interactive query in the API. getSelect(),
    // getSelectByType(), and getEnt() all prompt the user and spin a nested
    // event loop, so they are not exposed here; see docs/findings.md, risk 6.
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

        if (!wanted.isEmpty() && !wanted.contains(type)) {
            delete entity;
            continue;
        }

        QJsonObject item;
        item.insert(QStringLiteral("handle"), registerEntity(entity));
        item.insert(QStringLiteral("type"), typeToName(type));
        if (includeData)
            item.insert(QStringLiteral("data"), entityDataToJson(type, data));
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
    result.insert(QStringLiteral("data"), entityDataToJson(type, data));
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

int Dispatcher::countEntitiesOfType(int dpiType)
{
    QList<Plug_Entity *> entities;
    if (!m_doc->getAllEntities(&entities, false))
        return -1;

    int count = 0;
    for (Plug_Entity *entity : entities) {
        if (readEntityType(entity) == dpiType)
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
        if (m_native->setSelected(entity, readEntityType(entity), selected))
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
        if (m_native->setSelected(entity, readEntityType(entity), true))
            ++selected;
    }
    return selected;
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

} // namespace lcbridge
