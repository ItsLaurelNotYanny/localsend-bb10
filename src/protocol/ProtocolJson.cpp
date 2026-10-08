#include "protocol/ProtocolJson.h"

#include <QVariant>
#include <QVariantMap>
#include <QVariantList>
#include <QRegExp>

#if defined(Q_OS_BLACKBERRY)
#include <bb/data/JsonDataAccess>
#endif

namespace {

// Lightweight, safe JSON parser and serializer for QVariantMap / QVariantList.
// Designed for Qt4 / C++03, fully supporting UTF-8, 64-bit integers, and escapes.

class JsonHelper {
public:
    static bool parse(const QByteArray &json, QVariant *out, QString *error)
    {
        int pos = 0;
        skipWhitespace(json, &pos);
        if (pos >= json.size()) {
            if (error) *error = "Empty JSON input";
            return false;
        }

        if (!parseValue(json, &pos, out, error)) {
            return false;
        }

        skipWhitespace(json, &pos);
        if (pos < json.size()) {
            if (error) *error = "Unexpected trailing data after JSON";
            return false;
        }
        return true;
    }

    static QByteArray stringify(const QVariant &val)
    {
        QByteArray out;
        serializeValue(val, &out);
        return out;
    }

private:
    static void skipWhitespace(const QByteArray &json, int *pos)
    {
        while (*pos < json.size()) {
            char c = json.at(*pos);
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                (*pos)++;
            } else {
                break;
            }
        }
    }

    static bool parseValue(const QByteArray &json, int *pos, QVariant *out, QString *error)
    {
        skipWhitespace(json, pos);
        if (*pos >= json.size()) {
            if (error) *error = "Unexpected end of input";
            return false;
        }

        char c = json.at(*pos);
        if (c == '{') {
            return parseObject(json, pos, out, error);
        } else if (c == '[') {
            return parseArray(json, pos, out, error);
        } else if (c == '"') {
            QString str;
            if (!parseString(json, pos, &str, error)) return false;
            *out = str;
            return true;
        } else if (c == 't' || c == 'f') {
            return parseBool(json, pos, out, error);
        } else if (c == 'n') {
            return parseNull(json, pos, out, error);
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            return parseNumber(json, pos, out, error);
        }

        if (error) *error = QString("Unexpected character: '%1' at position %2").arg(c).arg(*pos);
        return false;
    }

    static bool parseObject(const QByteArray &json, int *pos, QVariant *out, QString *error)
    {
        (*pos)++; // Skip '{'
        QVariantMap map;
        skipWhitespace(json, pos);

        if (*pos < json.size() && json.at(*pos) == '}') {
            (*pos)++;
            *out = map;
            return true;
        }

        while (*pos < json.size()) {
            skipWhitespace(json, pos);
            if (*pos >= json.size() || json.at(*pos) != '"') {
                if (error) *error = "Expected string key in object";
                return false;
            }

            QString key;
            if (!parseString(json, pos, &key, error)) return false;

            skipWhitespace(json, pos);
            if (*pos >= json.size() || json.at(*pos) != ':') {
                if (error) *error = "Expected ':' after object key";
                return false;
            }
            (*pos)++; // Skip ':'

            QVariant val;
            if (!parseValue(json, pos, &val, error)) return false;
            map.insert(key, val);

            skipWhitespace(json, pos);
            if (*pos < json.size() && json.at(*pos) == ',') {
                (*pos)++;
                continue;
            } else if (*pos < json.size() && json.at(*pos) == '}') {
                (*pos)++;
                *out = map;
                return true;
            } else {
                if (error) *error = "Expected ',' or '}' in object";
                return false;
            }
        }

        if (error) *error = "Unterminated object";
        return false;
    }

