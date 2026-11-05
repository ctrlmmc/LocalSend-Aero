QT += core gui widgets network

TARGET = lsaero
TEMPLATE = app
CONFIG += c++11
CONFIG -= app_bundle

# Vista-era headers keep TaskDialog/DWM declarations available while the code
# still loads every Vista-only API dynamically (Windows XP support).
DEFINES += _WIN32_WINNT=0x0600 WINVER=0x0600 _WIN32_IE=0x0700

RC_FILE = lsaero.rc
RESOURCES += icons.qrc
INCLUDEPATH += .

# OpenSSL ships with Qt's MinGW toolchain (1.0.1j). Qt only needs it for the
# TLS classes; the certificate generation calls the library directly.
OPENSSL_DIR = C:/Qt/Qt5.6.3/Tools/mingw492_32/opt
INCLUDEPATH += $$OPENSSL_DIR/include

# gdi32: region helpers used together with the DWM blur-behind call.
# comctl32: TaskDialogIndirect (Vista+), loaded dynamically so XP still runs.
# ws2_32/crypt32: required by OpenSSL on Windows.
LIBS += -L$$OPENSSL_DIR/lib -lssl -lcrypto -lws2_32 -lcrypt32 -lgdi32 -lcomctl32

SOURCES += \
    main.cpp \
    mainwindow.cpp \
    settings.cpp \
    singleinstance.cpp \
    autostart.cpp \
    theme.cpp \
    logger.cpp \
    protocol.cpp \
    crypto.cpp \
    httpclient.cpp \
    httpserver.cpp \
    discovery.cpp \
    networkservice.cpp \
    sendworker.cpp \
    history.cpp \
    ui.cpp \
    i18n.cpp

HEADERS += \
    appinfo.h \
    mainwindow.h \
    settings.h \
    singleinstance.h \
    autostart.h \
    theme.h \
    logger.h \
    protocol.h \
    crypto.h \
    httpclient.h \
    httpserver.h \
    discovery.h \
    networkservice.h \
    sendworker.h \
    history.h \
    ui.h \
    i18n.h
