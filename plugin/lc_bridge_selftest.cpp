/*****************************************************************************/
/*  lc_bridge_selftest.cpp - hard-coded exercise of the dispatch layer       */
/*                                                                           */
/*  Licensed under the GNU General Public License, version 2 or later.       */
/*****************************************************************************/

#include "lc_bridge_selftest.h"

#include "lc_bridge_dispatch.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QStringList>
#include <QtMath>

namespace lcbridge {
namespace {

QJsonObject request(const QString &op, const QJsonObject &args = QJsonObject())
{
    QJsonObject object;
    object.insert(QStringLiteral("op"), op);
    if (!args.isEmpty())
        object.insert(QStringLiteral("args"), args);
    return object;
}

//! A request expected to fail with \a code, so the error paths are covered too.
QJsonObject failing(const QString &code, const QString &op,
                    const QJsonObject &args = QJsonObject())
{
    QJsonObject object = request(op, args);
    object.insert(QStringLiteral("expect_error"), code);
    return object;
}

QJsonArray point(double x, double y)
{
    return QJsonArray{x, y};
}

//! Compact one-line rendering of a result for the report.
QString summarise(const QJsonValue &value)
{
    if (value.isNull() || value.isUndefined())
        return QStringLiteral("-");
    if (value.isString())
        return QStringLiteral("\"%1\"").arg(value.toString());
    if (value.isBool())
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    if (value.isDouble())
        return QString::number(value.toDouble());

    const QJsonDocument document = value.isArray()
                                       ? QJsonDocument(value.toArray())
                                       : QJsonDocument(value.toObject());
    QString text = QString::fromUtf8(document.toJson(QJsonDocument::Compact));
    if (text.size() > 160)
        text = text.left(157) + QStringLiteral("...");
    return text;
}

} // namespace

QJsonArray selfTestRequests()
{
    QJsonArray requests;

    // --- metadata and introspection ---------------------------------------
    requests.append(request(QStringLiteral("ping")));
    requests.append(request(QStringLiteral("operations")));

    // --- layers -----------------------------------------------------------
    requests.append(request(QStringLiteral("get_current_layer")));
    requests.append(request(QStringLiteral("set_layer"),
                            {{QStringLiteral("name"), QStringLiteral("LC_BRIDGE_SELFTEST")}}));
    requests.append(request(QStringLiteral("get_current_layer")));
    requests.append(request(QStringLiteral("get_layers")));
    requests.append(request(QStringLiteral("get_layer_properties")));
    requests.append(request(QStringLiteral("set_layer_properties"),
                            {{QStringLiteral("color"), 0x00FF00}}));
    requests.append(request(QStringLiteral("get_layer_properties")));
    requests.append(request(QStringLiteral("get_blocks")));

    // --- creation, one of each shape --------------------------------------
    requests.append(request(QStringLiteral("add_point"),
                            {{QStringLiteral("at"), point(0.0, 0.0)}}));
    requests.append(request(QStringLiteral("add_line"),
                            {{QStringLiteral("start"), point(0.0, 0.0)},
                             {QStringLiteral("end"), point(40.0, 0.0)}}));
    requests.append(request(QStringLiteral("add_circle"),
                            {{QStringLiteral("center"), point(20.0, 20.0)},
                             {QStringLiteral("radius"), 8.0}}));
    // Angles are radians at this layer even though addArc() wants degrees.
    requests.append(request(QStringLiteral("add_arc"),
                            {{QStringLiteral("center"), point(50.0, 20.0)},
                             {QStringLiteral("radius"), 8.0},
                             {QStringLiteral("start_angle"), 0.0},
                             {QStringLiteral("end_angle"), M_PI}}));
    requests.append(request(QStringLiteral("add_ellipse"),
                            {{QStringLiteral("center"), point(80.0, 20.0)},
                             {QStringLiteral("major"), point(12.0, 0.0)},
                             {QStringLiteral("ratio"), 0.5},
                             {QStringLiteral("start_angle"), 0.0},
                             {QStringLiteral("end_angle"), 0.0}}));
    requests.append(request(QStringLiteral("add_text"),
                            {{QStringLiteral("text"), QStringLiteral("dispatch self-test")},
                             {QStringLiteral("at"), point(0.0, 35.0)},
                             {QStringLiteral("height"), 4.0},
                             {QStringLiteral("halign"), QStringLiteral("left")},
                             {QStringLiteral("valign"), QStringLiteral("bottom")}}));

    // --- batch: the path bulk geometry will take --------------------------
    {
        QJsonArray batched;
        for (int i = 0; i < 4; ++i) {
            const double x = 100.0 + i * 10.0;
            batched.append(request(QStringLiteral("add_line"),
                                   {{QStringLiteral("start"), point(x, 0.0)},
                                    {QStringLiteral("end"), point(x, 25.0)}}));
        }
        batched.append(request(QStringLiteral("add_lines"),
                               {{QStringLiteral("points"),
                                 QJsonArray{point(100.0, 0.0), point(130.0, 0.0),
                                            point(130.0, 25.0)}}}));
        batched.append(request(QStringLiteral("add_polyline"),
                               {{QStringLiteral("vertices"),
                                 QJsonArray{point(150.0, 0.0), point(175.0, 0.0),
                                            QJsonArray{175.0, 25.0, 0.4},
                                            point(150.0, 25.0)}},
                                {QStringLiteral("closed"), true}}));
        requests.append(request(QStringLiteral("batch"),
                                {{QStringLiteral("requests"), batched}}));
    }

    // --- query ------------------------------------------------------------
    requests.append(request(QStringLiteral("get_entities"),
                            {{QStringLiteral("types"), QJsonArray{QStringLiteral("CIRCLE")}}}));
    requests.append(request(QStringLiteral("get_entities"),
                            {{QStringLiteral("include_data"), false}}));

    // --- drawing variables and formatting ---------------------------------
    requests.append(request(QStringLiteral("set_variable"),
                            {{QStringLiteral("key"), QStringLiteral("$LC_BRIDGE_PROBE")},
                             {QStringLiteral("type"), QStringLiteral("int")},
                             {QStringLiteral("value"), 7}}));
    requests.append(request(QStringLiteral("get_variable"),
                            {{QStringLiteral("key"), QStringLiteral("$LC_BRIDGE_PROBE")},
                             {QStringLiteral("type"), QStringLiteral("int")}}));
    requests.append(request(QStringLiteral("real_to_string"),
                            {{QStringLiteral("value"), 12.3456},
                             {QStringLiteral("units"), 2},
                             {QStringLiteral("precision"), 2}}));

    // --- error paths ------------------------------------------------------
    requests.append(failing(QStringLiteral("unknown_op"),
                            QStringLiteral("no_such_operation")));
    requests.append(failing(QStringLiteral("bad_args"), QStringLiteral("add_line"),
                            {{QStringLiteral("start"), point(0.0, 0.0)}}));
    requests.append(failing(QStringLiteral("bad_args"), QStringLiteral("add_line"),
                            {{QStringLiteral("start"), point(0.0, 0.0)},
                             {QStringLiteral("end"), QJsonArray{1.0, 2.0, 3.0}}}));
    requests.append(failing(QStringLiteral("bad_args"), QStringLiteral("add_circle"),
                            {{QStringLiteral("center"), point(0.0, 0.0)},
                             {QStringLiteral("radius"), QStringLiteral("big")}}));
    requests.append(failing(QStringLiteral("bad_args"), QStringLiteral("add_text"),
                            {{QStringLiteral("text"), QStringLiteral("x")},
                             {QStringLiteral("at"), point(0.0, 0.0)},
                             {QStringLiteral("height"), 1.0},
                             {QStringLiteral("halign"), QStringLiteral("sideways")}}));
    requests.append(failing(QStringLiteral("no_such_handle"),
                            QStringLiteral("entity_data"),
                            {{QStringLiteral("handle"), 999999}}));
    requests.append(failing(QStringLiteral("bad_args"), QStringLiteral("get_entities"),
                            {{QStringLiteral("types"), QJsonArray{QStringLiteral("SQUIGGLE")}}}));

    // --- cleanup ----------------------------------------------------------
    requests.append(request(QStringLiteral("release_handles")));
    requests.append(request(QStringLiteral("update_view")));

    return requests;
}

namespace {

//! Dispatches requests and scores them against expectations, accumulating the
//! report. Used for the entity phase, where a request has to carry a handle
//! that only becomes known once the previous request has run.
class Runner
{
public:
    explicit Runner(Dispatcher &dispatcher)
        : m_dispatcher(dispatcher)
    {}

