#pragma once

#include <QString>
#include <QStringList>

#include <functional>

namespace scrapeff {

/** Network + concurrency profile (gallery errors are orthogonal; this affects scrapeff scraping only). */
enum class HttpClientProfile {
    /** Parallel workers, Chromium-like headers + pacing tuned for Fairfax / Cloudflare. */
    ChromiumAutomation,
    /** Single-threaded, shared QNetworkCookieJar, python-requests minimal headers (~requests.Session semantics). */
    PythonUrllibCompatible,
};

struct ScrapeSettings {
    QString workDir;
    QStringList urls;
    int threadCount = 8;
    HttpClientProfile httpProfile = HttpClientProfile::ChromiumAutomation;
    /** When set, TRY/END progress lines go here instead of stdout (gallery in-process scrape). */
    std::function<void(const QString &line)> logLine;
};

bool runScrape(const ScrapeSettings &settings);

} // namespace scrapeff