    static bool parseArray(const QByteArray &json, int *pos, QVariant *out, QString *error)
    {
        (*pos)++; // Skip '['
        QVariantList list;
        skipWhitespace(json, pos);

        if (*pos < json.size() && json.at(*pos) == ']') {
            (*pos)++;
            *out = list;
            return true;
        }

        while (*pos < json.size()) {
            QVariant val;
            if (!parseValue(json, pos, &val, error)) return false;
            list.append(val);

            skipWhitespace(json, pos);
            if (*pos < json.size() && json.at(*pos) == ',') {
                (*pos)++;
                continue;
            } else if (*pos < json.size() && json.at(*pos) == ']') {
                (*pos)++;
                *out = list;
                return true;
            } else {
                if (error) *error = "Expected ',' or ']' in array";
                return false;
            }
        }

        if (error) *error = "Unterminated array";
        return false;
    }

    static bool parseString(const QByteArray &json, int *pos, QString *out, QString *error)
    {
        (*pos)++; // Skip opening quote
        QString result;

        while (*pos < json.size()) {
            char c = json.at(*pos);
            if (c == '"') {
                (*pos)++;
                *out = result;
                return true;
            }
            if (c == '\\') {
                (*pos)++;
                if (*pos >= json.size()) {
                    if (error) *error = "Unterminated escape sequence";
                    return false;
                }
                char esc = json.at(*pos);
                if (esc == '"' || esc == '\\' || esc == '/') {
                    result.append(QChar(esc));
                    (*pos)++;
                } else if (esc == 'b') {
                    result.append(QChar('\b'));
                    (*pos)++;
                } else if (esc == 'f') {
                    result.append(QChar('\f'));
                    (*pos)++;
                } else if (esc == 'n') {
                    result.append(QChar('\n'));
                    (*pos)++;
                } else if (esc == 'r') {
                    result.append(QChar('\r'));
                    (*pos)++;
                } else if (esc == 't') {
                    result.append(QChar('\t'));
                    (*pos)++;
                } else if (esc == 'u') {
                    (*pos)++;
                    if (*pos + 4 > json.size()) {
                        if (error) *error = "Incomplete unicode escape";
                        return false;
                    }
                    bool ok = false;
                    ushort code = json.mid(*pos, 4).toUShort(&ok, 16);
                    if (!ok) {
                        if (error) *error = "Invalid unicode escape sequence";
                        return false;
                    }
                    result.append(QChar(code));
                    *pos += 4;
                } else {
                    if (error) *error = QString("Invalid escape character: %1").arg(esc);
                    return false;
                }
            } else {
                // Read raw UTF-8 sequence
                int start = *pos;
                while (*pos < json.size() && json.at(*pos) != '"' && json.at(*pos) != '\\') {
                    (*pos)++;
                }
                result.append(QString::fromUtf8(json.mid(start, *pos - start)));
            }
        }

        if (error) *error = "Unterminated string";
        return false;
    }

    static bool parseNumber(const QByteArray &json, int *pos, QVariant *out, QString *error)
    {
        int start = *pos;
        bool isFloating = false;

        if (json.at(*pos) == '-') {
            (*pos)++;
        }

        while (*pos < json.size()) {
            char c = json.at(*pos);
            if (c >= '0' && c <= '9') {
                (*pos)++;
            } else if (c == '.' || c == 'e' || c == 'E' || c == '+') {
                isFloating = true;
                (*pos)++;
            } else {
                break;
            }
        }

        QByteArray numStr = json.mid(start, *pos - start);
        if (isFloating) {
            bool ok = false;
            double d = numStr.toDouble(&ok);
            if (!ok) {
                if (error) *error = "Malformed floating point number";
                return false;
            }
            *out = d;
            return true;
        } else {
            bool ok = false;
            qint64 val = numStr.toLongLong(&ok);
            if (!ok) {
                if (error) *error = "Malformed integer number";
                return false;
            }
            *out = val;
            return true;
        }
    }

