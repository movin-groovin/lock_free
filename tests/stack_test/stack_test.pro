#QMAKE_CXX = GCC7

TEMPLATE = app
CONFIG += console
CONFIG -= app_bundle
CONFIG -= qt

QMAKE_CXXFLAGS += -std=c++14 -Wall -Wextra -pedantic -O3 -pthread
QMAKE_LFLAGS += -lpthread
INCLUDEPATH += ./../../
DESTDIR = build
OBJECTS_DIR = build

HEADERS += ./../../technical.hpp \
    ./../../tp/stack.hpp \
    ./../../hp/stack.hpp \
    ./../../locked/stack.hpp

SOURCES += main.cpp

