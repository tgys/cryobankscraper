#include "phash.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

namespace scrapeff {

static void dct1d(const double *in, double *out, int n)
{
    const double scale0 = 1.0 / std::sqrt(static_cast<double>(n));
    const double scale = std::sqrt(2.0 / static_cast<double>(n));
    for (int k = 0; k < n; ++k) {
        double sum = 0.0;
        for (int i = 0; i < n; ++i)
            sum += in[i] * std::cos(std::numbers::pi / n * (i + 0.5) * k);
        out[k] = (k == 0) ? sum * scale0 : sum * scale;
    }
}

static void dct2d(double *block, int n)
{
    std::vector<double> tmp(n * n);
    std::vector<double> row(n), rowOut(n);
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x)
            row[x] = block[y * n + x];
        dct1d(row.data(), rowOut.data(), n);
        for (int x = 0; x < n; ++x)
            tmp[y * n + x] = rowOut[x];
    }
    std::vector<double> col(n), colOut(n);
    for (int x = 0; x < n; ++x) {
        for (int y = 0; y < n; ++y)
            col[y] = tmp[y * n + x];
        dct1d(col.data(), colOut.data(), n);
        for (int y = 0; y < n; ++y)
            block[y * n + x] = colOut[y];
    }
}

static double sampleLumaNearest(const QImage &img, int x, int y)
{
    const auto *line = reinterpret_cast<const QRgb *>(img.constScanLine(y));
    const QRgb px = line[x];
    return 0.299 * qRed(px) + 0.587 * qGreen(px) + 0.114 * qBlue(px);
}

std::uint64_t perceptualHash64(const QImage &rgbImage)
{
    if (rgbImage.isNull())
        return 0;
    // Avoid QImage::convertToFormat / scaled(SmoothTransformation) — those call into
    // libQt6Gui and segfault when invoked from QtConcurrent worker threads under QCoreApplication.
    const QImage::Format fmt = rgbImage.format();
    if (fmt != QImage::Format_RGB32 && fmt != QImage::Format_ARGB32 && fmt != QImage::Format_ARGB32_Premultiplied)
        return 0;

    const int srcW = rgbImage.width();
    const int srcH = rgbImage.height();
    if (srcW <= 0 || srcH <= 0)
        return 0;

    const int N = 32;
    std::vector<double> pix(N * N);
    for (int y = 0; y < N; ++y) {
        const int sy = qMin(srcH - 1, y * srcH / N);
        for (int x = 0; x < N; ++x) {
            const int sx = qMin(srcW - 1, x * srcW / N);
            pix[y * N + x] = sampleLumaNearest(rgbImage, sx, sy);
        }
    }
    dct2d(pix.data(), N);
    const int hashSize = 8;
    std::vector<double> low(hashSize * hashSize);
    for (int y = 0; y < hashSize; ++y)
        for (int x = 0; x < hashSize; ++x)
            low[y * hashSize + x] = pix[y * N + x];
    double med = 0;
    for (double v : low)
        med += v;
    med /= static_cast<double>(low.size());
    std::uint64_t h = 0;
    for (std::size_t i = 0; i < low.size(); ++i) {
        if (low[i] > med)
            h |= (std::uint64_t(1) << i);
    }
    return h;
}

bool isNearDuplicate(std::uint64_t h, const std::vector<std::uint64_t> &seen, int maxDistance)
{
    for (std::uint64_t p : seen) {
        if (hammingDistance(h, p) <= maxDistance)
            return true;
    }
    return false;
}

} // namespace scrapeff
