/*****************************************************************************/
/*  fake_document.h - stub Document_Interface for testing the dispatcher     */
/*                                                                           */
/*  Lets the dispatch layer be exercised in a plain console binary, with no   */
/*  LibreCAD process involved. The stub is not a CAD engine: it records what  */
/*  was asked of it and stores entity attributes faithfully enough that       */
/*  get_entities and entity_data round-trip through the real attribute        */
/*  tables.                                                                  */
/*                                                                           */
/*  Licensed under the GNU General Public License, version 2 or later.       */
/*****************************************************************************/

#ifndef FAKE_DOCUMENT_H
#define FAKE_DOCUMENT_H

#include "document_interface.h"

#include <QHash>
#include <QList>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <memory>
#include <vector>

namespace faketest {

//! One entity as the stub stores it.
struct Record {
    int type {DPI::UNKNOWN};
    QHash<int, QVariant> data;
    QList<Plug_VertexData> vertices;
    bool alive {true};
};

class FakeDocument;

//! Plug_Entity over a Record. Mirrors Plugin_Entity's observable behaviour,
//! including the detail that updateData() does not re-point the wrapper while
//! move/rotate/scale do.
class FakeEntity : public Plug_Entity
{
public:
    FakeEntity(Record *record, FakeDocument *document);

    int getEntityType() override;
    void getData(QHash<int, QVariant> *data) override;
    void updateData(QHash<int, QVariant> *data) override;
    void getPolylineData(QList<Plug_VertexData> *data) override;
    void updatePolylineData(QList<Plug_VertexData> *data) override;
    void move(QPointF offset, DPI::Disposition disp) override;
    void moveRotate(QPointF const &offset, QPointF const &center, double angle,
                    DPI::Disposition disp) override;
    void rotate(QPointF center, double angle, DPI::Disposition disp) override;
    void scale(QPointF center, QPointF factor, DPI::Disposition disp) override;
    QString intColor2str(int color) override;

    Record *record() const { return m_record; }

private:
    Record *m_record {nullptr};
    FakeDocument *m_document {nullptr};
};

class FakeDocument : public Document_Interface
{
public:
    FakeDocument();
    ~FakeDocument() override;

    //! Every call the dispatcher made, in order, as "name(detail)".
    const QStringList &calls() const { return m_calls; }
    //! Entities still in the drawing.
    int liveEntityCount() const;

    Record *addRecord(int type);

    void updateView() override;

    void addPoint(QPointF *start) override;
    void addLine(QPointF *start, QPointF *end) override;
    void addText(QString txt, QString sty, QPointF *start, double height,
                 double angle, DPI::HAlign ha, DPI::VAlign va) override;
    void addCircle(QPointF *start, qreal radius) override;
    void addArc(QPointF *start, qreal radius, qreal a1, qreal a2) override;
    void addEllipse(QPointF *start, QPointF *end, qreal ratio, qreal a1,
                    qreal a2) override;
    void addLines(std::vector<QPointF> const &points, bool closed) override;
    void addPolyline(std::vector<Plug_VertexData> const &points, bool closed) override;
    void addSplinePoints(std::vector<QPointF> const &points, bool closed) override;
    void addImage(int handle, QPointF *start, QPointF *uvr, QPointF *vvr, int w,
                  int h, QString name, int br, int con, int fade) override;
    void addInsert(QString name, QPointF ins, QPointF scale, qreal rot) override;
    QString addBlockfromFromdisk(QString fullName) override;
    void addEntity(Plug_Entity *handle) override;
    Plug_Entity *newEntity(enum DPI::ETYPE type) override;
    void removeEntity(Plug_Entity *ent) override;

    void setLayer(QString name) override;
    QString getCurrentLayer() override;
    QStringList getAllLayer() override;
    QStringList getAllBlocks() override;
    bool deleteLayer(QString name) override;

    void getCurrentLayerProperties(int *c, DPI::LineWidth *w, DPI::LineType *t) override;
    void getCurrentLayerProperties(int *c, QString *w, QString *t) override;
    void setCurrentLayerProperties(int c, DPI::LineWidth w, DPI::LineType t) override;
    void setCurrentLayerProperties(int c, QString const &w, QString const &t) override;

    bool getPoint(QPointF *point, const QString &message, QPointF *base) override;
    Plug_Entity *getEnt(const QString &message) override;
    bool getSelect(QList<Plug_Entity *> *sel, const QString &message) override;
    bool getSelectByType(QList<Plug_Entity *> *sel, enum DPI::ETYPE type,
                         const QString &message) override;
    bool getAllEntities(QList<Plug_Entity *> *sel, bool visible) override;
    void unselectEntities() override;

    bool getVariableInt(const QString &key, int *num) override;
    bool getVariableDouble(const QString &key, double *num) override;
    bool addVariable(const QString &key, int value, int code) override;
    bool addVariable(const QString &key, double value, int code) override;

    bool getInt(int *num, const QString &message, const QString &title) override;
    bool getReal(qreal *num, const QString &message, const QString &title) override;
    bool getString(QString *txt, const QString &message, const QString &title) override;

    QString realToStr(const qreal num, const int units, const int prec) override;

private:
    void note(const QString &call);
    //! Stamps the attributes every entity carries, using the current layer and
    //! layer properties, the way LibreCAD's "current attributes" behave.
    void applyCurrentAttributes(Record *record);

    QStringList m_calls;
    std::vector<std::unique_ptr<Record>> m_records;
    qulonglong m_nextId {1};

    QStringList m_layers;
    QString m_currentLayer;
    int m_color {-1};
    QString m_lineweight {QStringLiteral("ByLayer")};
    QString m_linetype {QStringLiteral("ByLayer")};

    QStringList m_blocks;
    QHash<QString, int> m_intVariables;
    QHash<QString, double> m_doubleVariables;
};

} // namespace faketest

#endif // FAKE_DOCUMENT_H
