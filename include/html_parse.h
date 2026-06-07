#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

namespace scrapeff {

QStringList urlsFromSrcset(const QString &srcset);

QStringList extractImageLinks(const QString &htmlUtf8, const QString &baseUrl);

struct ProfileDownloadLinks {
    QString summaryProfile;
    QString medicalProfile;
    QString personalProfile;
};

ProfileDownloadLinks findDonorDownloadLinks(const QString &htmlUtf8, const QString &baseUrl);

QStringList profileDocumentUrlsSummaryMedical(const QString &htmlUtf8, const QString &baseUrl);
QStringList profileDocumentUrlsAllProfileDocs(const QString &htmlUtf8, const QString &baseUrl);

} // namespace scrapeff