    //! Dispatches \a request, expecting success, and returns its "result".
    QJsonValue expectOk(QJsonObject request, const QString &note = QString())
    {
        const QString op = request.value(QStringLiteral("op")).toString();
        const QJsonObject response = m_dispatcher.dispatch(request);

        if (response.value(QStringLiteral("ok")).toBool()) {
            pass(QStringLiteral("%1%2 -> %3")
                     .arg(op, note.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(note),
                          summarise(response.value(QStringLiteral("result")))));
            return response.value(QStringLiteral("result"));
        }

        const QJsonObject error = response.value(QStringLiteral("error")).toObject();
        fail(QStringLiteral("%1 -> %2: %3")
                 .arg(op, error.value(QStringLiteral("code")).toString(),
                      error.value(QStringLiteral("message")).toString()));
        return QJsonValue();
    }

    //! Dispatches \a request, expecting it to fail with \a code.
    void expectError(const QString &code, QJsonObject request,
                     const QString &note = QString())
    {
        const QString op = request.value(QStringLiteral("op")).toString();
        const QJsonObject response = m_dispatcher.dispatch(request);
        const QString actual = response.value(QStringLiteral("error"))
                                   .toObject()
                                   .value(QStringLiteral("code"))
                                   .toString();

        const QString label = note.isEmpty() ? op : QStringLiteral("%1 (%2)").arg(op, note);
        if (response.value(QStringLiteral("ok")).toBool())
            fail(QStringLiteral("%1 -> succeeded, expected error %2").arg(label, code));
        else if (actual != code)
            fail(QStringLiteral("%1 -> error %2, expected %3").arg(label, actual, code));
        else
            pass(QStringLiteral("%1 -> rejected as %2").arg(label, actual));
    }

