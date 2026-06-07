#pragma once

#include <optional>

#include <QString>

namespace scrapeff {

std::optional<int> childhoodPhotoDidFromImageUrl(const QString &imageUrl);
std::optional<int> donorNumberFromProfileUrl(const QString &pageUrl);

} // namespace scrapeff
