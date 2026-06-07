#include "mainwindow.h"

#include "cli_scrape.h"
#include "http_request.h"
#include "ssl_utils.h"
#include "url_parse.h"

#include <QAction>

#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QCloseEvent>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <functional>
#include <QMouseEvent>
#include <QScreen>
#include <QDialog>
#include <QFile>
#include <QFileDialog>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLabel>
#include <QLineEdit>
#include <QLoggingCategory>
#include <QMenuBar>
#include <QMessageBox>
#include <QMetaObject>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkCookieJar>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTextEdit>
#include <QTimer>
#include <QDesktopServices>
#include <QUrlQuery>
#include <QVBoxLayout>

#include <QSet>
#include <QTextStream>

#include <algorithm>
#include <optional>

static QNetworkRequest imageRequest(const QUrl &url, const QString &referrerProfile = {})
{
    return scrapeff::makeGalleryImageNetworkRequest(url, 30000, referrerProfile);
}

static QUrl galleryHttpUrlFromString(const QString &raw)
{
    const QString s = raw.trimmed();
    if (s.isEmpty())
        return {};
    if (s.startsWith(QLatin1String("//")))
        return QUrl(QLatin1String("https:") + s);
    QUrl u = QUrl::fromUserInput(s);
    if (u.isValid() && !u.scheme().isEmpty())
        return u;
    return QUrl(s, QUrl::StrictMode);
}

/** Mirrors a first hit with urllib3 `requests.Session`—cookie jar picks up CDN / ASP.NET markers before many parallel GETs. */
static void blockingFairfaxWarmup(QNetworkAccessManager *nam)
{
    if (!nam)
        return;
    const QString skip = QString::fromLocal8Bit(qgetenv("SCRAPEFF_GALLERY_SKIP_WARMUP")).trimmed();
    if (skip == QLatin1String("1") || skip.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0)
        return;

    QNetworkReply *reply = nam->get(scrapeff::makePythonRequestsCompatibleRequest(
        QUrl(QStringLiteral("https://fairfaxcryobank.com/search/")), 25000, true, {}));

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    reply->deleteLater();
}

constexpr int kMaxParallel = 8;

namespace {

bool looksLikeProfileUrl(const QString &u)
{
    if (u.isEmpty())
        return false;
    const QString lu = u.toLower();
    return lu.contains(QStringLiteral("donorprofile")) || lu.contains(QStringLiteral("/search/donor"));
}

bool entryHasDonorProfilePic(const GalleryEntry &e)
{
    return scrapeff::childhoodPhotoDidFromImageUrl(e.imageUrl).has_value();
}

bool looksLikeDirectImageUrl(const QString &u)
{
    if (u.isEmpty())
        return false;
    const QString lu = u.toLower();
    if (lu.contains(QStringLiteral("childhoodphoto")))
        return true;
    if (lu.contains(QStringLiteral("/wp-content/")) || lu.contains(QStringLiteral("/uploads/"))
        || lu.contains(QStringLiteral("/images-new/")))
        return true;
    static const QStringList suf = {QStringLiteral(".jpg"), QStringLiteral(".jpeg"), QStringLiteral(".png"),
                                    QStringLiteral(".gif"), QStringLiteral(".webp"), QStringLiteral(".svg"),
                                    QStringLiteral(".ico"), QStringLiteral(".bmp"), QStringLiteral(".ashx")};
    for (const QString &s : suf) {
        if (lu.endsWith(s))
            return true;
    }
    return false;
}

QPair<QString, QString> coerceImageAndProfile(const QString &rawSrc, const QString &rawPage)
{
    const QString a = rawSrc.trimmed();
    const QString b = rawPage.trimmed();
    if (a.isEmpty() && b.isEmpty())
        return {{}, {}};
    if (!a.isEmpty() && b.isEmpty())
        return {a, {}};
    if (a.isEmpty() && !b.isEmpty())
        return {{}, b};
    const bool aProf = looksLikeProfileUrl(a);
    const bool bProf = looksLikeProfileUrl(b);
    const bool aImg = looksLikeDirectImageUrl(a);
    const bool bImg = looksLikeDirectImageUrl(b);
    if (aImg && bProf)
        return {a, b};
    if (bImg && aProf)
        return {b, a};
    if (aProf && !bProf && bImg)
        return {b, a};
    if (bProf && !aProf && aImg)
        return {a, b};
    return {a, b};
}

std::optional<int> jsonOptInt(const QJsonValue &v)
{
    if (v.isNull() || v.isUndefined() || v.isBool())
        return std::nullopt;
    if (v.isDouble())
        return int(v.toDouble());
    if (v.isString()) {
        const QString s = v.toString().trimmed();
        bool ok = false;
        const int n = s.toInt(&ok);
        if (ok)
            return n;
    }
    return std::nullopt;
}

QVector<GalleryEntry> entriesFromGalleryRoot(const QJsonObject &data)
{
    QVector<GalleryEntry> out;
    const QJsonValue rawIm = data.value(QStringLiteral("images"));
    if (rawIm.isArray()) {
        const QJsonArray arr = rawIm.toArray();
        if (arr.isEmpty())
            return out;
        if (arr.first().isObject()) {
            for (const QJsonValue &item : arr) {
                if (!item.isObject())
                    continue;
                const QJsonObject o = item.toObject();
                QString rawSrc = o.value(QStringLiteral("src")).toString();
                if (rawSrc.isEmpty())
                    rawSrc = o.value(QStringLiteral("url")).toString();
                if (rawSrc.isEmpty())
                    rawSrc = o.value(QStringLiteral("image")).toString();
                QString rawPage = o.value(QStringLiteral("page")).toString();
                if (rawPage.isEmpty())
                    rawPage = o.value(QStringLiteral("profile")).toString();
                const auto coerced = coerceImageAndProfile(rawSrc, rawPage);
                if (coerced.first.isEmpty())
                    continue;
                std::optional<int> did = jsonOptInt(o.value(QStringLiteral("did")));
                std::optional<int> pn = jsonOptInt(o.value(QStringLiteral("profile_number")));
                if (!did)
                    did = scrapeff::childhoodPhotoDidFromImageUrl(coerced.first);
                if (!pn)
                    pn = scrapeff::donorNumberFromProfileUrl(coerced.second);
                out.append({coerced.first, coerced.second.trimmed(), did, pn});
            }
            return out;
        }
        QSet<QString> seen;
        for (const QJsonValue &u : arr) {
            if (!u.isString())
                continue;
            const QString s = u.toString();
            if (s.isEmpty() || seen.contains(s))
                continue;
            seen.insert(s);
            out.append({s, {}, scrapeff::childhoodPhotoDidFromImageUrl(s), std::nullopt});
        }
        return out;
    }
    const QStringList pages = data.keys();
    QStringList pSorted = pages;
    std::sort(pSorted.begin(), pSorted.end());
    QSet<QString> seen;
    for (const QString &page : pSorted) {
        if (!data.contains(page)) // not a string key with array - skip
            continue;
        const QJsonValue imgsV = data.value(page);
        if (!imgsV.isArray())
            continue;
        const std::optional<int> pn = scrapeff::donorNumberFromProfileUrl(page);
        for (const QJsonValue &uv : imgsV.toArray()) {
            if (!uv.isString())
                continue;
            const QString u = uv.toString();
            if (u.isEmpty() || seen.contains(u))
                continue;
            seen.insert(u);
            out.append({u, page, scrapeff::childhoodPhotoDidFromImageUrl(u), pn});
        }
    }
    return out;
}

QString escapeHtml(const QString &s)
{
    return QString(s)
        .replace('&', QStringLiteral("&amp;"))
        .replace('<', QStringLiteral("&lt;"))
        .replace('>', QStringLiteral("&gt;"))
        .replace('"', QStringLiteral("&quot;"));
}

} // namespace