    void pass(const QString &message)
    {
        ++m_result.passed;
        m_lines << QStringLiteral("%1. ok    %2").arg(++m_index, 2).arg(message);
    }

    void fail(const QString &message)
    {
        ++m_result.failed;
        m_lines << QStringLiteral("%1. FAIL  %2").arg(++m_index, 2).arg(message);
    }

    void section(const QString &title)
    {
        m_lines << QString();
        m_lines << QStringLiteral("-- %1").arg(title);
    }

    SelfTestResult finish()
    {
        m_lines << QString();
        m_lines << QStringLiteral("%1 passed, %2 failed")
                       .arg(m_result.passed).arg(m_result.failed);
        m_result.report = m_lines.join(QLatin1Char('\n'));
        return m_result;
    }

private:
    Dispatcher &m_dispatcher;
    SelfTestResult m_result;
    QStringList m_lines;
    int m_index {0};
};

//! Finds the handle of the first entity of \a type in a get_entities result,
//! or -1 when there is none.
int firstHandle(const QJsonValue &entities, const QString &type)
{
    const QJsonArray array = entities.toArray();
    for (int i = 0; i < array.size(); ++i) {
        const QJsonObject item = array.at(i).toObject();
        if (item.value(QStringLiteral("type")).toString() == type)
            return item.value(QStringLiteral("handle")).toInt();
    }
    return -1;
}

QJsonObject handleRequest(const QString &op, int handle,
                          QJsonObject args = QJsonObject())
{
    args.insert(QStringLiteral("handle"), handle);
    return request(op, args);
}

//! The second phase: operations that need a handle obtained at run time.
//!
//! This is where the two awkward lifetime rules are checked, because they are
//! the easiest things in the layer to get wrong:
//!   - move/rotate/scale leave the handle usable, because Plugin_Entity
//!     re-points its wrapper at the modified clone;
//!   - entity_update and entity_remove do not, so the dispatcher drops the
//!     handle and a later use must be refused rather than touching an entity
//!     that is no longer in the drawing.
void runEntityPhase(Runner &runner)
{
    runner.section(QStringLiteral("entity handles"));

    const QJsonValue lines = runner.expectOk(
        request(QStringLiteral("get_entities"),
                {{QStringLiteral("types"), QJsonArray{QStringLiteral("LINE")}}}),
        QStringLiteral("LINE"));

    const int line = firstHandle(lines, QStringLiteral("LINE"));
    if (line < 0) {
        runner.fail(QStringLiteral("get_entities returned no LINE to work with"));
        return;
    }

    runner.expectOk(handleRequest(QStringLiteral("entity_data"), line));

    runner.expectOk(handleRequest(QStringLiteral("entity_move"), line,
                                  {{QStringLiteral("offset"), point(5.0, 5.0)}}));
    runner.expectOk(handleRequest(QStringLiteral("entity_data"), line),
                    QStringLiteral("handle survived move"));

    runner.expectOk(handleRequest(QStringLiteral("entity_rotate"), line,
                                  {{QStringLiteral("center"), point(0.0, 0.0)},
                                   {QStringLiteral("angle"), M_PI / 2.0}}));
    runner.expectOk(handleRequest(QStringLiteral("entity_scale"), line,
                                  {{QStringLiteral("center"), point(0.0, 0.0)},
                                   {QStringLiteral("factor"), point(2.0, 2.0)}}));
    runner.expectOk(handleRequest(QStringLiteral("entity_move_rotate"), line,
                                  {{QStringLiteral("offset"), point(1.0, 1.0)},
                                   {QStringLiteral("center"), point(0.0, 0.0)},
                                   {QStringLiteral("angle"), -M_PI / 2.0}}));
    runner.expectOk(handleRequest(QStringLiteral("entity_data"), line),
                    QStringLiteral("handle survived rotate/scale"));

    // Attribute validation: unknown names and read-only names are refused
    // rather than quietly dropped.
    runner.expectError(QStringLiteral("bad_args"),
                       handleRequest(QStringLiteral("entity_update"), line,
                                     {{QStringLiteral("data"),
                                       QJsonObject{{QStringLiteral("radius"), 3.0}}}}),
                       QStringLiteral("radius is not a LINE attribute"));
    runner.expectError(QStringLiteral("bad_args"),
                       handleRequest(QStringLiteral("entity_update"), line,
                                     {{QStringLiteral("data"),
                                       QJsonObject{{QStringLiteral("id"), 5}}}}),
                       QStringLiteral("id is read-only"));

    runner.expectOk(handleRequest(QStringLiteral("entity_update"), line,
                                  {{QStringLiteral("data"),
                                    QJsonObject{{QStringLiteral("end_x"), 99.0},
                                                {QStringLiteral("color"), 0xFF0000}}}}));
    runner.expectError(QStringLiteral("no_such_handle"),
                       handleRequest(QStringLiteral("entity_data"), line),
                       QStringLiteral("handle dropped after update"));

    runner.section(QStringLiteral("polylines"));

    const QJsonValue polylines = runner.expectOk(
        request(QStringLiteral("get_entities"),
                {{QStringLiteral("types"), QJsonArray{QStringLiteral("POLYLINE")}}}),
        QStringLiteral("POLYLINE"));

    const int polyline = firstHandle(polylines, QStringLiteral("POLYLINE"));
    if (polyline < 0) {
        runner.fail(QStringLiteral("get_entities returned no POLYLINE to work with"));
    } else {
        runner.expectOk(handleRequest(QStringLiteral("entity_polyline"), polyline));
        runner.expectOk(handleRequest(QStringLiteral("entity_set_polyline"), polyline,
                                      {{QStringLiteral("vertices"),
                                        QJsonArray{point(150.0, 0.0), point(180.0, 0.0),
                                                   point(180.0, 30.0)}}}));
        runner.expectOk(handleRequest(QStringLiteral("entity_polyline"), polyline),
                        QStringLiteral("after rewrite"));
    }

    runner.section(QStringLiteral("removal"));

    const QJsonValue points = runner.expectOk(
        request(QStringLiteral("get_entities"),
                {{QStringLiteral("types"), QJsonArray{QStringLiteral("POINT")}}}),
        QStringLiteral("POINT"));

    const int pointHandle = firstHandle(points, QStringLiteral("POINT"));
    if (pointHandle < 0) {
        runner.fail(QStringLiteral("get_entities returned no POINT to work with"));
    } else {
        runner.expectOk(handleRequest(QStringLiteral("entity_remove"), pointHandle));
        runner.expectError(QStringLiteral("no_such_handle"),
                           handleRequest(QStringLiteral("entity_data"), pointHandle),
                           QStringLiteral("handle dropped after remove"));
    }

    runner.expectOk(request(QStringLiteral("release_handles")));
}

} // namespace

SelfTestResult runSelfTest(Dispatcher &dispatcher)
{
    Runner runner(dispatcher);
    runner.section(QStringLiteral("fixed request sequence"));

    const QJsonArray requests = selfTestRequests();
    for (int i = 0; i < requests.size(); ++i) {
        QJsonObject request = requests.at(i).toObject();
        const QString expectedError =
            request.take(QStringLiteral("expect_error")).toString();

        if (expectedError.isEmpty())
            runner.expectOk(request);
        else
            runner.expectError(expectedError, request);
    }

    runEntityPhase(runner);
    return runner.finish();
}

} // namespace lcbridge
