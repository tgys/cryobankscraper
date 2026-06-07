#include "ssl_utils.h"

#include <QFile>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QStringList>

#include <QDebug>

namespace scrapeff {

namespace {

bool appendCertsFromFile(const QString &path, QList<QSslCertificate> *aggregate)
{
    if (path.isEmpty() || aggregate == nullptr || !QFile::exists(path))
        return false;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning() << "applySslFromEnv: failed to open CA bundle:" << path << f.errorString();
        return false;
    }

    const QByteArray data = f.readAll();
    if (data.isEmpty()) {
        qWarning() << "applySslFromEnv: empty CA bundle:" << path;
        return false;
    }

    QList<QSslCertificate> chunk = QSslCertificate::fromData(data, QSsl::Pem);
    if (chunk.isEmpty())
        chunk = QSslCertificate::fromData(data, QSsl::Der);
    if (chunk.isEmpty()) {
        qWarning() << "applySslFromEnv: no certificates parsed from:" << path;
        return false;
    }

    aggregate->append(chunk);
    return true;
}

void applyAggregatedCa(const QList<QSslCertificate> &extra)
{
    if (extra.isEmpty())
        return;
    QSslConfiguration conf = QSslConfiguration::defaultConfiguration();
    conf.setCaCertificates(extra + conf.caCertificates());
    QSslConfiguration::setDefaultConfiguration(conf);
}

static QStringList systemCaBundlePaths()
{
    return {
        QStringLiteral("/etc/ssl/certs/ca-certificates.crt"),
        QStringLiteral("/etc/pki/tls/certs/ca-bundle.crt"),
        QStringLiteral("/etc/ssl/cert.pem"),
        QStringLiteral("/etc/ssl/certs/ca-bundle.crt"),
    };
}

} // namespace

void applySslFromEnv()
{
    if (!QSslSocket::supportsSsl())
        return;

    QList<QSslCertificate> extra;
    QStringList tried;

    const QString envPaths[] = {
        QString(qEnvironmentVariable("SSL_CERT_FILE")),
        QString(qEnvironmentVariable("NIX_SSL_CERT_FILE")),
        QString(qEnvironmentVariable("REQUESTS_CA_BUNDLE")), // python-requests convention
        QString(qEnvironmentVariable("CURL_CA_BUNDLE")),
    };
    for (const QString &p : envPaths) {
        if (p.isEmpty() || tried.contains(p))
            continue;
        tried.append(p);
        if (appendCertsFromFile(p, &extra))
            break;
    }

    // Outside nix-shell / GUI launches: still load distro CA store (parity with urllib/cPython on same machine).
    if (extra.isEmpty()) {
        for (const QString &p : systemCaBundlePaths()) {
            if (appendCertsFromFile(p, &extra))
                break;
        }
    }

    applyAggregatedCa(extra);
}

} // namespace scrapeff