constexpr int kThumbPx = 280;

class ThumbnailLabel;
class ThumbnailLoadLimiter : public QObject {
public:
    explicit ThumbnailLoadLimiter(QNetworkAccessManager *nam, int maxInFlight, QObject *parent = nullptr)
        : QObject(parent), nam_(nam), max_(qMax(1, maxInFlight))
    {
    }
    void submit(ThumbnailLabel *thumb, const QNetworkRequest &req);
    void cancelQueuedFor(ThumbnailLabel *thumb);
    void release();

private:
    void start(const QNetworkRequest &req, ThumbnailLabel *thumb);
    QNetworkAccessManager *nam_;
    int max_;
    int active_ = 0;
    QList<QPair<QNetworkRequest, ThumbnailLabel *>> queue_;
};

class ThumbnailLabel : public QLabel {
public:
    ThumbnailLabel(const QString &imageUrl, ThumbnailLoadLimiter *lim, QWidget *parent = nullptr,
                   const QString &profilePageUrl = {}, const std::optional<int> &did = std::nullopt,
                   const std::optional<int> &profileNumber = std::nullopt,
                   std::function<void(const QString &, const QString &, std::optional<int>, std::optional<int>)>
                       onMax = {})
        : QLabel(parent)
        , limiter_(lim)
        , url_(imageUrl)
        , pageUrl_(profilePageUrl)
        , did_(did)
        , profileNumber_(profileNumber)
        , onMaximize_(std::move(onMax))
    {
        setFixedSize(kThumbPx, kThumbPx);
        setAlignment(Qt::AlignCenter);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        setFrameShape(QFrame::StyledPanel);
        setStyleSheet(QStringLiteral("QLabel { background: #2a2a2a; color: #888; font-size: 11px; }"));
        setText(QStringLiteral("…"));
        setWordWrap(true);
        const QUrl imgU = galleryHttpUrlFromString(imageUrl);
        if (!imgU.isValid() || imgU.scheme().isEmpty()) {
            setText(QStringLiteral("Bad URL"));
            const QString tip = QStringLiteral("%1\n(Invalid HTTP(S) URL in JSON.)").arg(imageUrl);
            setToolTip(tip);
            return;
        }
        limiter_->submit(this, imageRequest(imgU, profilePageUrl));
    }
    void abortPendingLoad()
    {
        limiter_->cancelQueuedFor(this);
        if (!reply_)
            return;
        disconnect(reply_, nullptr, this, nullptr);
        reply_->abort();
        reply_->deleteLater();
        reply_ = nullptr;
        limiter_->release();
    }
    void bindReply(QNetworkReply *r)
    {
        reply_ = r;
        connect(reply_, &QNetworkReply::finished, this, [this] { finishReply(); });
    }

private:
    void finishReply()
    {
        QNetworkReply *r = reply_;
        reply_ = nullptr;
        if (!r)
            return;
        const int err = int(r->error());
        const QByteArray data = r->readAll();
        const QString errStr = r->errorString();
        r->deleteLater();
        limiter_->release();
        if (err != QNetworkReply::NoError) {
            setText(QStringLiteral("Error"));
            const QVariant httpSt = r->attribute(QNetworkRequest::HttpStatusCodeAttribute);
            QString httpBit;
            if (httpSt.isValid())
                httpBit = QStringLiteral("\nHTTP %1").arg(httpSt.toInt());
            const QString tip = pageUrl_.isEmpty() ? url_ : (pageUrl_ + QLatin1Char('\n') + url_);
            setToolTip(tip + QLatin1Char('\n') + errStr + httpBit);
            return;
        }
        QImage im;
        if (!im.loadFromData(data)) {
            setText(QStringLiteral("Bad data"));
            return;
        }
        QPixmap pix = QPixmap::fromImage(im);
        setScaledContents(false);
        setText({});
        const int w = pix.width();
        const int h = pix.height();
        const qreal s = qMin(qMin(kThumbPx / qreal(qMax(w, 1)), kThumbPx / qreal(qMax(h, 1))), 1.0);
        if (s < 1.0)
            pix = pix.scaled(int(w * s), int(h * s), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        setPixmap(pix);
        setCursor(Qt::PointingHandCursor);
        QStringList tip;
        if (did_)
            tip.append(QStringLiteral("image id=%1").arg(*did_));
        if (profileNumber_)
            tip.append(QStringLiteral("profile=%1").arg(*profileNumber_));
        tip.append(pageUrl_.isEmpty() ? url_ : pageUrl_);
        tip.append(QStringLiteral("Click to enlarge"));
        setToolTip(tip.join(QLatin1Char('\n')));
    }

protected:
    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton && onMaximize_)
            onMaximize_(url_, pageUrl_, did_, profileNumber_);
        else if (e->button() == Qt::LeftButton)
            QDesktopServices::openUrl(QUrl(pageUrl_.isEmpty() ? url_ : pageUrl_));
        QLabel::mousePressEvent(e);
    }

    ThumbnailLoadLimiter *limiter_;
    QString url_;
    QString pageUrl_;
    std::optional<int> did_;
    std::optional<int> profileNumber_;
    std::function<void(const QString &, const QString &, std::optional<int>, std::optional<int>)> onMaximize_;
    QNetworkReply *reply_ = nullptr;
};

