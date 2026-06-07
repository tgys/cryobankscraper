#include "scrape_runner.h"

#include "html_parse.h"
#include "http_request.h"
#include "scrape_runner.h"
#include "ssl_utils.h"
#include "url_parse.h"

#include <QtConcurrent>

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkCookieJar>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSet>
#include <QTextStream>
#include <QThread>
#include <QThreadPool>
#include <QUrl>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <tuple>
#include <vector>

namespace {

// Named constants for better maintainability
constexpr int kImageBytesMax = 15 * 1024 * 1024;
constexpr int kHtmlDocMaxBytes = 8 * 1024 * 1024;
constexpr int kRetryBaseDelayMs = 400;
constexpr int kRetryBaseDelayNetworkMs = 250;
constexpr int kMaxRetryAttempts = 3;
constexpr int kHtmlTransferTimeoutMs = 20000;
constexpr int kImageTransferTimeoutMs = 25000;

/** SCRAPEFF_HTTP_SPACING_MS: explicit ms; unset → Chromium default ~150 ms, urllib-compatible default off (Python loop). */
static int pacingIntervalMs(scrapeff::HttpClientProfile profile)
{
    const QByteArray v = qgetenv("SCRAPEFF_HTTP_SPACING_MS");
    if (v.compare("0", Qt::CaseInsensitive) == 0 || v.compare("off", Qt::CaseInsensitive) == 0)
        return 0;
    bool ok = false;
    const int n = QString::fromLocal8Bit(v).trimmed().toInt(&ok);
    if (ok && n >= 0)
        return n;
    return (profile == scrapeff::HttpClientProfile::ChromiumAutomation) ? 150 : 0;
}

/** Serializes HTTP starts globally when pacing ms > 0. */
static void httpPacingDelay(scrapeff::HttpClientProfile profile)
{
    const int spacing = pacingIntervalMs(profile);
    if (spacing <= 0)
        return;

    using clock = std::chrono::steady_clock;
    using Ms = std::chrono::milliseconds;

    static std::mutex mutex;
    static auto nextAvail = clock::now();

    std::lock_guard<std::mutex> guard(mutex);
    auto now = clock::now();
    if (now < nextAvail) {
        const auto waitMs = std::chrono::duration_cast<Ms>(nextAvail - now).count();
        if (waitMs > 0)
            QThread::msleep(int(waitMs));
        now = clock::now();
    }
    nextAvail = now + Ms(spacing);
}

/** Serialize stdout URL progress — QtConcurrent workers must not QTextStream concurrently. */
static std::mutex gScrapeStdoutLock;

struct Shared;

static void logScrapeUrlProgress(const Shared *sh, const QString &line);

/** In-site referrer for donor profile GETs — some Fairfax/WAF stacks treat bare document requests differently. */
static QString htmlNavigationReferrerForUrl(const QUrl &url)
{
    const QString h = url.host().toLower();
    if (!h.endsWith(QLatin1String("fairfaxcryobank.com")))
        return {};
    const QString path = url.path().toLower();
    if (!path.contains(QLatin1String("/search/")))
        return {};
    return QStringLiteral("https://fairfaxcryobank.com/search/");
}

inline QNetworkRequest makeHtmlReq(scrapeff::HttpClientProfile p, const QUrl &url, int transferTimeoutMs,
                                   const QString &htmlReferrer = {})
{
    if (p == scrapeff::HttpClientProfile::PythonUrllibCompatible)
        return scrapeff::makePythonRequestsCompatibleRequest(url, transferTimeoutMs, true, htmlReferrer);
    return scrapeff::makeScraperNetworkRequest(url, transferTimeoutMs, true, htmlReferrer);
}

inline QNetworkRequest makeBinaryReq(scrapeff::HttpClientProfile p, const QUrl &url, int transferTimeoutMs,
                                     const QString &referrerOpt = {})
{
    if (p == scrapeff::HttpClientProfile::PythonUrllibCompatible)
        return scrapeff::makePythonRequestsCompatibleRequest(url, transferTimeoutMs, false, referrerOpt);
    return scrapeff::makeScraperNetworkRequest(url, transferTimeoutMs, false, referrerOpt);
}


int blockingGet(QNetworkAccessManager &nam, const QNetworkRequest &req, QByteArray *body, QUrl *effectiveUrl,
                scrapeff::HttpClientProfile profile)
{
    httpPacingDelay(profile);
    QNetworkReply *r = nam.get(req);
    QEventLoop loop;
    QObject::connect(r, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    const QVariant code = r->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    const int st = code.isValid() ? code.toInt() : 0;
    if (effectiveUrl)
        *effectiveUrl = r->url();
    *body = r->readAll();
    r->deleteLater();
    return st;
}

int getHtmlPage(QNetworkAccessManager &nam, const QUrl &url, QString *htmlOut, QUrl *effectiveOut,
                  scrapeff::HttpClientProfile profile)
{
    for (int attempt = 0; attempt < kMaxRetryAttempts; ++attempt) {
        const QString navRef = htmlNavigationReferrerForUrl(url);
        QNetworkRequest req = makeHtmlReq(profile, url, kHtmlTransferTimeoutMs, navRef);
        QByteArray body;
        const int st = blockingGet(nam, req, &body, effectiveOut, profile);
        if (st == 200) {
            if (body.size() > kHtmlDocMaxBytes) {
                qWarning() << "getHtmlPage: response too large:" << body.size() << "bytes for" << url.toString();
                return 413;
            }
            *htmlOut = QString::fromUtf8(body);
            return 200;
        }
        if (st == 429 || st == 403 || st == 500 || st == 502 || st == 503 || st == 504) {
            QThread::msleep(kRetryBaseDelayMs * (1 << attempt));
            continue;
        }
        if (st == 0 && attempt < kMaxRetryAttempts - 1) {
            QThread::msleep(kRetryBaseDelayNetworkMs * (1 << attempt));
            continue;
        }
        if (st != 200) {
            // Fairfax donor lookup often returns genuine 404 for unused IDs — expected when brute-forcing,
            // so avoid spamming the console.
            const bool quietDonorMiss = st == 404
                && url.path().contains(QLatin1String("donorprofile.aspx"), Qt::CaseInsensitive);
            if (!quietDonorMiss)
                qWarning() << "getHtmlPage: request failed with status" << st << "for" << url.toString();
        }
        return st;
    }
    qWarning() << "getHtmlPage: all retry attempts exhausted for" << url.toString();
    return 0;
}

int getBytesLimited(QNetworkAccessManager &nam, const QUrl &url, QByteArray *out, int maxBytes, int timeoutMs,
                    scrapeff::HttpClientProfile profile, const QString &referrerOpt)
{
    for (int attempt = 0; attempt < kMaxRetryAttempts; ++attempt) {
        QNetworkRequest req = makeBinaryReq(profile, url, timeoutMs, referrerOpt);
        QByteArray body;
        QUrl eff;
        const int st = blockingGet(nam, req, &body, &eff, profile);
        (void) eff;
        if (st == 200) {
            if (body.size() > maxBytes) {
                qWarning() << "getBytesLimited: response too large:" << body.size() << "bytes for" << url.toString();
                return 413;
            }
            *out = body;
            return 200;
        }
        if (st == 429 || st == 403 || st == 500 || st == 502 || st == 503 || st == 504) {
            QThread::msleep(kRetryBaseDelayMs * (1 << attempt));
            continue;
        }
        if (st == 0 && attempt < kMaxRetryAttempts - 1) {
            QThread::msleep(kRetryBaseDelayNetworkMs * (1 << attempt));
            continue;
        }
        return st;
    }
    return 0;
}

struct Shared {
    QMutex mutex;
    QHash<QString, int> childhoodDidCounts;
    /** One gallery tile per donor profile id (avoids duplicate tiles from number=428 vs number=0428). */
    QSet<int> acceptedProfileNums;
    QSet<QString> acceptedGalleryKeys;
    QVector<std::tuple<QString, QString, std::optional<int>, std::optional<int>>> galleryEntries;
    QVector<QString> imageUrlFileLines;
    QVector<QString> fetchFailedLines;
    /** Profile page URLs fetched successfully — recorded before parsing #foto (see output file same name). */
    QVector<QString> profileUrlsQueuedBeforeImages;
    QHash<QString, QStringList> results;
    std::function<void(const QString &line)> logLine;
    QString galleryJsonPath;
};

static void logScrapeUrlProgress(const Shared *sh, const QString &line)
{
    std::lock_guard<std::mutex> guard(gScrapeStdoutLock);
    if (sh && sh->logLine) {
        sh->logLine(line);
        return;
    }
    QTextStream ts(stdout);
    ts << line << QLatin1Char('\n');
    ts.flush();
}

bool writeGalleryJson(const QString &path, const Shared &sh);

bool tryAddGalleryImage(Shared &sh, QNetworkAccessManager &nam, const QString &profileUrl, const QString &referrerUrl,
                        const QString &imgUrl, const std::optional<int> &profNum, scrapeff::HttpClientProfile profile)
{
    const std::optional<int> did = scrapeff::childhoodPhotoDidFromImageUrl(imgUrl);
    const QString galleryKey = profileUrl + QLatin1Char('\t') + imgUrl;
    {
        QMutexLocker lk(&sh.mutex);
        if (profNum && sh.acceptedProfileNums.contains(*profNum))
            return false;
        if (sh.acceptedGalleryKeys.contains(galleryKey))
            return false;
    }

    QByteArray bytes;
    if (getBytesLimited(nam, QUrl(imgUrl), &bytes, kImageBytesMax, kImageTransferTimeoutMs, profile, referrerUrl) != 200
        || bytes.isEmpty())
        return false;

    QImage im;
    if (!im.loadFromData(bytes)) {
        qWarning() << "tryAddGalleryImage: failed to decode image:" << imgUrl;
        return false;
    }

    QMutexLocker lk(&sh.mutex);
    if (profNum && sh.acceptedProfileNums.contains(*profNum))
        return false;
    if (sh.acceptedGalleryKeys.contains(galleryKey))
        return false;
    if (profNum)
        sh.acceptedProfileNums.insert(*profNum);
    sh.acceptedGalleryKeys.insert(galleryKey);
    sh.galleryEntries.append({profileUrl, imgUrl, did, profNum});
    sh.imageUrlFileLines.append(imgUrl);

    if (!sh.galleryJsonPath.isEmpty())
        writeGalleryJson(sh.galleryJsonPath, sh);
    return true;
}

void processOneRequestedUrl(const QString &requestedUrl, Shared &sh, QNetworkAccessManager &nam,
                              scrapeff::HttpClientProfile profile)
{
    logScrapeUrlProgress(&sh, QLatin1String("TRY\t") + requestedUrl);

    QString html;
    QUrl effective;
    const int st = getHtmlPage(nam, QUrl(requestedUrl), &html, &effective, profile);
    const QString pageEff = effective.toString(QUrl::FullyEncoded);

    const QString redirected =
        pageEff.trimmed().isEmpty() || pageEff.trimmed() == requestedUrl.trimmed()
            ? QString()
            : (QStringLiteral("\tredirect=") + pageEff);

    if (st != 200 || html.isEmpty()) {
        QMutexLocker lk(&sh.mutex);
        sh.fetchFailedLines.append(requestedUrl + QLatin1Char('\t') + QString::number(st));
        logScrapeUrlProgress(&sh, QStringLiteral("END\t") + requestedUrl + QLatin1String("\thttp=%1\t#foto_hrefs=-")
                                                      .arg(QString::number(st))
                                                  + QStringLiteral("\tstate=fetch_failed") + redirected);
        return;
    }

    {
        QMutexLocker lk(&sh.mutex);
        sh.profileUrlsQueuedBeforeImages.append(requestedUrl);
    }

    const QStringList images = scrapeff::extractImageLinks(html, pageEff);
    const qsizetype nHrefs = images.size();

    if (images.isEmpty()) {
        logScrapeUrlProgress(&sh, QStringLiteral("END\t") + requestedUrl + QLatin1String("\thttp=%1\t#foto_hrefs=0")
                                                      .arg(QString::number(st))
                                                  + QLatin1String("\tstate=no_images_in_selector") + redirected);
        return;
    }

    {
        QMutexLocker lk(&sh.mutex);
        sh.results.insert(requestedUrl, images);
    }

    std::optional<int> profileId =
        scrapeff::donorNumberFromProfileUrl(requestedUrl.trimmed());
    if (!profileId)
        profileId = scrapeff::donorNumberFromProfileUrl(pageEff);

    {
        QMutexLocker lk(&sh.mutex);
        for (const QString &u : images) {
            const std::optional<int> didList = scrapeff::childhoodPhotoDidFromImageUrl(u);
            if (didList) {
                const QString dk = QString::number(*didList);
                sh.childhoodDidCounts[dk] = sh.childhoodDidCounts.value(dk, 0) + 1;
            }
        }
    }
    // Save profile URL before image fetching - use requestedUrl (original) for storage,
    // pageEff (after redirects) for HTTP referrer
    const QString savedProfileUrl = requestedUrl;
    int galleryAdded = 0;
    for (const QString &u : images) {
        if (tryAddGalleryImage(sh, nam, savedProfileUrl, pageEff, u, profileId, profile))
            ++galleryAdded;
    }

    const QString galleryState =
        galleryAdded > 0 ? QStringLiteral("\tstate=ok_gallery\tgallery_added=%1").arg(galleryAdded)
                         : QStringLiteral("\tstate=ok_no_gallery\tgallery_added=0");
    logScrapeUrlProgress(&sh, QStringLiteral("END\t") + requestedUrl + QLatin1String("\thttp=%1\t#foto_hrefs=%2")
                                                      .arg(QString::number(st), QString::number(nHrefs))
                                                  + galleryState + redirected);
}

void writeTruncatedFiles(const scrapeff::ScrapeSettings &settings)
{
    const QStringList rels = {QStringLiteral("image_urls.txt"), QStringLiteral("fetch_failed.txt"),
                              QStringLiteral("profile_urls_before_image_extract.txt"),
                              QStringLiteral("gallery_data.json")};
    for (const QString &r : rels) {
        QFile f(QDir(settings.workDir).filePath(r));
        (void) f.open(QIODevice::WriteOnly | QIODevice::Truncate);
    }
}

bool writeGalleryJson(const QString &path, const Shared &sh)
{
    // Caller must hold sh.mutex when flushing during an active scrape.
    QJsonArray imgs;
    for (const auto &t : sh.galleryEntries) {
        const auto &[page, src, did, pnum] = t;
        QJsonObject o;
        o.insert(QStringLiteral("page"), page);
        o.insert(QStringLiteral("src"), src);
        if (did)
            o.insert(QStringLiteral("did"), *did);
        if (pnum)
            o.insert(QStringLiteral("profile_number"), *pnum);
        imgs.append(o);
    }
    QJsonObject root;
    root.insert(QStringLiteral("images"), imgs);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning() << "writeGalleryJson: failed to open" << path << f.errorString();
        return false;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return true;
}

void writeDidCounts(const QString &path, const QHash<QString, int> &counts)
{
    QJsonObject o;
    QList<QString> keys = counts.keys();
    std::sort(keys.begin(), keys.end());
    for (const QString &k : keys)
        o.insert(k, counts.value(k));
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return;
    f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
}

void writeGalleryHtml(const QString &path, const Shared &sh)
{
    QString tiles;
    const auto htmlEsc = [](const QString &s) -> QString {
        return QString(s).replace('&', QStringLiteral("&amp;")).replace('<', QStringLiteral("&lt;")).replace(
            '>', QStringLiteral("&gt;"));
    };
    for (const auto &t : sh.galleryEntries) {
        const auto &[page, src, did, pnum] = t;
        QStringList cap;
        if (did)
            cap.append(QStringLiteral("did=%1").arg(*did));
        if (pnum)
            cap.append(QStringLiteral("profile=%1").arg(*pnum));
        const QString capStr = cap.join(QStringLiteral(" \u00b7 "));
        tiles += QStringLiteral(
                    "      <div class=\"tile-wrap\">\n"
                    "      <button type=\"button\" class=\"tile\" data-full=\"%1\" data-page=\"%2\" "
                    "title=\"Click to enlarge \u00b7 %3\">"
                    "<img src=\"%1\" loading=\"lazy\" alt=\"\"></button>\n"
                    "      <div class=\"cap\">%4</div>\n"
                    "      </div>\n")
                     .arg(htmlEsc(src), htmlEsc(page), htmlEsc(capStr),
                          cap.isEmpty() ? QStringLiteral("\u2014") : htmlEsc(capStr));
    }
    const int nTiles = sh.galleryEntries.size();
    QSet<QString> distinct;
    for (const auto &t : sh.galleryEntries)
        distinct.insert(std::get<1>(t));
    const int nDist = distinct.size();
    const int nPages = sh.results.size();
    QString doc = QStringLiteral(
        "<!DOCTYPE html>\n<html>\n<head>\n<meta charset=\"UTF-8\">\n<title>Image Gallery</title>\n"
        "<style>\n"
        "body { font-family: Arial, sans-serif; margin: 20px; background: #1a1a1a; color: #fff; }\n"
        "h1 { text-align: center; }\n"
        ".summary { text-align: center; margin: 20px 0; padding: 15px; background: #333; border-radius: 10px; }\n"
        ".main { background: #222; border-radius: 10px; padding: 16px; max-width: 1400px; margin: 0 auto; }\n"
        ".main .foto {\n"
        "  display: grid; grid-template-columns: repeat(auto-fill, minmax(140px, 1fr)); gap: 10px;\n"
        "  background: #2a2a2a; border-radius: 8px; padding: 12px;\n"
        "}\n"
        ".main .foto .tile-wrap { display: flex; flex-direction: column; gap: 4px; align-items: center; }\n"
        ".main .foto .cap { font-size: 11px; color: #9cdcfe; text-align: center; word-break: break-all; "
        "max-width: 140px; line-height: 1.2; }\n"
        ".main .foto button.tile { display: block; padding: 0; margin: 0; border: none; border-radius: 6px; "
        "overflow: hidden; cursor: zoom-in; background: #333; aspect-ratio: 1; }\n"
        ".main .foto button.tile:focus { outline: 2px solid #6cf; outline-offset: 2px; }\n"
        ".main .foto button.tile img { width: 100%; height: 100%; object-fit: contain; vertical-align: middle; "
        "display: block; }\n"
        "dialog#imgdlg { border: none; border-radius: 10px; padding: 0; background: #111; max-width: 96vw; "
        "max-height: 96vh; }\n"
        "dialog#imgdlg::backdrop { background: rgba(0,0,0,0.75); }\n"
        "dialog#imgdlg .dlg-bar { display: flex; gap: 12px; align-items: center; padding: 10px 12px; "
        "background: #2a2a2a; justify-content: flex-end; }\n"
        "dialog#imgdlg .dlg-bar a { color: #6cf; }\n"
        "dialog#imgdlg .dlg-body { padding: 8px; max-height: calc(96vh - 52px); overflow: auto; "
        "text-align: center; }\n"
        "dialog#imgdlg .dlg-body img { max-width: 100%; height: auto; border-radius: 6px; }\n"
        "</style>\n</head>\n<body>\n<h1>Image Gallery</h1>\n<div class=\"summary\">\n<strong>%1</strong> "
        "tile(s) \u00b7 <strong>%2</strong> distinct image URL(s) \u00b7 <strong>%3</strong> profile page(s) "
        "with images\n"
        "</div>\n<div class=\"main\"><div class=\"foto\">\n%4\n</div></div>\n"
        "<dialog id=\"imgdlg\"><div class=\"dlg-bar\"><a id=\"dlgopen\" href=\"#\" target=\"_blank\" rel=\"noopener\">Open "
        "profile page</a><button type=\"button\" id=\"dlgclose\">Close</button></div>"
        "<div class=\"dlg-body\"><img id=\"dlgimg\" src=\"\" alt=\"\"></div></dialog>\n<script>\n"
        "(function(){\nvar dlg=document.getElementById(\"imgdlg\");var dlgimg=document.getElementById(\"dlgimg\");"
        "var dlgopen=document.getElementById(\"dlgopen\");"
        "document.getElementById(\"dlgclose\").addEventListener(\"click\",function(){dlg.close();});"
        "document.querySelectorAll(\".main .foto button.tile\").forEach(function(btn){"
        "btn.addEventListener(\"click\",function(){var src=btn.getAttribute(\"data-full\")||\"\";"
        "var page=btn.getAttribute(\"data-page\")||\"\";dlgimg.src=src;dlgopen.href=page||src;dlg.showModal();});});"
        "dlg.addEventListener(\"click\",function(e){if(e.target===dlg)dlg.close();});})();\n</script>\n</body>\n</html>\n");
    doc = doc.arg(QString::number(nTiles)).arg(QString::number(nDist)).arg(QString::number(nPages)).arg(tiles);
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Text))
        f.write(doc.toUtf8());
}

} // namespace

