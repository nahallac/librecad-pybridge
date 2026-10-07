# Offline check for the dispatch layer: links the dispatcher and its self-test
# against a stub Document_Interface, so `make test` needs no LibreCAD process.
# The real native layer is replaced by fake_native.cpp: lc_bridge_native.cpp
# is compiled against LibreCAD's source tree, and the stub reports every
# native operation as unavailable, which is what the self-test asserts.
QT += core network widgets
CONFIG += console c++17
CONFIG -= app_bundle
TEMPLATE = app
TARGET = dispatchtest

isEmpty(LIBRECAD_INCLUDE): LIBRECAD_INCLUDE = /usr/include/librecad
INCLUDEPATH += $$LIBRECAD_INCLUDE $$PWD/../../plugin

MOC_DIR = $$OUT_PWD/.moc
OBJECTS_DIR = $$OUT_PWD/.obj

SOURCES += \
    main.cpp \
    fake_document.cpp \
    fake_native.cpp \
    $$PWD/../../plugin/lc_bridge_dispatch.cpp \
    $$PWD/../../plugin/lc_bridge_selftest.cpp \
    $$PWD/../../plugin/lc_bridge_server.cpp \
    $$PWD/../../plugin/lc_bridge_events.cpp

HEADERS += \
    fake_document.h \
    $$PWD/../../plugin/lc_bridge_dispatch.h \
    $$PWD/../../plugin/lc_bridge_native.h \
    $$PWD/../../plugin/lc_bridge_selftest.h \
    $$PWD/../../plugin/lc_bridge_server.h \
    $$PWD/../../plugin/lc_bridge_events.h