void ThumbnailLoadLimiter::submit(ThumbnailLabel *thumb, const QNetworkRequest &req)
{
    if (active_ < max_)
        start(req, thumb);
    else
        queue_.append({req, thumb});
}

void ThumbnailLoadLimiter::cancelQueuedFor(ThumbnailLabel *thumb)
{
    for (int i = queue_.size() - 1; i >= 0; --i) {
        if (queue_.at(i).second == thumb)
            queue_.removeAt(i);
    }
}

void ThumbnailLoadLimiter::start(const QNetworkRequest &req, ThumbnailLabel *thumb)
{
    ++active_;
    QNetworkReply *r = nam_->get(req);
    thumb->bindReply(r);
}

void ThumbnailLoadLimiter::release()
{
    active_ = qMax(0, active_ - 1);
    while (active_ < max_ && !queue_.isEmpty()) {
        const auto pr = queue_.takeFirst();
        start(pr.first, pr.second);
    }
}

class ImageMaxDialog : public QDialog {
public:
    ImageMaxDialog(QNetworkAccessManager *nam, const QString &imageUrl, const QString &profilePageUrl,
                   const std::optional<int> &did, const std::optional<int> &profileNumber, QWidget *parent = nullptr)
        : QDialog(parent)
        , nam_(nam)
    {
        setWindowTitle(QStringLiteral("Image — did, profile id, URLs"));
        if (QScreen *s = QGuiApplication::primaryScreen()) {
            const int aw = s->availableGeometry().width() - 80;
            resize(qMin(960, qMax(480, aw)), 720);
        } else
            resize(960, 720);

        auto *v = new QVBoxLayout(this);
        auto *top = new QHBoxLayout();
        top->addStretch(1);
        auto *topClose = new QPushButton(QStringLiteral("Close"));
        topClose->setObjectName(QStringLiteral("windowCloseButton"));
        topClose->setCursor(Qt::PointingHandCursor);
        connect(topClose, &QPushButton::clicked, this, &QDialog::accept);
        top->addWidget(topClose);
        v->addLayout(top);
        auto addRo = [&](const QString &label, const QString &text) {
            v->addWidget(new QLabel(label));
            auto *le = new QLineEdit(text);
            le->setReadOnly(true);
            v->addWidget(le);
        };
        addRo(QStringLiteral("Image ID (ChildhoodPhoto ?did=):"), did ? QString::number(*did) : QStringLiteral("—"));
        addRo(QStringLiteral("Profile id from main page (?number=, etc.):"),
              profileNumber ? QString::number(*profileNumber) : QStringLiteral("—"));
        addRo(QStringLiteral("Image URL (handler / file src):"), imageUrl);
        auto *ur = new QLabel(profilePageUrl.isEmpty()
                                  ? QStringLiteral("No profile URL in this JSON; re-run scrape for page links.")
                                  : QStringLiteral("Profile page URL (select to copy):"));
        ur->setWordWrap(true);
        v->addWidget(ur);
        auto *urlEdit = new QLineEdit(profilePageUrl.isEmpty() ? imageUrl : profilePageUrl);
        urlEdit->setReadOnly(true);
        v->addWidget(urlEdit);
        imgLabel_ = new QLabel(QStringLiteral("Loading…"));
        imgLabel_->setAlignment(Qt::AlignCenter);
        auto *scroll = new QScrollArea;
        scroll->setWidgetResizable(false);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidget(imgLabel_);
        v->addWidget(scroll, 1);
        auto *bar = new QHBoxLayout();
        auto *openP = new QPushButton(QStringLiteral("Open profile page"));
        openP->setEnabled(!profilePageUrl.isEmpty());
        connect(openP, &QPushButton::clicked, this, [profilePageUrl] {
            if (!profilePageUrl.isEmpty())
                QDesktopServices::openUrl(QUrl(profilePageUrl));
        });
        auto *openI = new QPushButton(QStringLiteral("Open image URL"));
        connect(openI, &QPushButton::clicked, this, [imageUrl] { QDesktopServices::openUrl(QUrl(imageUrl)); });
        bar->addStretch(1);
        bar->addWidget(openI);
        bar->addWidget(openP);
        v->addLayout(bar);

        const QUrl reqUrl = galleryHttpUrlFromString(imageUrl);
        if (!reqUrl.isValid() || reqUrl.scheme().isEmpty()) {
            imgLabel_->setText(QStringLiteral("Invalid image URL"));
        } else {
            QNetworkReply *r = nam_->get(imageRequest(reqUrl, profilePageUrl));
            connect(r, &QNetworkReply::finished, this, [this, r] {
                r->deleteLater();
                if (r->error() != QNetworkReply::NoError) {
                    imgLabel_->setText(r->errorString());
                    return;
                }
                QImage im;
                if (!im.loadFromData(r->readAll())) {
                    imgLabel_->setText(QStringLiteral("Could not decode image"));
                    return;
                }
                QPixmap pix = QPixmap::fromImage(im);
                imgLabel_->setPixmap(pix);
                imgLabel_->resize(pix.size());
            });
        }
        QTimer::singleShot(0, urlEdit, [urlEdit] { urlEdit->selectAll(); });
    }

private:
    QNetworkAccessManager *nam_;
    QLabel *imgLabel_ = nullptr;
};