    static bool parseBool(const QByteArray &json, int *pos, QVariant *out, QString *error)
    {
        if (json.mid(*pos, 4) == "true") {
            *pos += 4;
            *out = true;
            return true;
        } else if (json.mid(*pos, 5) == "false") {
            *pos += 5;
            *out = false;
            return true;
        }
        if (error) *error = "Invalid boolean literal";
        return false;
    }

    static bool parseNull(const QByteArray &json, int *pos, QVariant *out, QString *error)
    {
        if (json.mid(*pos, 4) == "null") {
            *pos += 4;
            *out = QVariant(); // Invalid variant represents null
            return true;
        }
        if (error) *error = "Invalid null literal";
        return false;
    }

    static void serializeValue(const QVariant &val, QByteArray *out)
    {
        if (!val.isValid()) {
            out->append("null");
        } else if (val.type() == QVariant::Map) {
            out->append("{");
            QVariantMap map = val.toMap();
            bool first = true;
            for (QVariantMap::const_iterator it = map.begin(); it != map.end(); ++it) {
                if (!first) out->append(",");
                first = false;
                serializeString(it.key(), out);
                out->append(":");
                serializeValue(it.value(), out);
            }
            out->append("}");
        } else if (val.type() == QVariant::List) {
            out->append("[");
            QVariantList list = val.toList();
            for (int i = 0; i < list.size(); ++i) {
                if (i > 0) out->append(",");
                serializeValue(list.at(i), out);
            }
            out->append("]");
        } else if (val.type() == QVariant::String) {
            serializeString(val.toString(), out);
        } else if (val.type() == QVariant::Bool) {
            out->append(val.toBool() ? "true" : "false");
        } else if (val.type() == QVariant::LongLong || val.type() == QVariant::Int || val.type() == QVariant::UInt || val.type() == QVariant::ULongLong) {
            out->append(QByteArray::number(val.toLongLong()));
        } else if (val.type() == QVariant::Double) {
            out->append(QByteArray::number(val.toDouble()));
        } else {
            serializeString(val.toString(), out);
        }
    }

    static void serializeString(const QString &str, QByteArray *out)
    {
        out->append("\"");
        for (int i = 0; i < str.length(); ++i) {
            QChar c = str.at(i);
            if (c == '"') {
                out->append("\\\"");
            } else if (c == '\\') {
                out->append("\\\\");
            } else if (c == '\b') {
                out->append("\\b");
            } else if (c == '\f') {
                out->append("\\f");
            } else if (c == '\n') {
                out->append("\\n");
            } else if (c == '\r') {
                out->append("\\r");
            } else if (c == '\t') {
                out->append("\\t");
            } else {
                // Direct UTF-8 byte serialization
                out->append(QString(c).toUtf8());
            }
        }
        out->append("\"");
    }
};

// Generic JSON loader dispatch
bool loadJsonBuffer(const QByteArray &data, QVariant *out, QString *error)
{
    if (data.size() > ProtocolJson::MAX_JSON_SIZE) {
        if (error) *error = "JSON payload exceeds maximum size limit (64 KiB)";
        return false;
    }
#if defined(Q_OS_BLACKBERRY)
    bb::data::JsonDataAccess jda;
    *out = jda.loadFromBuffer(data);
    if (jda.hasError()) {
        if (error) *error = jda.error().errorMessage();
        return false;
    }
    return true;
#else
    return JsonHelper::parse(data, out, error);
#endif
}

// Generic JSON saver dispatch
QByteArray saveJsonBuffer(const QVariant &val, QString *error)
{
#if defined(Q_OS_BLACKBERRY)
    bb::data::JsonDataAccess jda;
    QByteArray out;
    jda.saveToBuffer(val, &out);
    if (jda.hasError()) {
        if (error) *error = jda.error().errorMessage();
        return QByteArray();
    }
    return out;
#else
    Q_UNUSED(error);
    return JsonHelper::stringify(val);
#endif
}

