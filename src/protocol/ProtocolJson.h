#ifndef PROTOCOL_JSON_H
#define PROTOCOL_JSON_H

#include "protocol/ProtocolTypes.h"

namespace ProtocolJson {

static const int MAX_JSON_SIZE = 65536; // 64 KiB project budget
static const int MAX_OFFERED_FILES = 64; // At most 64 offered files per prepare request

bool decodeAnnouncement(const QByteArray &data, DeviceInfo *info, bool *announce, QString *error);
bool decodeRegisterRequest(const QByteArray &data, DeviceInfo *info, QString *error);
bool decodeRegisterResponse(const QByteArray &data, const PeerDevice &target,
                            PeerDevice *peer, QString *error);
bool decodePrepareRequest(const QByteArray &data, PrepareUploadRequest *req, QString *error);
bool decodePrepareResponse(const QByteArray &data, PrepareUploadResponse *res, QString *error);

QByteArray encodeAnnouncement(const DeviceInfo &info, bool announce, QString *error);
QByteArray encodeRegisterRequest(const DeviceInfo &info, QString *error);
QByteArray encodeDeviceResponse(const DeviceInfo &info, QString *error);
QByteArray encodePrepareRequest(const DeviceInfo &info, const FileMeta &file, QString *error);
QByteArray encodePrepareResponse(const PrepareUploadResponse &res, QString *error);

} // namespace ProtocolJson

#endif // PROTOCOL_JSON_H
