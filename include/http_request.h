#pragma once

#include <QNetworkRequest>
#include <QString>
#include <QUrl>

namespace scrapeff {

/**
 * QNetworkRequest tuned like a Chrome document or subresource fetch.
 * @param referrerOpt For HTML: usually empty (top-level navigation). For images: profile page URL so
 *    Sec-Fetch-* headers plus Referer behave like a real browser subresource load.
 */
QNetworkRequest makeScraperNetworkRequest(const QUrl &url, int transferTimeoutMs, bool forHtml,
                                          const QString &referrerOpt = {});

/**
 * Thumbnail / full-image loads in gallery_qt. Same headers as makePythonRequestsCompatibleRequest
 * so CDN behavior matches the default scrapeff / scrape_requests urllib profile.
 */
QNetworkRequest makeGalleryImageNetworkRequest(const QUrl &url, int transferTimeoutMs,
                                               const QString &referrerOpt = {});

/**
 * Headers close to python-requests + urllib3 defaults (naive script, no Chromium client hints).
 */
QNetworkRequest makePythonRequestsCompatibleRequest(const QUrl &url, int transferTimeoutMs, bool forHtml,
                                                    const QString &referrerOpt = {});

} // namespace scrapeff