// Helper to decode and validate common DeviceInfo fields
bool decodeDeviceInfoMap(const QVariantMap &map, DeviceInfo *info, QString *error, bool requirePortAndProtocol)
{
    if (!map.contains("alias") || map.value("alias").type() != QVariant::String || map.value("alias").toString().trimmed().isEmpty()) {
        if (error) *error = "Missing or invalid 'alias'";
        return false;
    }
    info->alias = map.value("alias").toString();

    if (!map.contains("version") || map.value("version").type() != QVariant::String) {
        if (error) *error = "Missing or invalid 'version'";
        return false;
    }
    info->version = map.value("version").toString();
    if (!info->version.startsWith("2.")) {
        if (error) *error = QString("Incompatible protocol version '%1'; requires major version 2").arg(info->version);
        return false;
    }

    if (!map.contains("fingerprint") || map.value("fingerprint").type() != QVariant::String || map.value("fingerprint").toString().trimmed().isEmpty()) {
        if (error) *error = "Missing or invalid 'fingerprint'";
        return false;
    }
    info->fingerprint = map.value("fingerprint").toString();

    // deviceModel (optional/nullable)
    if (map.contains("deviceModel") && map.value("deviceModel").isValid()) {
        info->deviceModel = map.value("deviceModel").toString();
    } else {
        info->deviceModel = "BlackBerry";
    }

    // deviceType (optional; unknown falls back to desktop)
    if (map.contains("deviceType") && map.value("deviceType").isValid()) {
        QString dt = map.value("deviceType").toString().toLower();
        if (dt == "mobile" || dt == "desktop" || dt == "web" || dt == "headless" || dt == "server") {
            info->deviceType = dt;
        } else {
            info->deviceType = "desktop";
        }
    } else {
        info->deviceType = "desktop";
    }

    // download (optional, default false)
    if (map.contains("download") && map.value("download").type() == QVariant::Bool) {
        info->download = map.value("download").toBool();
    } else {
        info->download = false;
    }

    if (requirePortAndProtocol) {
        if (!map.contains("port")) {
            if (error) *error = "Missing 'port'";
            return false;
        }
        QVariant portVar = map.value("port");
        if (portVar.type() == QVariant::String) {
            if (error) *error = "Invalid 'port' type (must be integer, not string)";
            return false;
        }
        if (portVar.type() == QVariant::Double) {
            double d = portVar.toDouble();
            if (d != static_cast<double>(static_cast<qint64>(d))) {
                if (error) *error = "Invalid 'port' type (fractional number not allowed)";
                return false;
            }
        }
        bool ok = false;
        qint64 p = portVar.toLongLong(&ok);
        if (!ok || p < 1 || p > 65535) {
            if (error) *error = "Invalid 'port' value (must be integer 1-65535)";
            return false;
        }
        info->port = static_cast<quint16>(p);

        if (!map.contains("protocol") || map.value("protocol").type() != QVariant::String) {
            if (error) *error = "Missing or invalid 'protocol'";
            return false;
        }
        QString proto = map.value("protocol").toString().toLower();
        if (proto != "http" && proto != "https") {
            if (error) *error = QString("Invalid protocol '%1'; must be 'http' or 'https'").arg(proto);
            return false;
        }
        info->protocol = proto;
    } else {
        // Optional port/protocol
        if (map.contains("port")) {
            QVariant portVar = map.value("port");
            if (portVar.type() != QVariant::String && portVar.type() != QVariant::Double) {
                bool ok = false;
                qint64 p = portVar.toLongLong(&ok);
                if (ok && p >= 1 && p <= 65535) {
                    info->port = static_cast<quint16>(p);
                }
            }
        }
        if (map.contains("protocol") && map.value("protocol").type() == QVariant::String) {
            QString proto = map.value("protocol").toString().toLower();
            if (proto == "http" || proto == "https") {
                info->protocol = proto;
            }
        }
    }

    return true;
}

} // namespace

