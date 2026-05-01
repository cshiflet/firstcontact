#include "Logger.h"

#include "Paths.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QString>
#include <QTextStream>
#include <QtGlobal>

#include <cstdio>

namespace fc::util {

namespace {

constexpr qint64 kRotateSizeBytes = 2 * 1024 * 1024;  // 2 MB

QFile* gLogFile() {
    static QFile file;
    return &file;
}

QMutex& gMutex() {
    static QMutex m;
    return m;
}

void rotateIfNeeded(QFile& file) {
    if (file.size() < kRotateSizeBytes) return;
    file.close();
    const QString rotated = file.fileName() + QStringLiteral(".1");
    QFile::remove(rotated);
    QFile::rename(file.fileName(), rotated);
    file.open(QIODevice::Append | QIODevice::Text);
}

const char* levelTag(QtMsgType t) {
    switch (t) {
        case QtDebugMsg:    return "DEBUG";
        case QtInfoMsg:     return "INFO ";
        case QtWarningMsg:  return "WARN ";
        case QtCriticalMsg: return "ERROR";
        case QtFatalMsg:    return "FATAL";
    }
    return "?????";
}

void handler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg) {
    QMutexLocker lock(&gMutex());
    QFile& file = *gLogFile();
    if (file.isOpen()) {
        rotateIfNeeded(file);
        QTextStream out(&file);
        out << QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)
            << ' ' << levelTag(type)
            << ' ' << (ctx.category ? ctx.category : "default")
            << " - " << msg << '\n';
        out.flush();
    }
#ifndef NDEBUG
    std::fprintf(stderr, "%s %s\n", levelTag(type), msg.toUtf8().constData());
#endif
    if (type == QtFatalMsg) std::abort();
}

}  // namespace

void installLogger() {
    static bool installed = false;
    if (installed) return;
    installed = true;

    QFile& file = *gLogFile();
    file.setFileName(logDir() + QStringLiteral("/firstcontact.log"));
    file.open(QIODevice::Append | QIODevice::Text);
    qInstallMessageHandler(handler);
}

}  // namespace fc::util
