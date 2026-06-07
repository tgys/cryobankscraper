#include "url_parse.h"

#include <optional>

#include <QUrl>
#include <QUrlQuery>

namespace scrapeff {

std::optional<int> childhoodPhotoDidFromImageUrl(const QString &imageUrl)
{
    const QUrl u(imageUrl);
    if (!u.path().contains(QLatin1String("childhoodphoto.ashx"), Qt::CaseInsensitive))
        return std::nullopt;
    const QUrlQuery q(u.query());
    if (!q.hasQueryItem(QLatin1String("did")))
        return std::nullopt;
    bool ok = false;
    const int v = q.queryItemValue(QLatin1String("did")).toInt(&ok);
    if (!ok || v < 0)
        return std::nullopt;
    return v;
}

std::optional<int> donorNumberFromProfileUrl(const QString &pageUrl)
{
    const QUrlQuery q(QUrl(pageUrl).query());
    for (const QLatin1String key : {QLatin1String("number"), QLatin1String("donorid"), QLatin1String("id")}) {
        if (!q.hasQueryItem(key))
            continue;
        bool ok = false;
        const int v = q.queryItemValue(key).toInt(&ok);
        if (ok && v >= 0)
            return v;
    }
    return std::nullopt;
}

} // namespace scrapeff