namespace ProtocolJson {

bool decodeAnnouncement(const QByteArray &data, DeviceInfo *info, bool *announce, QString *error)
{
    if (!info) {
        if (error) *error = "Null info pointer";
        return false;
    }

    QVariant root;
    if (!loadJsonBuffer(data, &root, error)) {
        return false;
    }

    if (root.type() != QVariant::Map) {
        if (error) *error = "Announcement JSON root must be an object";
        return false;
    }

    QVariantMap map = root.toMap();
    if (!decodeDeviceInfoMap(map, info, error, true)) {
        return false;
    }

    // announce flag is optional, default to true
    bool ann = true;
    if (map.contains("announce") && map.value("announce").type() == QVariant::Bool) {
        ann = map.value("announce").toBool();
    }
    if (announce) {
        *announce = ann;
    }

    return true;
}

bool decodeRegisterRequest(const QByteArray &data, DeviceInfo *info, QString *error)
{
    if (!info) {
        if (error) *error = "Null info pointer";
        return false;
    }

    QVariant root;
    if (!loadJsonBuffer(data, &root, error)) {
        return false;
    }

    if (root.type() != QVariant::Map) {
        if (error) *error = "Register request JSON root must be an object";
        return false;
    }

    return decodeDeviceInfoMap(root.toMap(), info, error, true);
}

bool decodeRegisterResponse(const QByteArray &data, const PeerDevice &target,
                            PeerDevice *peer, QString *error)
{
    if (!peer) {
        if (error) *error = "Null peer pointer";
        return false;
    }

    QVariant root;
    if (!loadJsonBuffer(data, &root, error)) {
        return false;
    }

    if (root.type() != QVariant::Map) {
        if (error) *error = "Register response JSON root must be an object";
        return false;
    }

    DeviceInfo info;
    info.port = target.info.port;
    info.protocol = target.info.protocol;

    if (!decodeDeviceInfoMap(root.toMap(), &info, error, false)) {
        return false;
    }

    peer->info = info;
    peer->info.port = target.info.port;
    peer->info.protocol = target.info.protocol;
    peer->address = target.address;
    peer->lastSeen = QDateTime::currentDateTime();

    return true;
}

bool decodePrepareRequest(const QByteArray &data, PrepareUploadRequest *req, QString *error)
{
    if (!req) {
        if (error) *error = "Null request pointer";
        return false;
    }

    QVariant root;
    if (!loadJsonBuffer(data, &root, error)) {
        return false;
    }

    if (root.type() != QVariant::Map) {
        if (error) *error = "Prepare upload request root must be an object";
        return false;
    }

    QVariantMap map = root.toMap();

    // info object
    if (!map.contains("info") || map.value("info").type() != QVariant::Map) {
        if (error) *error = "Missing or invalid 'info' object in prepare request";
        return false;
    }
    if (!decodeDeviceInfoMap(map.value("info").toMap(), &req->info, error, true)) {
        return false;
    }

    // files object
    if (!map.contains("files")) {
        if (error) *error = "Missing 'files' in prepare request";
        return false;
    }

    QVariant filesVar = map.value("files");
    if (filesVar.type() == QVariant::List) {
        if (error) *error = "'files' must be an object keyed by file id, not an array";
        return false;
    }
    if (filesVar.type() != QVariant::Map) {
        if (error) *error = "'files' must be an object";
        return false;
    }

    QVariantMap filesMap = filesVar.toMap();
    if (filesMap.isEmpty()) {
        if (error) *error = "'files' map cannot be empty";
        return false;
    }
    if (filesMap.size() > MAX_OFFERED_FILES) {
        if (error) *error = QString("Offered file count (%1) exceeds maximum limit of %2")
                                .arg(filesMap.size()).arg(MAX_OFFERED_FILES);
        return false;
    }

    req->files.clear();
    for (QVariantMap::const_iterator it = filesMap.begin(); it != filesMap.end(); ++it) {
        QString keyId = it.key();
        if (it.value().type() != QVariant::Map) {
            if (error) *error = QString("File item '%1' must be an object").arg(keyId);
            return false;
        }

        QVariantMap fmap = it.value().toMap();
        if (!fmap.contains("id") || fmap.value("id").type() != QVariant::String) {
            if (error) *error = "Missing or invalid file 'id'";
            return false;
        }

        QString innerId = fmap.value("id").toString();
        if (innerId != keyId) {
            if (error) *error = QString("File id mismatch between map key '%1' and object id '%2'")
                                    .arg(keyId).arg(innerId);
            return false;
        }

        if (!fmap.contains("fileName") || fmap.value("fileName").type() != QVariant::String) {
            if (error) *error = "Missing or invalid 'fileName'";
            return false;
        }
        QString rawFileName = fmap.value("fileName").toString();
        if (rawFileName.trimmed().isEmpty()) {
            if (error) *error = "Empty 'fileName'";
            return false;
        }
        if (rawFileName.contains("..") || rawFileName.contains('/') || rawFileName.contains('\\') || rawFileName.contains(QChar('\0'))) {
            if (error) *error = QString("Illegal directory traversal or separators in fileName: '%1'").arg(rawFileName);
            return false;
        }

        if (!fmap.contains("size")) {
            if (error) *error = "Missing file 'size'";
            return false;
        }
        QVariant sizeVar = fmap.value("size");
        if (sizeVar.type() == QVariant::String) {
            if (error) *error = "File 'size' must not be a string";
            return false;
        }
        if (sizeVar.type() == QVariant::Double) {
            double d = sizeVar.toDouble();
            if (d != static_cast<double>(static_cast<qint64>(d))) {
                if (error) *error = "File 'size' must not be fractional";
                return false;
            }
        }
        bool sizeOk = false;
        qint64 sizeVal = sizeVar.toLongLong(&sizeOk);
        if (!sizeOk || sizeVal < 0) {
            if (error) *error = "File 'size' must be a nonnegative integer";
            return false;
        }

        FileMeta meta;
        meta.id = innerId;
        meta.fileName = rawFileName;
        meta.size = sizeVal;

        if (fmap.contains("fileType") && fmap.value("fileType").isValid()) {
            meta.fileType = fmap.value("fileType").toString();
        } else {
            meta.fileType = "application/octet-stream";
        }

        if (fmap.contains("sha256") && fmap.value("sha256").isValid() && !fmap.value("sha256").isNull()) {
            QString hash = fmap.value("sha256").toString().trimmed();
            if (!hash.isEmpty()) {
                QRegExp hexRegex("^[0-9a-fA-F]{64}$");
                if (!hexRegex.exactMatch(hash)) {
                    if (error) *error = QString("Invalid sha256 checksum format for file '%1'").arg(innerId);
                    return false;
                }
                meta.sha256 = hash.toLower();
            }
        }

        req->files.insert(innerId, meta);
    }

    return true;
}

bool decodePrepareResponse(const QByteArray &data, PrepareUploadResponse *res, QString *error)
{
    if (!res) {
        if (error) *error = "Null response pointer";
        return false;
    }

    QVariant root;
    if (!loadJsonBuffer(data, &root, error)) {
        return false;
    }

    if (root.type() != QVariant::Map) {
        if (error) *error = "Prepare upload response root must be an object";
        return false;
    }

    QVariantMap map = root.toMap();
    if (!map.contains("sessionId") || map.value("sessionId").type() != QVariant::String || map.value("sessionId").toString().isEmpty()) {
        if (error) *error = "Missing or invalid 'sessionId' in prepare response";
        return false;
    }
    res->sessionId = map.value("sessionId").toString();

    if (!map.contains("files") || map.value("files").type() != QVariant::Map) {
        if (error) *error = "Missing or invalid 'files' map in prepare response";
        return false;
    }

    QVariantMap filesMap = map.value("files").toMap();
    res->fileTokens.clear();
    for (QVariantMap::const_iterator it = filesMap.begin(); it != filesMap.end(); ++it) {
        if (it.value().type() != QVariant::String || it.value().toString().isEmpty()) {
            if (error) *error = QString("Missing or invalid token for file '%1'").arg(it.key());
            return false;
        }
        res->fileTokens.insert(it.key(), it.value().toString());
    }

    return true;
}

QByteArray encodeAnnouncement(const DeviceInfo &info, bool announce, QString *error)
{
    QVariantMap map;
    map.insert("alias", info.alias);
    map.insert("version", info.version.isEmpty() ? "2.2" : info.version);
    map.insert("deviceModel", info.deviceModel.isEmpty() ? "BlackBerry" : info.deviceModel);
    map.insert("deviceType", info.deviceType.isEmpty() ? "mobile" : info.deviceType);
    map.insert("fingerprint", info.fingerprint);
    map.insert("port", info.port);
    map.insert("protocol", info.protocol.isEmpty() ? "http" : info.protocol);
    map.insert("download", info.download);
    map.insert("announce", announce);

    return saveJsonBuffer(map, error);
}

QByteArray encodeRegisterRequest(const DeviceInfo &info, QString *error)
{
    QVariantMap map;
    map.insert("alias", info.alias);
    map.insert("version", info.version.isEmpty() ? "2.2" : info.version);
    map.insert("deviceModel", info.deviceModel.isEmpty() ? "BlackBerry" : info.deviceModel);
    map.insert("deviceType", info.deviceType.isEmpty() ? "mobile" : info.deviceType);
    map.insert("fingerprint", info.fingerprint);
    map.insert("port", info.port);
    map.insert("protocol", info.protocol.isEmpty() ? "http" : info.protocol);
    map.insert("download", info.download);

    return saveJsonBuffer(map, error);
}

QByteArray encodeDeviceResponse(const DeviceInfo &info, QString *error)
{
    return encodeRegisterRequest(info, error);
}

QByteArray encodePrepareRequest(const DeviceInfo &info, const FileMeta &file, QString *error)
{
    QVariantMap root;

    // Info object
    QVariantMap infoMap;
    infoMap.insert("alias", info.alias);
    infoMap.insert("version", info.version.isEmpty() ? "2.2" : info.version);
    infoMap.insert("deviceModel", info.deviceModel.isEmpty() ? "BlackBerry" : info.deviceModel);
    infoMap.insert("deviceType", info.deviceType.isEmpty() ? "mobile" : info.deviceType);
    infoMap.insert("fingerprint", info.fingerprint);
    infoMap.insert("port", info.port);
    infoMap.insert("protocol", info.protocol.isEmpty() ? "http" : info.protocol);
    infoMap.insert("download", info.download);
    root.insert("info", infoMap);

    // Files object (keyed by file.id)
    QVariantMap filesMap;
    QVariantMap fileItem;
    fileItem.insert("id", file.id);
    fileItem.insert("fileName", file.fileName);
    fileItem.insert("size", file.size);
    fileItem.insert("fileType", file.fileType.isEmpty() ? "application/octet-stream" : file.fileType);
    fileItem.insert("sha256", file.sha256.isEmpty() ? QVariant() : file.sha256);
    fileItem.insert("preview", QVariant());
    fileItem.insert("metadata", QVariant());

    filesMap.insert(file.id, fileItem);
    root.insert("files", filesMap);

    return saveJsonBuffer(root, error);
}

QByteArray encodePrepareResponse(const PrepareUploadResponse &res, QString *error)
{
    QVariantMap root;
    root.insert("sessionId", res.sessionId);

    QVariantMap filesMap;
    for (QMap<QString, QString>::const_iterator it = res.fileTokens.begin(); it != res.fileTokens.end(); ++it) {
        filesMap.insert(it.key(), it.value());
    }
    root.insert("files", filesMap);

    return saveJsonBuffer(root, error);
}

} // namespace ProtocolJson
