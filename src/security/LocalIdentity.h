#ifndef LOCAL_IDENTITY_H
#define LOCAL_IDENTITY_H

#include <QString>
#include <QByteArray>

namespace localsend {

class LocalIdentity {
public:
    LocalIdentity() {}

    const QString &certPath() const { return m_certPath; }
    const QString &keyPath() const { return m_keyPath; }
    const QByteArray &certPem() const { return m_certPem; }
    const QByteArray &keyPem() const { return m_keyPem; }
    const QString &fingerprint() const { return m_fingerprint; }
    bool isValid() const { return !m_fingerprint.isEmpty() && !m_certPem.isEmpty() && !m_keyPem.isEmpty(); }

    // Load an existing certificate/key pair from storageDir, or generate and persist
    // a new EC P-256 self-signed pair if neither file exists.
    // Partial/corrupt state (e.g. only one file exists, unreadable, invalid PEM,
    // mismatched key, or write failure) fails visibly and never silently replaces
    // an existing identity file.
    static bool loadOrCreate(const QString &storageDir, LocalIdentity *out, QString *error = 0);

    // Calculate SHA-256 of leaf certificate DER (64 lowercase hex characters)
    static QString calculateFingerprint(const QByteArray &certPem, QString *error = 0);

    // Normalize a valid 64-hex fingerprint string (allowing optional ':', '-', ' ' separators).
    // Returns empty QString if invalid.
    static QString normalizeFingerprint(const QString &rawPin);

private:
    QString m_certPath;
    QString m_keyPath;
    QByteArray m_certPem;
    QByteArray m_keyPem;
    QString m_fingerprint;
};

} // namespace localsend

#endif // LOCAL_IDENTITY_H
