#-------------------------------------------------
# LC_PyBridge - LibreCAD scripting bridge plugin
#
# Standalone build: compiles against the LibreCAD plugin headers installed by
# the distro package, so no LibreCAD source tree is needed.
#
# Requires Qt 5 (LibreCAD 2.2.1.x links Qt 5.15). Build with the Qt5 qmake:
#   qmake -query QT_VERSION   # must report 5.x
#-------------------------------------------------

QT          += core gui widgets
TEMPLATE     = lib
CONFIG      += plugin c++17
VERSION      = 0.1.0
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

DESTDIR      = $$OUT_PWD/../build
MOC_DIR      = $$OUT_PWD/.moc
OBJECTS_DIR  = $$OUT_PWD/.obj

SOURCES += \
    lc_pybridge.cpp \
    lc_bridge_dispatch.cpp \
    lc_bridge_selftest.cpp

HEADERS += \
    lc_pybridge.h \
    lc_bridge_dispatch.h \
    lc_bridge_selftest.h
DISTFILES += lc_pybridge.json
