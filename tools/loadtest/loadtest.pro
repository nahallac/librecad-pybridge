# Pre-flight check: load the built plugin with QPluginLoader exactly as
# LibreCAD does, and print its name() and getCapabilities() without having to
# start LibreCAD. Catches IID mismatches and Qt version mismatches early.
QT += core gui widgets
CONFIG += console c++17
CONFIG -= app_bundle
TEMPLATE = app
TARGET = loadtest

isEmpty(LIBRECAD_INCLUDE): LIBRECAD_INCLUDE = /usr/include/librecad
INCLUDEPATH += $$LIBRECAD_INCLUDE

MOC_DIR = $$OUT_PWD/.moc
OBJECTS_DIR = $$OUT_PWD/.obj
SOURCES += main.cpp
