#pragma once

#include <QString>
#include <QStringList>

#include <functional>

enum class BuiltinScraperEntry {
    ScrapeffDefault,
    PythonRequestsCompatible,
};

using ScrapeLogSink = std::function<void(const QString &line)>;

struct BuiltinScrapeOptions {
    QString workDir;
    BuiltinScraperEntry entry = BuiltinScraperEntry::ScrapeffDefault;
    QStringList cliArgs;
    ScrapeLogSink log;
};

/** Same URL discovery + runScrape path as the CLI, without creating QCoreApplication. */
int runBuiltinScrape(const BuiltinScrapeOptions &opts);

int runCliScraper(int argc, char *argv[], BuiltinScraperEntry entry);
