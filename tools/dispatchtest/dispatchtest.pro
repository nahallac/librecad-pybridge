# Offline check for the dispatch layer: links the dispatcher and its self-test
# against a stub Document_Interface, so `make test` needs no LibreCAD process.
QT += core
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
    $$PWD/../../plugin/lc_bridge_dispatch.cpp \
    $$PWD/../../plugin/lc_bridge_selftest.cpp

HEADERS += \
    fake_document.h \
    $$PWD/../../plugin/lc_bridge_dispatch.h \
    $$PWD/../../plugin/lc_bridge_selftest.h
