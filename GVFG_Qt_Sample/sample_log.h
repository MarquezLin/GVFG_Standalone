#pragma once

#include <QFile>
#include <QObject>
#include <QString>

class SampleLog final : public QObject
{
    Q_OBJECT
public:
    explicit SampleLog(QObject *parent = nullptr);
    QString filePath() const { return filePath_; }
    bool fileAvailable() const { return file_.isOpen(); }

public slots:
    void append(const QString &message);
    void appendDiagnostic(const QString &message);

signals:
    void lineReady(const QString &line);

private:
    bool openPart();
    void writeLine(const QString &line);

    static constexpr qint64 kMaxFileBytes = 20ll * 1024ll * 1024ll;
    QFile file_;
    QString directoryPath_, filePath_, sessionStamp_;
    int partIndex_ = 1;
};
