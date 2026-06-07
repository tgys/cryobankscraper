#pragma once

#include <QImage>
#include <cstdint>
#include <vector>

namespace scrapeff {

std::uint64_t perceptualHash64(const QImage &rgbImage);

inline int hammingDistance(std::uint64_t a, std::uint64_t b)
{
    int n = 0;
    std::uint64_t x = a ^ b;
    while (x) {
        n++;
        x &= x - 1;
    }
    return n;
}

bool isNearDuplicate(std::uint64_t h, const std::vector<std::uint64_t> &seen, int maxDistance);

} // namespace scrapeff
