#include "http_request.h"

#include <QNetworkRequest>
#include <QUrl>

namespace scrapeff {

namespace {

// Typical browser-ish UA — keep in sync everywhere we fetch remotely.
static const char kChromeLikeUserAgent[] =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/122.0.0.0 Safari/537.36";

static const char kSecChUa[] =
    "\"Chromium\";v=\"122\", \"Not(A:Brand\";v=\"24\", \"Google Chrome\";v=\"122\"";

static QString canonicalHost(const QString &host)
{
    QString h = host.toLower();
    if (h.startsWith(QLatin1String("www.")))
        return h.mid(4);
    return h;
}

static QByteArray secFetchSiteFor(const QUrl &url, const QString &referrerOpt, bool forHtmlRequest)
{
    if (referrerOpt.trimmed().isEmpty())
        return forHtmlRequest ? QByteArrayLiteral("none") : QByteArrayLiteral("cross-site");
    const QUrl ref(referrerOpt);
    if (!ref.isValid())
        return QByteArrayLiteral("cross-site");
    const QString a = canonicalHost(url.host());
    const QString b = canonicalHost(ref.host());
    if (a.isEmpty() || b.isEmpty())
        return QByteArrayLiteral("cross-site");
    if (a == b)
        return QByteArrayLiteral("same-origin");
    return QByteArrayLiteral("cross-site");
}

} // namespace

QNetworkRequest makeScraperNetworkRequest(const QUrl &url, int transferTimeoutMs, bool forHtml,
                                          const QString &referrerOpt)
{
    QNetworkRequest r(url);
    r.setTransferTimeout(transferTimeoutMs);
    // Many CDNs inspect these lists; naive scripts often omit them entirely.
    r.setRawHeader("User-Agent", QByteArray(kChromeLikeUserAgent));
    r.setRawHeader("sec-ch-ua", QByteArray(kSecChUa));
    r.setRawHeader("sec-ch-ua-mobile", QByteArrayLiteral("?0"));
    r.setRawHeader("sec-ch-ua-platform", QByteArrayLiteral("\"Windows\""));

    const QByteArray site = secFetchSiteFor(url, referrerOpt, forHtml);
    const QString refTrim = referrerOpt.trimmed();
    const QByteArray refUtf = refTrim.isEmpty() ? QByteArray() : refTrim.toUtf8();

    if (forHtml) {
        r.setRawHeader(
            "Accept",
            QByteArrayLiteral("text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,image/"
                              "apng,*/*;q=0.8"));
        r.setRawHeader("Sec-Fetch-Dest", QByteArrayLiteral("document"));
        r.setRawHeader("Sec-Fetch-Mode", QByteArrayLiteral("navigate"));
        r.setRawHeader("Sec-Fetch-Site", site);
        // Direct URL entry behaves like typed navigation — most browsers omit this; keep Site accurate.
        r.setRawHeader("Sec-Fetch-User", QByteArrayLiteral("?1"));
        r.setRawHeader("Upgrade-Insecure-Requests", QByteArrayLiteral("1"));
        if (!refUtf.isEmpty())
            r.setRawHeader("Referer", refUtf);
    } else {
        r.setRawHeader(
            "Accept",
            QByteArrayLiteral("image/avif,image/webp,image/apng,image/*,*/*;q=0.8"));
        r.setRawHeader("Sec-Fetch-Dest", QByteArrayLiteral("image"));
        r.setRawHeader("Sec-Fetch-Mode", QByteArrayLiteral("no-cors"));
        r.setRawHeader("Sec-Fetch-Site", site);
        if (!refUtf.isEmpty())
            r.setRawHeader("Referer", refUtf);
    }

    r.setRawHeader("Accept-Language", QByteArrayLiteral("en-US,en;q=0.9"));
    r.setRawHeader("Cache-Control", QByteArrayLiteral("max-age=0"));

    // HTTP/2 can change TLS fingerprints vs older stacks; disabling matches urllib3 defaults on many setups.
    r.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    return r;
}

QNetworkRequest makeGalleryImageNetworkRequest(const QUrl &url, int transferTimeoutMs,
                                               const QString &referrerOpt)
{
    // Match the default scrapeff Chromium-style headers — the scraper uses ChromiumAutomation by default,
    // so gallery image fetches must use the same profile or CDNs may reject due to fingerprint mismatch.
    return makeScraperNetworkRequest(url, transferTimeoutMs, false, referrerOpt);
}

QNetworkRequest makePythonRequestsCompatibleRequest(const QUrl &url, int transferTimeoutMs, bool forHtml,
                                                    const QString &referrerOpt)
{
    QNetworkRequest r(url);
    r.setTransferTimeout(transferTimeoutMs);
    // Defaults seen with python-requests / urllib3 (no Sec-Fetch-*, no sec-ch-ua).
    r.setRawHeader("User-Agent", QByteArrayLiteral("python-requests/2.32.3"));
    if (forHtml)
        r.setRawHeader(
            "Accept",
            QByteArrayLiteral("text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/"
                              "webp,image/apng,*/*;q=0.8"));
    else
        r.setRawHeader("Accept", QByteArrayLiteral("*/*"));
    r.setRawHeader("Accept-Encoding", QByteArrayLiteral("gzip, deflate"));
    r.setRawHeader("Connection", QByteArrayLiteral("keep-alive"));

    const QString refTrim = referrerOpt.trimmed();
    if (!refTrim.isEmpty())
        r.setRawHeader("Referer", refTrim.toUtf8());

    return r;
}

} // namespace scrapeff
