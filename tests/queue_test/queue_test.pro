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
    ./../../tp/queue.hpp \
    ./../../hp/queue.hpp \
    ./../../locked/queue.hpp \
    ./../../other/queue.hpp

SOURCES += main.cpp

