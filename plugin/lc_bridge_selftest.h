/*****************************************************************************/
/*  lc_bridge_selftest.h - hard-coded exercise of the dispatch layer         */
/*                                                                           */
/*  Milestone 2 has no transport yet, so the dispatcher is driven by a fixed  */
/*  request sequence. The same sequence runs two ways: against the real       */
/*  document from LibreCAD's Plugins menu, and against a stub document in    */
/*  tools/dispatchtest for a build-time check with no LibreCAD involved.     */
/*                                                                           */
/*  Licensed under the GNU General Public License, version 2 or later.       */
/*****************************************************************************/

#ifndef LC_BRIDGE_SELFTEST_H
#define LC_BRIDGE_SELFTEST_H

#include <QJsonArray>
#include <QString>

namespace lcbridge {

class Dispatcher;

/**
 * Result of running the self-test sequence.
 */
struct SelfTestResult {
    int passed {0};
    int failed {0};
    //! One line per request: the operation, whether it succeeded, and either a
    //! short rendering of the result or the error message.
    QString report;

    bool ok() const { return failed == 0; }
};

//! The request sequence, as it would arrive over the wire.
QJsonArray selfTestRequests();

//! Runs selfTestRequests() through \a dispatcher.
//!
//! A request may carry "expect_error": "<code>" to assert that it fails with
//! that error code; such a request counts as passed when it does. The field is
//! stripped before dispatch, so it never reaches an operation handler.
SelfTestResult runSelfTest(Dispatcher &dispatcher);

} // namespace lcbridge

#endif // LC_BRIDGE_SELFTEST_H
