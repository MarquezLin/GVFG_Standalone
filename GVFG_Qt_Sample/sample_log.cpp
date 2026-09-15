#include "sample_log.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QStringList>

namespace
{
QString filePrefix()
{
#if GVFG_INTERNAL_DIAGNOSTICS
    return QStringLiteral("gvfg_qt_preview_debug");
#else
    return QStringLiteral("gvfg_qt_preview");
#endif
}
}

SampleLog::SampleLog(QObject *parent) : QObject(parent)
{
    sessionStamp_ = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"));
    directoryPath_ = QCoreApplication::applicationDirPath() + QStringLiteral("/logs");
    if (QDir().mkpath(directoryPath_)) openPart();
    else filePath_ = directoryPath_;
}

void SampleLog::append(const QString &message)
{
    const QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"));
    const QStringList lines = message.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i)
    {
        const QString line = QStringLiteral("[%1] %2%3")
                                 .arg(timestamp, i == 0 ? QString() : QStringLiteral("  "), lines.at(i));
        writeLine(line);
        emit lineReady(line);
    }
}

void SampleLog::appendDiagnostic(const QString &message)
{
    const QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"));
    const QStringList lines = message.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i)
        writeLine(QStringLiteral("[%1] %2%3").arg(
            timestamp, i == 0 ? QStringLiteral("Diagnostic | ") : QStringLiteral("             "), lines.at(i)));
}

bool SampleLog::openPart()
{
    if (file_.isOpen()) file_.close();
    filePath_ = QStringLiteral("%1/%2_%3_part%4.log")
                    .arg(directoryPath_, filePrefix(), sessionStamp_).arg(partIndex_, 2, 10, QLatin1Char('0'));
    file_.setFileName(filePath_);
    if (!file_.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) return false;
    file_.write(QStringLiteral("\n==== %1 session %2 part %3 ====\n")
                    .arg(filePrefix(), QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")))
                    .arg(partIndex_).toUtf8());
    file_.flush();
    return true;
}

void SampleLog::writeLine(const QString &line)
{
    if (!file_.isOpen()) return;
    if (file_.size() >= kMaxFileBytes)
    {
        file_.write(QStringLiteral("==== log rotated at %1 ====\n")
                        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"))).toUtf8());
        file_.flush();
        ++partIndex_;
        if (!openPart()) return;
    }
    file_.write(line.toUtf8());
    file_.write("\n");
    file_.flush();
}