namespace {

static const char *kDarkStylesheet = R"(
QWidget { background-color: #1e1e1e; color: #e0e0e0; }
QMainWindow { background-color: #1a1a1a; }
QStackedWidget { background-color: #1a1a1a; }
QScrollArea { background-color: #1a1a1a; border: none; }
QMenuBar { background-color: #252526; color: #e0e0e0; padding: 2px 6px; border-bottom: 1px solid #3c3c3c; }
QStatusBar { background-color: #252526; color: #c0c0c0; border-top: 1px solid #3c3c3c; padding: 4px 8px; }
QLabel { color: #e0e0e0; }
QPushButton { background-color: #3c3c3c; color: #f0f0f0; border: 1px solid #555; border-radius: 6px; padding: 8px 16px; font-size: 13px; }
QPushButton:hover { background-color: #4a4a4a; border-color: #666; }
QPushButton:pressed { background-color: #2d2d2d; }
QPushButton:disabled { color: #888; background-color: #333; }
QPushButton#windowCloseButton { background-color: #c42b1c; color: #ffffff; border: 1px solid #e74c3c; font-weight: bold;
  font-size: 15px; min-width: 110px; min-height: 36px; padding: 8px 20px; border-radius: 6px; }
QPushButton#windowCloseButton:hover { background-color: #d73a2a; border-color: #ff6b5a; }
QPushButton#windowCloseButton:pressed { background-color: #a82318; }
QLineEdit { background-color: #252525; color: #6cf; border: 1px solid #444; border-radius: 4px; padding: 8px; selection-background-color: #264f78; }
QTextEdit { background-color: #252525; color: #6cf; border: 1px solid #444; border-radius: 4px; padding: 8px; selection-background-color: #264f78; }
QComboBox { background-color: #3c3c3c; color: #e0e0e0; border: 1px solid #555; border-radius: 4px; padding: 4px 8px; }
QFrame#main { background: #222; border-radius: 10px; }
QFrame#foto { background: #2a2a2a; border-radius: 8px; }
QProgressBar { border: 1px solid #454545; border-radius: 4px; background: #2a2a2a; text-align: center; }
QProgressBar::chunk { background-color: #0e639c; }
)";

class GalleryScrapeThread final : public QThread {
public:
    QString workDir;
    BuiltinScraperEntry entry = BuiltinScraperEntry::ScrapeffDefault;
    ScrapeLogSink log;
    int resultCode = -1;

protected:
    void run() override
    {
        BuiltinScrapeOptions opts;
        opts.workDir = workDir;
        opts.entry = entry;
        opts.log = log;
        resultCode = runBuiltinScrape(opts);
    }
};

} // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("Image gallery"));
    resize(1000, 700);
    scrapeff::applySslFromEnv();
    nam_ = new QNetworkAccessManager(this);
    nam_->setCookieJar(new QNetworkCookieJar(nam_));
    // Qt often follows system proxy; plain requests/curl may not — broken proxies can surface as mass HTTP 404.
    {
        const QString noSysProx = QString::fromLocal8Bit(qgetenv("SCRAPEFF_GALLERY_DISABLE_SYSTEM_PROXY")).trimmed();
        if (noSysProx == QLatin1String("1")
            || noSysProx.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0)
            nam_->setProxy(QNetworkProxy::NoProxy);
    }
    limiter_ = new ThumbnailLoadLimiter(nam_, kMaxParallel, this);

    stack_ = new QStackedWidget(this);
    setCentralWidget(stack_);

    root_ = new QWidget;
    vbox_ = new QVBoxLayout(root_);
    vbox_->setSpacing(16);
    vbox_->setContentsMargins(12, 12, 12, 12);

    galleryScroll_ = new QScrollArea;
    galleryScroll_->setWidgetResizable(true);
    galleryScroll_->setFrameShape(QFrame::NoFrame);
    galleryScroll_->setWidget(root_);

    summary_ = new QLabel;
    summary_->setStyleSheet(QStringLiteral("color: #b8b8b8; font-size: 13px; padding: 6px;"));
    vbox_->addWidget(summary_);

    filterRow_ = new QWidget;
    auto *fh = new QHBoxLayout(filterRow_);
    fh->setContentsMargins(0, 0, 0, 0);
    fh->addWidget(new QLabel(QStringLiteral("Filter:")));
    filterCombo_ = new QComboBox;
    filterCombo_->addItems({QStringLiteral("With profile pic"), QStringLiteral("All donors")});
    connect(filterCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, &MainWindow::onFilterChanged);
    fh->addWidget(filterCombo_, 1);
    vbox_->addWidget(filterRow_);

    sortRow_ = new QWidget;
    auto *sh = new QHBoxLayout(sortRow_);
    sh->setContentsMargins(0, 0, 0, 0);
    sh->addWidget(new QLabel(QStringLiteral("Sort:")));
    sortCombo_ = new QComboBox;
    sortCombo_->addItems({QStringLiteral("Donor id (low → high)"), QStringLiteral("As in file")});
    connect(sortCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, &MainWindow::onSortChanged);
    sh->addWidget(sortCombo_, 1);
    vbox_->addWidget(sortRow_);

    auto *mb = menuBar();
    auto *fileM = mb->addMenu(QStringLiteral("&File"));
    openJsonAction_ = new QAction(QStringLiteral("&Open JSON…"), this);
    connect(openJsonAction_, &QAction::triggered, this, &MainWindow::onOpenFile);
    fileM->addAction(openJsonAction_);
    fileM->addSeparator();
    auto *quitA = new QAction(QStringLiteral("&Quit"), this);
    connect(quitA, &QAction::triggered, this, &QWidget::close);
    fileM->addAction(quitA);

    auto *winClose = new QPushButton(QStringLiteral("Close"));
    winClose->setObjectName(QStringLiteral("windowCloseButton"));
    winClose->setCursor(Qt::PointingHandCursor);
    connect(winClose, &QPushButton::clicked, this, &QWidget::close);
    mb->setCornerWidget(winClose, Qt::TopRightCorner);

    setStatusBar(new QStatusBar);

    loadingPage_ = new QWidget;
    auto *lv = new QVBoxLayout(loadingPage_);
    auto *row = new QHBoxLayout;
    loadingSpinner_ = new QLabel(QStringLiteral("\u283c"));
    QFont spf;
    spf.setPointSize(22);
    loadingSpinner_->setFont(spf);
    loadingSpinner_->setStyleSheet(QStringLiteral("color: #6cf; min-width: 36px;"));
    row->addWidget(loadingSpinner_);
    auto *title = new QLabel(
        QStringLiteral("Running scrapeff — fetching pages and building gallery_data.json…"));
    title->setWordWrap(true);
    row->addWidget(title, 1);
    lv->addLayout(row);
    scrapeProgressLabel_ = new QLabel(QStringLiteral("Preparing donor URL list…"));
    scrapeProgressLabel_->setWordWrap(true);
    scrapeProgressLabel_->setStyleSheet(QStringLiteral("color: #b8b8b8; font-size: 12px;"));
    lv->addWidget(scrapeProgressLabel_);
    scrapeProgressBar_ = new QProgressBar;
    scrapeProgressBar_->setRange(0, 0);
    scrapeProgressBar_->setTextVisible(false);
    scrapeProgressBar_->setFixedHeight(8);
    lv->addWidget(scrapeProgressBar_);
    scrapeLog_ = new QTextEdit;
    scrapeLog_->setReadOnly(true);
    QFont mono(QStringLiteral("Monospace"));
    mono.setStyleHint(QFont::Monospace);
    scrapeLog_->setFont(mono);
    lv->addWidget(scrapeLog_, 1);

    stack_->addWidget(loadingPage_);
    stack_->addWidget(galleryScroll_);
    stack_->setCurrentWidget(galleryScroll_);

    connect(&spinTimer_, &QTimer::timeout, this, &MainWindow::tickSpinner);

    qApp->setStyleSheet(QString::fromUtf8(kDarkStylesheet));
}

void MainWindow::setScrapingBusy(bool busy)
{
    if (openJsonAction_)
        openJsonAction_->setEnabled(!busy);
    if (busy)
        spinTimer_.start(80);
    else
        spinTimer_.stop();
}

void MainWindow::updateScrapeProgressLabel()
{
    if (!scrapeProgressLabel_)
        return;
    if (scrapeUrlsTotal_ <= 0) {
        scrapeProgressLabel_->setText(QStringLiteral("Preparing donor URL list…"));
        return;
    }
    const int pct = scrapeUrlsTotal_ > 0 ? (scrapeUrlsDone_ * 100) / scrapeUrlsTotal_ : 0;
    scrapeProgressLabel_->setText(
        QStringLiteral("Checked %1 / %2 donor URLs (%3%, %4 with photos) — workers finish out of order, "
                       "but every URL in the list is tried.")
            .arg(scrapeUrlsDone_)
            .arg(scrapeUrlsTotal_)
            .arg(pct)
            .arg(scrapeUrlsFound_));
}

namespace {

QString canonicalGalleryJsonIfPresent(const QString &path)
{
    const QFileInfo fi(path);
    if (!fi.exists() || !fi.isFile())
        return {};
    return fi.canonicalFilePath();
}

} // namespace

QString MainWindow::defaultJsonPath()
{
    static const QString kName = QStringLiteral("gallery_data.json");
    const QString cwd = QDir::currentPath();
    const QString exeDir = QCoreApplication::applicationDirPath();

    // 1) Explicit cwd (terminal runs from project root / scrape repo root / build dir)
    if (QString p = canonicalGalleryJsonIfPresent(QDir(cwd).filePath(kName)); !p.isEmpty())
        return p;
    // 1b) cwd parent — e.g. launch from cpp/build/, JSON sibling at cpp/
    if (QString p =
            canonicalGalleryJsonIfPresent(QDir(cwd).filePath(QStringLiteral("../%1").arg(kName)));
        !p.isEmpty())
        return p;
    // 1c) cwd grandparent — e.g. launch from cpp/build/, JSON at repo root (relative to cwd)
    if (QString p =
            canonicalGalleryJsonIfPresent(QDir(cwd).filePath(QStringLiteral("../../%1").arg(kName)));
        !p.isEmpty())
        return p;
    // 2) Parent of executable dir — usual CMake layout: project/cpp/build/gallery_qt → JSON commonly at
    //    project/cpp/gallery_data.json
    if (QString p = canonicalGalleryJsonIfPresent(QDir(exeDir).filePath(QStringLiteral("../%1").arg(kName)));
        !p.isEmpty())
        return p;
    // 2b) Repo root beside cpp/ — e.g. scraped from .../scrapeff with cwd repo root writes
    //     .../scrapeff/gallery_data.json while this binary lives in .../scrapeff/cpp/build/
    if (QString p =
            canonicalGalleryJsonIfPresent(QDir(exeDir).filePath(QStringLiteral("../../%1").arg(kName)));
        !p.isEmpty())
        return p;
    // 3) Next to the executable
    if (QString p = canonicalGalleryJsonIfPresent(QDir(exeDir).filePath(kName)); !p.isEmpty())
        return p;

    // No file yet: pick the path scrapeff should write (beginDefaultScrape sets cwd to this directory)
    const QString besideProject = QDir(exeDir).filePath(QStringLiteral("../%1").arg(kName));
    const bool exeInBuildDir =
        QFileInfo(exeDir).fileName().compare(QStringLiteral("build"), Qt::CaseInsensitive) == 0;
    if (exeInBuildDir)
        return QFileInfo(besideProject).absoluteFilePath();
    return QFileInfo(QDir(cwd).filePath(kName)).absoluteFilePath();
}

void MainWindow::tickSpinner()
{
    static const char *fr[] = {"\u283f", "\u280b", "\u2819", "\u2832", "\u283c", "\u2834", "\u2826", "\u2817"};
    spinIndex_ = (spinIndex_ + 1) % 8;
    loadingSpinner_->setText(QString::fromUtf8(fr[spinIndex_]));
}

void MainWindow::onOpenFile()
{
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Open gallery data"), QDir::currentPath(),
                                                      QStringLiteral("JSON (*.json);;All (*.*)"));
    if (!path.isEmpty())
        loadFile(path);
}

void MainWindow::clearGalleryBody()
{
    while (vbox_->count() > kFixedHeaderRows) {
        QLayoutItem *it = vbox_->takeAt(kFixedHeaderRows);
        QWidget *w = it ? it->widget() : nullptr;
        delete it;
        if (w) {
            for (QWidget *ch : w->findChildren<QWidget *>(QString(), Qt::FindChildrenRecursively)) {
                if (auto *tl = dynamic_cast<ThumbnailLabel *>(ch))
                    tl->abortPendingLoad();
            }
            w->deleteLater();
        }
    }
}

QVector<GalleryEntry> MainWindow::filteredEntries() const
{
    if (!filterCombo_ || filterCombo_->currentIndex() != 0)
        return entriesRaw_;
    QVector<GalleryEntry> out;
    out.reserve(entriesRaw_.size());
    for (const GalleryEntry &e : entriesRaw_) {
        if (entryHasDonorProfilePic(e))
            out.append(e);
    }
    return out;
}

QVector<GalleryEntry> MainWindow::sortedEntries() const
{
    const QVector<GalleryEntry> filtered = filteredEntries();
    if (sortCombo_->currentIndex() != 0)
        return filtered;
    QVector<QPair<int, GalleryEntry>> tmp;
    tmp.reserve(filtered.size());
    for (int i = 0; i < filtered.size(); ++i)
        tmp.append({i, filtered[i]});
    std::sort(tmp.begin(), tmp.end(), [](const auto &a, const auto &b) {
        const int ap = a.second.profileNum ? *a.second.profileNum : int(1e9);
        const int bp = b.second.profileNum ? *b.second.profileNum : int(1e9);
        if (ap != bp)
            return ap < bp;
        return a.first < b.first;
    });
    QVector<GalleryEntry> out;
    for (const auto &p : tmp)
        out.append(p.second);
    return out;
}

void MainWindow::rebuildGallery()
{
    clearGalleryBody();
    const QVector<GalleryEntry> ent = sortedEntries();
    QString filterHint;
    if (filterCombo_ && filterCombo_->currentIndex() == 0)
        filterHint = QStringLiteral(" · <b>filter:</b> with profile pic");
    QString sortHint;
    if (sortCombo_->currentIndex() == 0)
        sortHint = QStringLiteral(" · <b>sort:</b> donor id");
    const int total = entriesRaw_.size();
    const QString countLine = ent.size() == total
                                  ? QStringLiteral("<b>%1</b> tile(s)").arg(ent.size())
                                  : QStringLiteral("<b>%1</b> tile(s) of <b>%2</b>").arg(ent.size()).arg(total);
    summary_->setText(QStringLiteral("%1 · <code>#main #foto</code> layout · "
                                      "captions: <b>image id</b> / <b>profile</b> · click for full detail%2%3")
                           .arg(countLine)
                           .arg(filterHint)
                           .arg(sortHint));

    auto *mainFr = new QFrame;
    mainFr->setObjectName(QStringLiteral("main"));
    auto *outer = new QVBoxLayout(mainFr);
    outer->setContentsMargins(16, 16, 16, 16);
    auto *fotoFr = new QFrame;
    fotoFr->setObjectName(QStringLiteral("foto"));
    auto *grid = new QGridLayout(fotoFr);
    grid->setContentsMargins(12, 12, 12, 12);
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(10);
    const int cols = 4;
    for (int i = 0; i < ent.size(); ++i) {
        const GalleryEntry &e = ent[i];
        auto *cell = new QWidget;
        auto *cv = new QVBoxLayout(cell);
        cv->setContentsMargins(0, 0, 0, 0);
        cv->setSpacing(4);
        auto *thumb = new ThumbnailLabel(
            e.imageUrl, limiter_, this, e.profileUrl, e.did, e.profileNum,
            [this](const QString &iu, const QString &pu, std::optional<int> d, std::optional<int> pnum) {
                statusBar()->showMessage(pu.isEmpty() ? iu : pu);
                auto *dlg = new ImageMaxDialog(nam_, iu, pu, d, pnum, this);
                dlg->setAttribute(Qt::WA_DeleteOnClose);
                dlg->exec();
                if (!galleryStatus_.isEmpty())
                    statusBar()->showMessage(galleryStatus_);
            });
        cv->addWidget(thumb, 0, Qt::AlignHCenter);
        QStringList cap;
        if (e.did)
            cap.append(QStringLiteral("image id %1").arg(*e.did));
        if (e.profileNum)
            cap.append(QStringLiteral("profile %1").arg(*e.profileNum));
        auto *capL = new QLabel(cap.isEmpty() ? QStringLiteral("—") : cap.join(QStringLiteral(" \u00b7 ")));
        capL->setStyleSheet(QStringLiteral("color: #9cdcfe; font-size: 11px;"));
        capL->setWordWrap(true);
        capL->setAlignment(Qt::AlignCenter);
        capL->setFixedWidth(kThumbPx);
        cv->addWidget(capL);
        grid->addWidget(cell, i / cols, i % cols);
    }
    outer->addWidget(fotoFr);
    vbox_->addWidget(mainFr);
    vbox_->addStretch(1);
}

void MainWindow::onFilterChanged(int)
{
    if (entriesRaw_.isEmpty())
        return;
    rebuildGallery();
    if (!galleryStatus_.isEmpty())
        statusBar()->showMessage(galleryStatus_);
}

void MainWindow::onSortChanged(int)
{
    if (entriesRaw_.isEmpty())
        return;
    rebuildGallery();
    if (!galleryStatus_.isEmpty())
        statusBar()->showMessage(galleryStatus_);
}

bool MainWindow::loadFile(const QString &path, bool silent)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (!silent)
            QMessageBox::warning(this, QStringLiteral("Open failed"), f.errorString());
        return false;
    }
    QJsonParseError pe;
    const QByteArray raw = f.readAll();
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &pe);
    if (pe.error != QJsonParseError::NoError) {
        if (!silent)
            QMessageBox::warning(this, QStringLiteral("Invalid JSON"), pe.errorString());
        return false;
    }
    if (!doc.isObject()) {
        if (!silent)
            QMessageBox::warning(this, QStringLiteral("Invalid format"), QStringLiteral("Root value must be a JSON object."));
        return false;
    }
    const QJsonObject o = doc.object();
    entriesRaw_ = entriesFromGalleryRoot(o);
    if (entriesRaw_.isEmpty()) {
        if (!silent)
            QMessageBox::information(this, QStringLiteral("No images"), QStringLiteral("No image URLs in this file."));
        return false;
    }
    dataPath_ = path;
    blockingFairfaxWarmup(nam_);
    rebuildGallery();
    setWindowTitle(QStringLiteral("Image gallery — %1 (%2 tiles)").arg(QFileInfo(path).fileName()).arg(entriesRaw_.size()));
    galleryStatus_ = path + QStringLiteral(" — ") + QString::number(entriesRaw_.size()) + QStringLiteral(" tiles");
    statusBar()->showMessage(galleryStatus_);
    return true;
}

void MainWindow::beginDefaultScrape(const QString &outputJson)
{
    scrapeTargetJson_ = QFileInfo(outputJson).absoluteFilePath();
    scrapeExitHandled_ = false;
    scrapeUrlsTotal_ = 0;
    scrapeUrlsDone_ = 0;
    scrapeUrlsFound_ = 0;
    if (scrapeProgressBar_) {
        scrapeProgressBar_->setRange(0, 0);
        scrapeProgressBar_->setValue(0);
    }
    updateScrapeProgressLabel();
    scrapeLog_->clear();
    const QString scrapeWorkDir = QFileInfo(scrapeTargetJson_).absolutePath();
    scrapeLog_->append(QStringLiteral("(built-in scraper, working directory: %1)\n\n").arg(scrapeWorkDir));

    stack_->setCurrentWidget(loadingPage_);
    setScrapingBusy(true);
    setWindowTitle(QStringLiteral("Image gallery — scraping…"));

    BuiltinScraperEntry entry = BuiltinScraperEntry::ScrapeffDefault;
    const QString preferReq =
        QString::fromLocal8Bit(qgetenv("SCRAPEFF_GALLERY_PREFER_REQUESTS_CLI")).trimmed();
    if (preferReq == QLatin1String("1")
        || preferReq.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0)
        entry = BuiltinScraperEntry::PythonRequestsCompatible;

    auto *thread = new GalleryScrapeThread;
    thread->workDir = scrapeWorkDir;
    thread->entry = entry;
    thread->log = [this](const QString &line) {
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty())
            return;
        QTimer::singleShot(0, this, [this, trimmed] {
            static const QRegularExpression kFetchTotal(
                QStringLiteral("Fetching and extracting images from (\\d+) URLs"));
            const QRegularExpressionMatch totalMatch = kFetchTotal.match(trimmed);
            if (!trimmed.startsWith(QLatin1String("END\t"))) {
                scrapeLog_->append(trimmed);
                if (totalMatch.hasMatch()) {
                    scrapeUrlsTotal_ = totalMatch.captured(1).toInt();
                    scrapeUrlsDone_ = 0;
                    scrapeUrlsFound_ = 0;
                    if (scrapeProgressBar_) {
                        scrapeProgressBar_->setRange(0, qMax(1, scrapeUrlsTotal_));
                        scrapeProgressBar_->setValue(0);
                    }
                    updateScrapeProgressLabel();
                }
                return;
            }

            ++scrapeUrlsDone_;
            if (trimmed.contains(QLatin1String("state=ok_gallery")))
                ++scrapeUrlsFound_;
            if (scrapeProgressBar_ && scrapeUrlsTotal_ > 0)
                scrapeProgressBar_->setValue(qMin(scrapeUrlsDone_, scrapeUrlsTotal_));

            const QString url = trimmed.section(QLatin1Char('\t'), 1, 1);
            QString donorLabel;
            if (const std::optional<int> pn = scrapeff::donorNumberFromProfileUrl(url))
                donorLabel = QString::number(*pn);
            else
                donorLabel = url;
            const QString detail = trimmed.section(QLatin1Char('\t'), 2).trimmed();
            scrapeLog_->append(QStringLiteral("[%1/%2] donor %3 — %4")
                                   .arg(scrapeUrlsDone_)
                                   .arg(scrapeUrlsTotal_ > 0 ? QString::number(scrapeUrlsTotal_)
                                                             : QStringLiteral("?"))
                                   .arg(donorLabel)
                                   .arg(detail));
            updateScrapeProgressLabel();
        });
    };
    scrapeThread_ = thread;
    connect(thread, &QThread::finished, this, [this, thread] {
        onBuiltinScrapeFinished(thread->resultCode);
        scrapeThread_.clear();
        thread->deleteLater();
    });
    thread->start();
}

void MainWindow::onBuiltinScrapeFinished(int exitCode)
{
    if (scrapeExitHandled_)
        return;
    scrapeExitHandled_ = true;
    setScrapingBusy(false);
    stack_->setCurrentWidget(galleryScroll_);
    setWindowTitle(QStringLiteral("Image gallery"));
    const QString target = scrapeTargetJson_;
    scrapeTargetJson_.clear();

    if (exitCode != 0) {
        scrapeLog_->append(QStringLiteral("\n[scraper exited with code %1]\n").arg(exitCode));
        QMessageBox::warning(this, QStringLiteral("Scraper failed"),
                             QStringLiteral("Built-in scraper exited with code %1. See log above.").arg(exitCode));
        statusBar()->showMessage(QStringLiteral("Scraper failed — open JSON manually or fix errors and retry."));
        return;
    }
    if (!target.isEmpty() && QFile::exists(target)) {
        if (loadFile(target, true)) {
            statusBar()->showMessage(QStringLiteral("Loaded %1 after scrape.").arg(QFileInfo(target).fileName()));
            return;
        }
        QMessageBox::information(this, QStringLiteral("No results"),
                                 QStringLiteral("Scraper finished but found no images. Check target_urls.txt and try again."));
        statusBar()->showMessage(QStringLiteral("Scraper finished — no images found"));
        return;
    }
    QMessageBox::warning(this, QStringLiteral("No gallery file"),
                         QStringLiteral("Scraper finished but %1 was not found.").arg(target));
    statusBar()->showMessage(QStringLiteral("Scraper finished — no gallery_data.json"));
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (scrapeThread_ && scrapeThread_->isRunning())
        scrapeThread_->wait();
    setScrapingBusy(false);
    QMainWindow::closeEvent(event);
}
