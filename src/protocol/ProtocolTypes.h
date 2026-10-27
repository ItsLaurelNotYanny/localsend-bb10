#ifndef PROTOCOL_TYPES_H
#define PROTOCOL_TYPES_H

#include <QString>
#include <QByteArray>
#include <QMap>
#include <QList>
#include <QDateTime>
#include <QHostAddress>
#include <QUrl>
#include <QMetaType>

struct DeviceInfo {
    QString alias;
    QString version;
    QString deviceModel;
    QString deviceType;
    QString fingerprint;
    quint16 port;
    QString protocol; // "http" or "https"
    bool download;

    DeviceInfo()
        : port(53317)
        , protocol("http")
        , download(false)
    {
    }
};

struct PeerDevice {
    DeviceInfo info;
    QHostAddress address; // Transport observation, not JSON
    QDateTime lastSeen;

    PeerDevice()
    {
    }
};

struct FileMeta {
    QString id;
    QString fileName;
    QString fileType;
    QString sha256; // Empty sha256 serializes as null
    qint64 size;

    FileMeta()
        : fileType("application/octet-stream")
        , size(0)
    {
    }
};

struct PrepareUploadRequest {
    DeviceInfo info;
    QMap<QString, FileMeta> files; // Keyed by file id; accept <= 1 in MVP
};

struct PrepareUploadResponse {
    QString sessionId;
    QMap<QString, QString> fileTokens; // Map of fileId -> token
};

struct HttpRequestHead {
    QByteArray method;
    QUrl target;
    QMap<QByteArray, QByteArray> headers; // Normalized lowercase keys
    QHostAddress peerAddress;
    qint64 contentLength; // -1 when absent
    bool chunked;
    bool isSecure;
    QString peerFingerprint;

    HttpRequestHead()
        : contentLength(-1)
        , chunked(false)
        , isSecure(false)
    {
    }
};

inline QString getUrlQueryItem(const QUrl &url, const QString &key)
{
#if QT_VERSION < 0x050000
    return url.queryItemValue(key);
#else
    QString query = url.query();
    QStringList pairs = query.split('&', QString::SkipEmptyParts);
    for (int i = 0; i < pairs.size(); ++i) {
        QString pair = pairs.at(i);
        int eqIdx = pair.indexOf('=');
        if (eqIdx >= 0) {
            QString k = pair.left(eqIdx);
            if (k == key) {
                return QUrl::fromPercentEncoding(pair.mid(eqIdx + 1).toUtf8());
            }
        }
    }
    return QString();
#endif
}

inline void setUrlQueryString(QUrl &url, const QString &queryString)
{
#if QT_VERSION < 0x050000
    url.setEncodedQuery(queryString.toUtf8());
#else
    url.setQuery(queryString);
#endif
}

Q_DECLARE_METATYPE(DeviceInfo)
Q_DECLARE_METATYPE(PeerDevice)
Q_DECLARE_METATYPE(FileMeta)
Q_DECLARE_METATYPE(PrepareUploadRequest)
Q_DECLARE_METATYPE(PrepareUploadResponse)
Q_DECLARE_METATYPE(HttpRequestHead)

#endif // PROTOCOL_TYPES_H
