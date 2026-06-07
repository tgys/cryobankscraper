#pragma once

#include <QMainWindow>
#include <QPointer>
#include <QThread>
#include <QMap>
#include <QTimer>
#include <QVector>
#include <optional>
#include <QString>

class QAction;
class QNetworkAccessManager;
class QStackedWidget;
class QScrollArea;
class QWidget;
class QVBoxLayout;
class QLabel;
class QComboBox;
class QTextEdit;
class QProgressBar;
class ThumbnailLoadLimiter;
class QJsonObject;

struct GalleryEntry {
    QString imageUrl;
    QString profileUrl;
    std::optional<int> did;
    std::optional<int> profileNum;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    static QString defaultJsonPath();
    bool loadFile(const QString &path, bool silent = false);
    void beginDefaultScrape(const QString &outputJson);

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onOpenFile();
    void onFilterChanged(int index);
    void onSortChanged(int index);
    void onBuiltinScrapeFinished(int exitCode);
    void tickSpinner();

private:
    void rebuildGallery();
    void clearGalleryBody();
    QVector<GalleryEntry> filteredEntries() const;
    QVector<GalleryEntry> sortedEntries() const;
    void setScrapingBusy(bool busy);
    void updateScrapeProgressLabel();

    QVector<GalleryEntry> entriesRaw_;
    QString dataPath_;
    QString galleryStatus_;

    QNetworkAccessManager *nam_ = nullptr;
    ThumbnailLoadLimiter *limiter_ = nullptr;

    QStackedWidget *stack_ = nullptr;
    QWidget *loadingPage_ = nullptr;
    QTextEdit *scrapeLog_ = nullptr;
    QLabel *scrapeProgressLabel_ = nullptr;
    QProgressBar *scrapeProgressBar_ = nullptr;
    QLabel *loadingSpinner_ = nullptr;
    int scrapeUrlsTotal_ = 0;
    int scrapeUrlsDone_ = 0;
    int scrapeUrlsFound_ = 0;
    QScrollArea *galleryScroll_ = nullptr;
    QWidget *root_ = nullptr;
    QVBoxLayout *vbox_ = nullptr;
    QLabel *summary_ = nullptr;
    QWidget *filterRow_ = nullptr;
    QComboBox *filterCombo_ = nullptr;
    QWidget *sortRow_ = nullptr;
    QComboBox *sortCombo_ = nullptr;
    QAction *openJsonAction_ = nullptr;

    QPointer<QThread> scrapeThread_;
    QString scrapeTargetJson_;
    bool scrapeExitHandled_ = false;

    QTimer spinTimer_;
    int spinIndex_ = 0;
    static constexpr int kFixedHeaderRows = 3;
};
