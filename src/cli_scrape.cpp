#include "cli_scrape.h"
#include "http_request.h"
#include "scrape_runner.h"
#include "ssl_utils.h"
#include "url_parse.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QTextStream>
#include <QUrl>

#include <QThreadPool>

#include <algorithm>
#include <climits>

namespace {

QStringList donorProfileUrlsFairfaxCryobankInHtml(const QString &html)
{
    static const QRegularExpression re(
        QStringLiteral(R"((?:https://fairfaxcryobank\.com)?/search/donorprofile\.aspx\?[^"'\s<>]+)"),
        QRegularExpression::CaseInsensitiveOption);
    QStringList out;
    qsizetype pos = 0;
    forever {
        const QRegularExpressionMatch m = re.match(html, pos);
        if (!m.hasMatch())
            break;
        QString href = m.captured(0);
        href.replace(QLatin1String("&amp;"), QStringLiteral("&"));
        pos = m.capturedEnd(0);
        if (!href.contains(QLatin1String("number="), Qt::CaseInsensitive))
            continue;
        if (href.startsWith(QLatin1String("/")))
            href.prepend(QLatin1String("https://fairfaxcryobank.com"));
        href = QUrl(href).toString(QUrl::FullyEncoded);
        if (!href.isEmpty())
            out.append(href);
    }
    return out;
}

static QString blockingHtmlGetListing(const QString &landingUrlStr)
{
    QUrl landing(landingUrlStr);
    QNetworkAccessManager nam;
    QNetworkRequest rq = scrapeff::makeScraperNetworkRequest(landing, 25000, true,
                                                             QStringLiteral("https://fairfaxcryobank.com/"));
    QNetworkReply *reply = nam.get(rq);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    const QByteArray raw = reply->readAll();
    const QVariant hc = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    reply->deleteLater();
    const int st = hc.isValid() ? hc.toInt() : 0;
    if (st != 200)
        return {};
    return QString::fromUtf8(raw);
}

static void emitLog(const ScrapeLogSink &log, const QString &line)
{
    if (log)
        log(line);
    else {
        QTextStream ts(stdout);
        ts << line;
        if (!line.endsWith(QLatin1Char('\n')))
            ts << QLatin1Char('\n');
        ts.flush();
    }
}

static QStringList discoverFairfaxListedDonorProfileUrls(const ScrapeLogSink &log)
{
    const QStringList landings = {QStringLiteral("https://fairfaxcryobank.com/meet-our-newest-donors/"),
                                  QStringLiteral("https://fairfaxcryobank.com/meet-our-newest-donors")};
    QSet<QString> seen;
    QStringList out;
    for (const QString &pg : landings) {
        const QString html = blockingHtmlGetListing(pg);
        if (html.isEmpty()) {
            emitLog(log, QStringLiteral("Listing harvest: skip or empty response for %1").arg(pg));
            continue;
        }
        int nNew = 0;
        const QStringList found = donorProfileUrlsFairfaxCryobankInHtml(html);
        for (const QString &u : found) {
            if (seen.contains(u))
                continue;
            seen.insert(u);
            out.append(u);
            ++nNew;
        }
        emitLog(log, QStringLiteral("Listing harvest: %1 donor href(s) scanned on %2 → %3 new unique canonical URLs.")
                    .arg(found.size())
                    .arg(pg)
                    .arg(nNew));
    }
    emitLog(log, QStringLiteral("Listing harvest: total %1 distinct donor URLs (checked before brute range).\n")
                .arg(out.size()));
    return out;
}

static bool scrapingFairfaxDonorProfiles(const QStringList &baseUrls)
{
    for (const QString &b : baseUrls) {
        const QString lc = b.toLower();
        if (lc.contains(QLatin1String("fairfaxcryobank.com")) && lc.contains(QLatin1String("donorprofile.aspx")))
            return true;
    }
    return false;
}

static void sortUrlsByDonorId(QStringList &urls)
{
    std::sort(urls.begin(), urls.end(), [](const QString &a, const QString &b) {
        const int ai = scrapeff::donorNumberFromProfileUrl(a).value_or(INT_MAX);
        const int bi = scrapeff::donorNumberFromProfileUrl(b).value_or(INT_MAX);
        if (ai != bi)
            return ai < bi;
        return a < b;
    });
}

static QStringList mergeDiscoveredAhead(const QStringList &discoveredInPageOrder,
                                        QStringList &&bruteNeedsSort)
{
    sortUrlsByDonorId(bruteNeedsSort);
    QSet<QString> seen;
    QStringList out;
    out.reserve(std::max((qsizetype)0, discoveredInPageOrder.size() + bruteNeedsSort.size()));
    for (const QString &u : discoveredInPageOrder) {
        if (seen.contains(u))
            continue;
        seen.insert(u);
        out.append(u);
    }
    for (const QString &u : bruteNeedsSort) {
        if (seen.contains(u))
            continue;
        seen.insert(u);
        out.append(u);
    }
    return out;
}

static QStringList mergeUrlListsUnique(QStringList first, const QStringList &second)
{
    QSet<QString> seen;
    QStringList out;
    out.reserve(first.size() + second.size());
    auto addAll = [&](const QStringList &lst) {
        for (const QString &u : lst) {
            if (u.isEmpty() || seen.contains(u))
                continue;
            seen.insert(u);
            out.append(u);
        }
    };
    addAll(first);
    addAll(second);
    return out;
}

static int fairfaxMaxDonorIdExclusive()
{
    bool ok = false;
    const int n = QString::fromLocal8Bit(qgetenv("SCRAPEFF_FAIRFAX_MAX_ID")).trimmed().toInt(&ok);
    if (ok && n >= 1 && n <= 50000)
        return n;
    return 10000;
}

static QString fairfaxDonorNumberSuffix(int donorId)
{
    return QString::number(donorId).rightJustified(4, QLatin1Char('0'));
}

static QStringList donorNumberSuffixVariants(int donorId)
{
    const QString padded = fairfaxDonorNumberSuffix(donorId);
    const QString unpadded = QString::number(donorId);
    if (padded == unpadded)
        return {padded};
    return {padded, unpadded};
}

QStringList loadBaseUrls(const QString &workDir)
{
    const QString path = QDir(workDir).filePath(QStringLiteral("base_urls.txt"));
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {QStringLiteral("https://fairfaxcryobank.com/search/donorprofile.aspx?number=")};
    }
    QStringList out;
    QTextStream ts(&f);
    while (!ts.atEnd()) {
        const QString ln = ts.readLine().trimmed();
        if (!ln.isEmpty() && !ln.startsWith(QLatin1Char('#')))
            out.append(ln);
    }
    if (out.isEmpty())
        out.append(QStringLiteral("https://fairfaxcryobank.com/search/donorprofile.aspx?number="));
    return out;
}

QStringList buildTargetUrls(const QStringList &baseUrls)
{
    QSet<QString> seen;
    QStringList out;
    const int maxId = fairfaxMaxDonorIdExclusive();
    for (const QString &b : baseUrls) {
        for (int i = 0; i < maxId; ++i) {
            for (const QString &suffix : donorNumberSuffixVariants(i)) {
                const QString p = QStringLiteral("%1%2").arg(b).arg(suffix);
                if (!seen.contains(p)) {
                    seen.insert(p);
                    out.append(p);
                }
            }
        }
    }
    sortUrlsByDonorId(out);
    return out;
}

QStringList loadTargetUrlsFromTxt(const QString &workDir)
{
    QFile tu(QDir(workDir).filePath(QStringLiteral("target_urls.txt")));
    if (!tu.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    QStringList out;
    QTextStream ts(&tu);
    while (!ts.atEnd()) {
        const QString ln = ts.readLine().trimmed();
        if (!ln.isEmpty() && !ln.startsWith(QLatin1Char('#')))
            out.append(ln);
    }
    return out;
}

static int scrapeffThreadCountEnv()
{
    bool ok = false;
    const int n = QString::fromLocal8Bit(qgetenv("SCRAPEFF_THREADS")).trimmed().toInt(&ok);
    if (ok && n >= 1 && n <= 64)
        return n;
    return 4;
}

static QStringList argsList(int argc, char *argv[])
{
    QStringList a;
    a.reserve(argc);
    for (int i = 0; i < argc; ++i)
        a.append(QString::fromLocal8Bit(argv[i]));
    return a;
}

static scrapeff::HttpClientProfile resolveHttpProfile(BuiltinScraperEntry entry, const QStringList &args)
{
    bool forceUrllibCompat = false;
    bool forceChromiumCompat = false;
    for (const QString &a : args) {
        if (a == QStringLiteral("--urllib-compat") || a == QStringLiteral("--requests-compat")
            || a == QStringLiteral("--python-compat"))
            forceUrllibCompat = true;
        if (a == QStringLiteral("--chrome-style") || a == QStringLiteral("--browser-compat"))
            forceChromiumCompat = true;
    }

    const QString profileEnv =
        QString::fromLocal8Bit(qgetenv("SCRAPEFF_HTTP_PROFILE")).trimmed().toLower();

    scrapeff::HttpClientProfile profile = scrapeff::HttpClientProfile::ChromiumAutomation;
    if (entry == BuiltinScraperEntry::PythonRequestsCompatible)
        profile = scrapeff::HttpClientProfile::PythonUrllibCompatible;
    if (!profileEnv.isEmpty()
        && (profileEnv == QLatin1String("urllib") || profileEnv == QLatin1String("python")
            || profileEnv == QLatin1String("requests")))
        forceUrllibCompat = true;

    if (forceUrllibCompat && !forceChromiumCompat)
        profile = scrapeff::HttpClientProfile::PythonUrllibCompatible;
    if (forceChromiumCompat)
        profile = scrapeff::HttpClientProfile::ChromiumAutomation;
    return profile;
}

} // namespace

int runBuiltinScrape(const BuiltinScrapeOptions &opts)
{
    scrapeff::applySslFromEnv();

    const QString workDir = opts.workDir;
    const ScrapeLogSink &log = opts.log;
    const scrapeff::HttpClientProfile profile = resolveHttpProfile(opts.entry, opts.cliArgs);

    const QStringList supplemental = loadTargetUrlsFromTxt(workDir);
    const QStringList baseUrls = loadBaseUrls(workDir);

    const QByteArray onlyFileRaw = qgetenv("SCRAPEFF_TARGET_URLS_ONLY").trimmed();
    const bool targetUrlsOnly =
        onlyFileRaw == QLatin1String("1")
        || QString::fromLatin1(onlyFileRaw.constData(), onlyFileRaw.size())
               .compare(QLatin1String("true"), Qt::CaseInsensitive) == 0;

    QStringList allUrls;

    if (targetUrlsOnly && !supplemental.isEmpty()) {
        allUrls = supplemental;
        emitLog(log, QStringLiteral("URL list — SCRAPEFF_TARGET_URLS_ONLY: using %1 URL(s) from target_urls.txt only.")
                    .arg(allUrls.size()));
    } else if (scrapingFairfaxDonorProfiles(baseUrls)) {
        QStringList brute = buildTargetUrls(baseUrls);
        QStringList disc = discoverFairfaxListedDonorProfileUrls(log);
        const QByteArray bruteSkipEnvRaw = qgetenv("SCRAPEFF_FAIRFAX_SKIP_BRUTE_IDS").trimmed();
        const QString bruteSkipEnv =
            bruteSkipEnvRaw.isEmpty()
                ? QString()
                : QString::fromLatin1(bruteSkipEnvRaw.constData(), bruteSkipEnvRaw.size()).trimmed();
        const bool bruteOnlyListed =
            bruteSkipEnv == QLatin1String("1")
            || bruteSkipEnv.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0;
        const qsizetype nBrute = brute.size();
        const int maxId = fairfaxMaxDonorIdExclusive();
        if (bruteOnlyListed && !disc.isEmpty()) {
            allUrls = disc;
            emitLog(log,
                    QStringLiteral(
                        "URL list — SCRAPEFF_FAIRFAX_SKIP_BRUTE_IDS: using %1 harvested donor URLs only "
                        "(skipping brute %2 candidate URLs).\n")
                        .arg(allUrls.size())
                        .arg(nBrute));
        } else {
            allUrls = mergeDiscoveredAhead(disc, std::move(brute));
            emitLog(log,
                    QStringLiteral("URL list — donor IDs 0–%1 checked (%2 candidate URLs, %3 base prefix(es)); "
                                   "listed donors scraped first → %4 unique URLs total.")
                        .arg(maxId - 1)
                        .arg(nBrute)
                        .arg(baseUrls.size())
                        .arg(allUrls.size()));
            if (bruteOnlyListed && disc.isEmpty())
                emitLog(log,
                        QStringLiteral(
                            "(SCRAPEFF_FAIRFAX_SKIP_BRUTE_IDS set but harvesting found 0 links — fallback "
                            "to full brute range.)"));
        }
        if (!supplemental.isEmpty()) {
            const qsizetype before = allUrls.size();
            allUrls = mergeUrlListsUnique(std::move(allUrls), supplemental);
            if (allUrls.size() > before) {
                emitLog(log, QStringLiteral("URL list — merged %1 supplemental URL(s) from existing target_urls.txt.")
                            .arg(allUrls.size() - before));
            }
        }
    } else if (!supplemental.isEmpty()) {
        allUrls = supplemental;
        emitLog(log, QStringLiteral("URL list — loaded %1 URLs from target_urls.txt (base_urls.txt base count: %2).")
                    .arg(allUrls.size())
                    .arg(baseUrls.size()));
    } else {
        allUrls = buildTargetUrls(baseUrls);
        emitLog(log, QStringLiteral("URL list — generated %1 URLs (%2 donor ID(s) × %3 base URL(s)).")
                    .arg(allUrls.size())
                    .arg(fairfaxMaxDonorIdExclusive())
                    .arg(baseUrls.size()));
    }

    QFile tu(QDir(workDir).filePath(QStringLiteral("target_urls.txt")));
    if (tu.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream ts(&tu);
        for (const QString &u : allUrls)
            ts << u << QLatin1Char('\n');
    }
    emitLog(log, QStringLiteral("Written target_urls.txt (active run).\n"));

    scrapeff::ScrapeSettings s;
    s.workDir = workDir;
    s.urls = allUrls;
    s.httpProfile = profile;
    if (log)
        s.logLine = [log](const QString &line) { log(line + QLatin1Char('\n')); };

    if (profile == scrapeff::HttpClientProfile::PythonUrllibCompatible) {
        emitLog(log, QStringLiteral(
                           "HTTP client: Python requests / urllib3–style (%1 thread, cookie jar persists across "
                           "profile + image hits like requests.Session).")
                    .arg(1));
        emitLog(log, QStringLiteral(
                           "(Use scrapeff defaults for Chromium-style parallelism; use --urllib-compat there, or "
                           "SCRAPEFF_HTTP_PROFILE=urllib.)"));
        s.threadCount = 1;
    } else {
        s.threadCount = scrapeffThreadCountEnv();
        emitLog(log, QStringLiteral("HTTP client: Chromium-style automation headers."));
        emitLog(log, QStringLiteral(
                           "Concurrency: %1 workers (SCRAPEFF_THREADS); pacing: SCRAPEFF_HTTP_SPACING_MS (default "
                           "~150 ms, 0 disables).")
                    .arg(s.threadCount));
    }

    emitLog(log, QStringLiteral("\nFetching and extracting images from %1 URLs...\n").arg(allUrls.size()));
    scrapeff::runScrape(s);
    return 0;
}

int runCliScraper(int argc, char *argv[], BuiltinScraperEntry entry)
{
    QCoreApplication app(argc, argv);
    BuiltinScrapeOptions opts;
    opts.workDir = QDir::currentPath();
    opts.entry = entry;
    opts.cliArgs = argsList(argc, argv);
    return runBuiltinScrape(opts);
}
