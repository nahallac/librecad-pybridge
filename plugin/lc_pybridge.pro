#-------------------------------------------------
# LC_PyBridge - LibreCAD scripting bridge plugin
#
# Compiles against the LibreCAD plugin headers installed by the distro package
# and, for the native layer (lc_bridge_native.cpp), against LibreCAD's own
# source tree: engine, main window, and generated ui_*.h headers.
#
# Requires Qt 5 (LibreCAD 2.2.1.x links Qt 5.15). Build with the Qt5 qmake:
#   qmake -query QT_VERSION   # must report 5.x
#-------------------------------------------------

QT          += core gui widgets network printsupport
TEMPLATE     = lib
CONFIG      += plugin c++17
VERSION      = 0.2.0
TARGET       = $$qtLibraryTarget(lc_pybridge)

lessThan(QT_MAJOR_VERSION, 5)|greaterThan(QT_MAJOR_VERSION, 5) {
    error("LC_PyBridge must be built with Qt 5 (found Qt $$QT_VERSION). \
LibreCAD 2.2.1.x links Qt 5.15; a Qt 6 plugin will not load.")
}

# LibreCAD plugin headers. Override on the command line if they live elsewhere:
#   qmake LIBRECAD_INCLUDE=/path/to/headers
isEmpty(LIBRECAD_INCLUDE): LIBRECAD_INCLUDE = /usr/include/librecad
!exists($$LIBRECAD_INCLUDE/qc_plugininterface.h) {
    error("LibreCAD plugin headers not found in $$LIBRECAD_INCLUDE. \
Pass LIBRECAD_INCLUDE=<dir> to qmake.")
}
INCLUDEPATH += $$LIBRECAD_INCLUDE

# LibreCAD source tree, for the native layer. It must be the source of the
# LibreCAD that will load the plugin: the layer binds to class layouts and
# vtables, not exported symbols. On Arch the librecad-debug package installs
# the exact sources of the installed binary here. Override:
#   qmake LIBRECAD_SRC=/path/to/LibreCAD
# LIBRECAD_VERSION is what QCoreApplication::applicationVersion() reports in
# that build (main.cpp sets it from LC_VERSION); the plugin refuses native
# access at runtime unless the running LibreCAD reports the same string.
isEmpty(LIBRECAD_SRC): LIBRECAD_SRC = /usr/src/debug/librecad/LibreCAD
isEmpty(LIBRECAD_VERSION): LIBRECAD_VERSION = v2.2.1.5
LIBRECAD_SRC_LIB = $$LIBRECAD_SRC/librecad/src
!exists($$LIBRECAD_SRC_LIB/main/doc_plugin_interface.h) {
    error("LibreCAD source tree not found at $$LIBRECAD_SRC \
(missing librecad/src/main/doc_plugin_interface.h). On Arch install \
librecad-debug, or pass LIBRECAD_SRC=<dir> to qmake.")
}
!exists($$LIBRECAD_SRC/generated/librecad/ui/ui_qg_dlghatch.h) {
    error("Generated ui headers not found under $$LIBRECAD_SRC/generated. \
The native layer needs a built tree (uic output), not a bare checkout.")
}
INCLUDEPATH += \
    $$LIBRECAD_SRC_LIB/lib/engine \
    $$LIBRECAD_SRC_LIB/lib/math \
    $$LIBRECAD_SRC_LIB/lib/gui \
    $$LIBRECAD_SRC_LIB/lib/fileio \
    $$LIBRECAD_SRC_LIB/lib/filters \
    $$LIBRECAD_SRC_LIB/lib/actions \
    $$LIBRECAD_SRC_LIB/lib/information \
    $$LIBRECAD_SRC_LIB/lib/modification \
    $$LIBRECAD_SRC_LIB/lib/creation \
    $$LIBRECAD_SRC_LIB/main \
    $$LIBRECAD_SRC_LIB/actions \
    $$LIBRECAD_SRC_LIB/ui \
    $$LIBRECAD_SRC_LIB/ui/forms \
    $$LIBRECAD_SRC/generated/librecad/ui
DEFINES += LC_PYBRIDGE_LIBRECAD_VERSION=\\\"$$LIBRECAD_VERSION\\\"

DESTDIR      = $$OUT_PWD/../build
MOC_DIR      = $$OUT_PWD/.moc
OBJECTS_DIR  = $$OUT_PWD/.obj

SOURCES += \
    lc_pybridge.cpp \
    lc_bridge_dispatch.cpp \
    lc_bridge_native.cpp \
    lc_bridge_server.cpp \
    lc_bridge_events.cpp

HEADERS += \
    lc_pybridge.h \
    lc_bridge_dispatch.h \
    lc_bridge_native.h \
    lc_bridge_server.h \
    lc_bridge_events.h
DISTFILES += lc_pybridge.json
