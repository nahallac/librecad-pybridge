/*****************************************************************************/
/*  fake_document.cpp - stub Document_Interface for testing the dispatcher   */
/*                                                                           */
/*  Licensed under the GNU General Public License, version 2 or later.       */
/*****************************************************************************/

#include "fake_document.h"

#include <QtMath>

// Plug_Entity::getEntityType() is declared virtual but not pure, and LibreCAD
// defines it in its own binary. In a standalone test binary that definition is
// missing, and because it is the class's key function its absence also means
// no vtable or typeinfo for Plug_Entity is emitted -- which deriving from it
// needs. Defining it here supplies all three. This lives only in the test
// harness; the plugin never defines it and takes LibreCAD's.
int Plug_Entity::getEntityType()
{
    return DPI::UNKNOWN;
}

namespace faketest {

// --------------------------------------------------------------------------
// FakeEntity
// --------------------------------------------------------------------------

FakeEntity::FakeEntity(Record *record, FakeDocument *document)
    : m_record(record)
    , m_document(document)
{}

int FakeEntity::getEntityType()
{
    return m_record ? m_record->type : DPI::UNKNOWN;
}

void FakeEntity::getData(QHash<int, QVariant> *data)
{
    if (m_record)
        *data = m_record->data;
}

void FakeEntity::updateData(QHash<int, QVariant> *data)
{
    if (!m_record)
        return;

    // Matches Plugin_Entity::updateData(): the edited entity replaces the
    // original, and the wrapper is left pointing at the original, which is no
    // longer in the drawing.
    Record *replacement = m_document->addRecord(m_record->type);
    replacement->data = m_record->data;
    replacement->vertices = m_record->vertices;
    for (auto it = data->constBegin(); it != data->constEnd(); ++it)
        replacement->data.insert(it.key(), it.value());

    m_record->alive = false;
}

void FakeEntity::getPolylineData(QList<Plug_VertexData> *data)
{
    if (m_record)
        *data = m_record->vertices;
}

void FakeEntity::updatePolylineData(QList<Plug_VertexData> *data)
{
    // In place, like the real thing: no replacement entity, no undo.
    if (m_record)
        m_record->vertices = *data;
}

void FakeEntity::move(QPointF offset, DPI::Disposition disp)
{
    if (!m_record)
        return;

    Record *replacement = m_document->addRecord(m_record->type);
    replacement->data = m_record->data;
    replacement->vertices = m_record->vertices;

    for (const int key : {static_cast<int>(DPI::STARTX), static_cast<int>(DPI::ENDX)}) {
        if (replacement->data.contains(key))
            replacement->data.insert(key, replacement->data.value(key).toDouble() + offset.x());
    }
    for (const int key : {static_cast<int>(DPI::STARTY), static_cast<int>(DPI::ENDY)}) {
        if (replacement->data.contains(key))
            replacement->data.insert(key, replacement->data.value(key).toDouble() + offset.y());
    }

    if (disp == DPI::DELETE_ORIGINAL)
        m_record->alive = false;

    // Re-point at the clone, as Plugin_Entity does.
    m_record = replacement;
}

void FakeEntity::moveRotate(QPointF const &offset, QPointF const &center,
                            double angle, DPI::Disposition disp)
{
    move(offset, disp);
    rotate(center, angle, DPI::DELETE_ORIGINAL);
}

void FakeEntity::rotate(QPointF center, double angle, DPI::Disposition disp)
{
    if (!m_record)
        return;

    Record *replacement = m_document->addRecord(m_record->type);
    replacement->data = m_record->data;
    replacement->vertices = m_record->vertices;

    const auto rotatePair = [&](int xKey, int yKey) {
        if (!replacement->data.contains(xKey) || !replacement->data.contains(yKey))
            return;
        const double x = replacement->data.value(xKey).toDouble() - center.x();
        const double y = replacement->data.value(yKey).toDouble() - center.y();
        replacement->data.insert(xKey, center.x() + x * std::cos(angle) - y * std::sin(angle));
        replacement->data.insert(yKey, center.y() + x * std::sin(angle) + y * std::cos(angle));
    };
    rotatePair(DPI::STARTX, DPI::STARTY);
    rotatePair(DPI::ENDX, DPI::ENDY);

    if (disp == DPI::DELETE_ORIGINAL)
        m_record->alive = false;
    m_record = replacement;
}

void FakeEntity::scale(QPointF center, QPointF factor, DPI::Disposition disp)
{
    if (!m_record)
        return;

    Record *replacement = m_document->addRecord(m_record->type);
    replacement->data = m_record->data;
    replacement->vertices = m_record->vertices;

    const auto scaleAxis = [&](int key, double origin, double scale) {
        if (replacement->data.contains(key)) {
            const double value = replacement->data.value(key).toDouble();
            replacement->data.insert(key, origin + (value - origin) * scale);
        }
    };
    scaleAxis(DPI::STARTX, center.x(), factor.x());
    scaleAxis(DPI::ENDX, center.x(), factor.x());
    scaleAxis(DPI::STARTY, center.y(), factor.y());
    scaleAxis(DPI::ENDY, center.y(), factor.y());
    if (replacement->data.contains(DPI::RADIUS)) {
        replacement->data.insert(DPI::RADIUS,
                                 replacement->data.value(DPI::RADIUS).toDouble() * factor.x());
    }

    if (disp == DPI::DELETE_ORIGINAL)
        m_record->alive = false;
    m_record = replacement;
}

QString FakeEntity::intColor2str(int color)
{
    if (color == -1)
        return QStringLiteral("ByLayer");
    if (color == -2)
        return QStringLiteral("ByBlock");
    return QStringLiteral("#%1").arg(color, 6, 16, QLatin1Char('0'));
}

// --------------------------------------------------------------------------
// FakeDocument
// --------------------------------------------------------------------------

FakeDocument::FakeDocument()
{
    m_layers << QStringLiteral("0");
    m_currentLayer = QStringLiteral("0");
    m_blocks << QStringLiteral("example_block");
}

FakeDocument::~FakeDocument() = default;

void FakeDocument::note(const QString &call)
{
    m_calls << call;
}

int FakeDocument::liveEntityCount() const
{
    int count = 0;
    for (const auto &record : m_records) {
        if (record->alive)
            ++count;
    }
    return count;
}

Record *FakeDocument::addRecord(int type)
{
    m_records.push_back(std::make_unique<Record>());
    Record *record = m_records.back().get();
    record->type = type;
    record->data.insert(DPI::ETYPE, type);
    record->data.insert(DPI::EID, m_nextId++);
    return record;
}

void FakeDocument::applyCurrentAttributes(Record *record)
{
    record->data.insert(DPI::LAYER, m_currentLayer);
    record->data.insert(DPI::COLOR, m_color);
    record->data.insert(DPI::LWIDTH, m_lineweight);
    record->data.insert(DPI::LTYPE, m_linetype);
    record->data.insert(DPI::VISIBLE, 1);
}

void FakeDocument::updateView()
{
    note(QStringLiteral("updateView()"));
}

void FakeDocument::addPoint(QPointF *start)
{
    note(QStringLiteral("addPoint(%1,%2)").arg(start->x()).arg(start->y()));
    Record *record = addRecord(DPI::POINT);
    applyCurrentAttributes(record);
    record->data.insert(DPI::STARTX, start->x());
    record->data.insert(DPI::STARTY, start->y());
}

void FakeDocument::addLine(QPointF *start, QPointF *end)
{
    note(QStringLiteral("addLine(%1,%2 -> %3,%4)")
             .arg(start->x()).arg(start->y()).arg(end->x()).arg(end->y()));
    Record *record = addRecord(DPI::LINE);
    applyCurrentAttributes(record);
    record->data.insert(DPI::STARTX, start->x());
    record->data.insert(DPI::STARTY, start->y());
    record->data.insert(DPI::ENDX, end->x());
    record->data.insert(DPI::ENDY, end->y());
}

void FakeDocument::addText(QString txt, QString sty, QPointF *start, double height,
                           double angle, DPI::HAlign ha, DPI::VAlign va)
{
    note(QStringLiteral("addText(\"%1\", style=%2, h=%3, angle=%4, ha=%5, va=%6)")
             .arg(txt, sty).arg(height).arg(angle).arg(int(ha)).arg(int(va)));
    Record *record = addRecord(DPI::TEXT);
    applyCurrentAttributes(record);
    record->data.insert(DPI::STARTX, start->x());
    record->data.insert(DPI::STARTY, start->y());
    record->data.insert(DPI::TEXTCONTENT, txt);
    record->data.insert(DPI::TXTSTYLE, sty);
    record->data.insert(DPI::HEIGHT, height);
    record->data.insert(DPI::STARTANGLE, angle);
}

void FakeDocument::addCircle(QPointF *start, qreal radius)
{
    note(QStringLiteral("addCircle(%1,%2 r=%3)")
             .arg(start->x()).arg(start->y()).arg(radius));
    Record *record = addRecord(DPI::CIRCLE);
    applyCurrentAttributes(record);
    record->data.insert(DPI::STARTX, start->x());
    record->data.insert(DPI::STARTY, start->y());
    record->data.insert(DPI::RADIUS, radius);
}

void FakeDocument::addArc(QPointF *start, qreal radius, qreal a1, qreal a2)
{
    // a1 and a2 arrive in DEGREES, as the real addArc() does. Stored as
    // radians because that is what getData() reports for an arc.
    note(QStringLiteral("addArc(%1,%2 r=%3 %4deg..%5deg)")
             .arg(start->x()).arg(start->y()).arg(radius).arg(a1).arg(a2));
    Record *record = addRecord(DPI::ARC);
    applyCurrentAttributes(record);
    record->data.insert(DPI::STARTX, start->x());
    record->data.insert(DPI::STARTY, start->y());
    record->data.insert(DPI::RADIUS, radius);
    record->data.insert(DPI::STARTANGLE, qDegreesToRadians(a1));
    record->data.insert(DPI::ENDANGLE, qDegreesToRadians(a2));
    record->data.insert(DPI::REVERSED, false);
}

void FakeDocument::addEllipse(QPointF *start, QPointF *end, qreal ratio, qreal a1,
                              qreal a2)
{
    note(QStringLiteral("addEllipse(%1,%2 major=%3,%4 ratio=%5)")
             .arg(start->x()).arg(start->y()).arg(end->x()).arg(end->y()).arg(ratio));
    Record *record = addRecord(DPI::ELLIPSE);
    applyCurrentAttributes(record);
    record->data.insert(DPI::STARTX, start->x());
    record->data.insert(DPI::STARTY, start->y());
    record->data.insert(DPI::ENDX, end->x());
    record->data.insert(DPI::ENDY, end->y());
    record->data.insert(DPI::HEIGHT, ratio);
    record->data.insert(DPI::STARTANGLE, a1);
    record->data.insert(DPI::ENDANGLE, a2);
    record->data.insert(DPI::REVERSED, false);
}

void FakeDocument::addLines(std::vector<QPointF> const &points, bool closed)
{
    note(QStringLiteral("addLines(%1 points, closed=%2)")
             .arg(points.size()).arg(closed));

    // One LINE per segment, as the real call does.
    const size_t segments = points.size() < 2
                                ? 0
                                : (closed ? points.size() : points.size() - 1);
    for (size_t i = 0; i < segments; ++i) {
        const QPointF &from = points[i];
        const QPointF &to = points[(i + 1) % points.size()];
        Record *record = addRecord(DPI::LINE);
        applyCurrentAttributes(record);
        record->data.insert(DPI::STARTX, from.x());
        record->data.insert(DPI::STARTY, from.y());
        record->data.insert(DPI::ENDX, to.x());
        record->data.insert(DPI::ENDY, to.y());
    }
}

void FakeDocument::addPolyline(std::vector<Plug_VertexData> const &points, bool closed)
{
    note(QStringLiteral("addPolyline(%1 vertices, closed=%2)")
             .arg(points.size()).arg(closed));
    Record *record = addRecord(DPI::POLYLINE);
    applyCurrentAttributes(record);
    record->data.insert(DPI::CLOSEPOLY, closed);
    for (const Plug_VertexData &vertex : points)
        record->vertices.append(vertex);
}

void FakeDocument::addSplinePoints(std::vector<QPointF> const &points, bool closed)
{
    note(QStringLiteral("addSplinePoints(%1 points, closed=%2)")
             .arg(points.size()).arg(closed));
    Record *record = addRecord(DPI::SPLINEPOINTS);
    applyCurrentAttributes(record);
}

void FakeDocument::addImage(int handle, QPointF *start, QPointF *uvr, QPointF *vvr,
                            int w, int h, QString name, int br, int con, int fade)
{
    Q_UNUSED(handle) Q_UNUSED(uvr) Q_UNUSED(vvr)
    Q_UNUSED(br) Q_UNUSED(con) Q_UNUSED(fade)
    note(QStringLiteral("addImage(\"%1\", %2x%3)").arg(name).arg(w).arg(h));
    Record *record = addRecord(DPI::IMAGE);
    applyCurrentAttributes(record);
    record->data.insert(DPI::STARTX, start->x());
    record->data.insert(DPI::STARTY, start->y());
    record->data.insert(DPI::BLKNAME, name);
}

void FakeDocument::addInsert(QString name, QPointF ins, QPointF scale, qreal rot)
{
    note(QStringLiteral("addInsert(\"%1\" at %2,%3)").arg(name).arg(ins.x()).arg(ins.y()));
    Record *record = addRecord(DPI::INSERT);
    applyCurrentAttributes(record);
    record->data.insert(DPI::STARTX, ins.x());
    record->data.insert(DPI::STARTY, ins.y());
    record->data.insert(DPI::BLKNAME, name);
    record->data.insert(DPI::XSCALE, scale.x());
    record->data.insert(DPI::YSCALE, scale.y());
    record->data.insert(DPI::STARTANGLE, rot);
}

QString FakeDocument::addBlockfromFromdisk(QString fullName)
{
    note(QStringLiteral("addBlockfromFromdisk(\"%1\")").arg(fullName));
    if (!fullName.endsWith(QStringLiteral(".dxf"), Qt::CaseInsensitive))
        return QString();
    const QString name = QStringLiteral("block_%1").arg(m_blocks.size());
    m_blocks << name;
    return name;
}

void FakeDocument::addEntity(Plug_Entity *handle)
{
    note(QStringLiteral("addEntity()"));
    if (auto *entity = dynamic_cast<FakeEntity *>(handle)) {
        if (Record *record = entity->record())
            record->alive = true;
    }
}

Plug_Entity *FakeDocument::newEntity(enum DPI::ETYPE type)
{
    note(QStringLiteral("newEntity(%1)").arg(int(type)));
    Record *record = addRecord(type);
    record->alive = false;
    applyCurrentAttributes(record);
    return new FakeEntity(record, this);
}

void FakeDocument::removeEntity(Plug_Entity *ent)
{
    note(QStringLiteral("removeEntity()"));
    if (auto *entity = dynamic_cast<FakeEntity *>(ent)) {
        if (Record *record = entity->record())
            record->alive = false;
    }
}

void FakeDocument::setLayer(QString name)
{
    note(QStringLiteral("setLayer(\"%1\")").arg(name));
    if (!m_layers.contains(name))
        m_layers << name;
    m_currentLayer = name;
}

QString FakeDocument::getCurrentLayer()
{
    note(QStringLiteral("getCurrentLayer()"));
    return m_currentLayer;
}

QStringList FakeDocument::getAllLayer()
{
    note(QStringLiteral("getAllLayer()"));
    return m_layers;
}

QStringList FakeDocument::getAllBlocks()
{
    note(QStringLiteral("getAllBlocks()"));
    return m_blocks;
}

bool FakeDocument::deleteLayer(QString name)
{
    note(QStringLiteral("deleteLayer(\"%1\")").arg(name));
    return m_layers.removeAll(name) > 0;
}

void FakeDocument::getCurrentLayerProperties(int *c, DPI::LineWidth *w, DPI::LineType *t)
{
    note(QStringLiteral("getCurrentLayerProperties(enum)"));
    *c = m_color;
    *w = DPI::WidthByLayer;
    *t = DPI::LineByLayer;
}

void FakeDocument::getCurrentLayerProperties(int *c, QString *w, QString *t)
{
    note(QStringLiteral("getCurrentLayerProperties(string)"));
    *c = m_color;
    *w = m_lineweight;
    *t = m_linetype;
}

void FakeDocument::setCurrentLayerProperties(int c, DPI::LineWidth w, DPI::LineType t)
{
    Q_UNUSED(w) Q_UNUSED(t)
    note(QStringLiteral("setCurrentLayerProperties(enum, color=%1)").arg(c));
    m_color = c;
}

void FakeDocument::setCurrentLayerProperties(int c, QString const &w, QString const &t)
{
    note(QStringLiteral("setCurrentLayerProperties(string, color=%1, w=%2, t=%3)")
             .arg(c).arg(w, t));
    m_color = c;
    m_lineweight = w;
    m_linetype = t;
}

// The interactive calls all report cancellation: there is no user here, and
// the dispatcher is not supposed to be reaching them in the first place.

bool FakeDocument::getPoint(QPointF *point, const QString &message, QPointF *base)
{
    Q_UNUSED(point) Q_UNUSED(message) Q_UNUSED(base)
    note(QStringLiteral("getPoint() [interactive, cancelled]"));
    return false;
}

Plug_Entity *FakeDocument::getEnt(const QString &message)
{
    Q_UNUSED(message)
    note(QStringLiteral("getEnt() [interactive, cancelled]"));
    return nullptr;
}

bool FakeDocument::getSelect(QList<Plug_Entity *> *sel, const QString &message)
{
    Q_UNUSED(sel) Q_UNUSED(message)
    note(QStringLiteral("getSelect() [interactive, cancelled]"));
    return false;
}

bool FakeDocument::getSelectByType(QList<Plug_Entity *> *sel, enum DPI::ETYPE type,
                                   const QString &message)
{
    Q_UNUSED(sel) Q_UNUSED(type) Q_UNUSED(message)
    note(QStringLiteral("getSelectByType() [interactive, cancelled]"));
    return false;
}

bool FakeDocument::getAllEntities(QList<Plug_Entity *> *sel, bool visible)
{
    note(QStringLiteral("getAllEntities(visible=%1)").arg(visible));
    for (const auto &record : m_records) {
        if (!record->alive)
            continue;
        if (visible && record->data.value(DPI::VISIBLE).toInt() == 0)
            continue;
        sel->append(new FakeEntity(record.get(), this));
    }
    return true;
}

void FakeDocument::unselectEntities()
{
    note(QStringLiteral("unselectEntities()"));
}

bool FakeDocument::getVariableInt(const QString &key, int *num)
{
    note(QStringLiteral("getVariableInt(\"%1\")").arg(key));
    const auto it = m_intVariables.constFind(key);
    if (it == m_intVariables.constEnd())
        return false;
    *num = *it;
    return true;
}

bool FakeDocument::getVariableDouble(const QString &key, double *num)
{
    note(QStringLiteral("getVariableDouble(\"%1\")").arg(key));
    const auto it = m_doubleVariables.constFind(key);
    if (it == m_doubleVariables.constEnd())
        return false;
    *num = *it;
    return true;
}

bool FakeDocument::addVariable(const QString &key, int value, int code)
{
    note(QStringLiteral("addVariable(\"%1\", %2, code=%3)").arg(key).arg(value).arg(code));
    m_intVariables.insert(key, value);
    return true;
}

bool FakeDocument::addVariable(const QString &key, double value, int code)
{
    note(QStringLiteral("addVariable(\"%1\", %2, code=%3)").arg(key).arg(value).arg(code));
    m_doubleVariables.insert(key, value);
    return true;
}

bool FakeDocument::getInt(int *num, const QString &message, const QString &title)
{
    Q_UNUSED(num) Q_UNUSED(message) Q_UNUSED(title)
    note(QStringLiteral("getInt() [interactive, cancelled]"));
    return false;
}

bool FakeDocument::getReal(qreal *num, const QString &message, const QString &title)
{
    Q_UNUSED(num) Q_UNUSED(message) Q_UNUSED(title)
    note(QStringLiteral("getReal() [interactive, cancelled]"));
    return false;
}

bool FakeDocument::getString(QString *txt, const QString &message, const QString &title)
{
    Q_UNUSED(txt) Q_UNUSED(message) Q_UNUSED(title)
    note(QStringLiteral("getString() [interactive, cancelled]"));
    return false;
}

QString FakeDocument::realToStr(const qreal num, const int units, const int prec)
{
    note(QStringLiteral("realToStr(%1, units=%2, prec=%3)").arg(num).arg(units).arg(prec));
    return QString::number(num, 'f', prec);
}

} // namespace faketest
