#QMAKE_CXX = GCC7

TEMPLATE = app
CONFIG += console
CONFIG -= app_bundle
CONFIG -= qt

#QMAKE_CFLAGS += -static
#QMAKE_CXXFLAGS += -static-libstdc++
QMAKE_CXXFLAGS += -std=c++14 -Wall -Wextra -pedantic -O3 -pthread
QMAKE_LFLAGS += -lpthread
INCLUDEPATH += ./../../
DESTDIR = build
OBJECTS_DIR = build

DEFINES += NDEBUG

HEADERS += ./../../technical.hpp \
    ./../../hp/hash_set.hpp \
    ./../../locked/hash_set.hpp

SOURCES += main.cpp