namespace scrapeff {

bool runScrape(const ScrapeSettings &settings)
{
    scrapeff::applySslFromEnv();
    writeTruncatedFiles(settings);
    Shared sh;
    sh.logLine = settings.logLine;
    sh.galleryJsonPath = QDir(settings.workDir).filePath(QStringLiteral("gallery_data.json"));

    if (settings.httpProfile == HttpClientProfile::PythonUrllibCompatible) {
        QNetworkAccessManager nam;
        nam.setCookieJar(new QNetworkCookieJar(&nam));
        for (const QString &u : settings.urls)
            processOneRequestedUrl(u, sh, nam, HttpClientProfile::PythonUrllibCompatible);
    } else {
        QThreadPool::globalInstance()->setMaxThreadCount(qMax(1, settings.threadCount));
        QList<QString> urlList = settings.urls;
        QtConcurrent::blockingMap(urlList, [&](const QString &u) {
            // Create NAM locally per invocation to avoid thread_local destruction issues
            // when QThreadPool threads outlive QCoreApplication
            QNetworkAccessManager nam;
            processOneRequestedUrl(u, sh, nam, settings.httpProfile);
        });
        // Release QtConcurrent worker threads before caller continues (avoids SIGSEGV / exit 11).
        QThreadPool::globalInstance()->clear();
        QThreadPool::globalInstance()->waitForDone();
    }

    QFile imgF(QDir(settings.workDir).filePath(QStringLiteral("image_urls.txt")));
    if (imgF.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream ts(&imgF);
        for (const QString &ln : sh.imageUrlFileLines)
            ts << ln << QLatin1Char('\n');
    }
    QFile ff(QDir(settings.workDir).filePath(QStringLiteral("fetch_failed.txt")));
    if (ff.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream ts(&ff);
        for (const QString &ln : sh.fetchFailedLines)
            ts << ln << QLatin1Char('\n');
    }

    QFile pre(QDir(settings.workDir).filePath(QStringLiteral("profile_urls_before_image_extract.txt")));
    if (pre.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream tsp(&pre);
        for (const QString &ln : sh.profileUrlsQueuedBeforeImages)
            tsp << ln << QLatin1Char('\n');
    }

    writeDidCounts(QDir(settings.workDir).filePath(QStringLiteral("childhood_photo_did_counts.json")),
                   sh.childhoodDidCounts);

    if (!sh.results.isEmpty())
        writeGalleryHtml(QDir(settings.workDir).filePath(QStringLiteral("gallery.html")), sh);
    writeGalleryJson(QDir(settings.workDir).filePath(QStringLiteral("gallery_data.json")), sh);
    return true;
}

} // namespace scrapeff
