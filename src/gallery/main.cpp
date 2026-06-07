#include "mainwindow.h"

#include "ssl_utils.h"

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QMessageBox>
#include <QPalette>
#include <QStatusBar>

static void applyDark(QApplication &app)
{
    app.setStyleSheet(QStringLiteral(
        "QWidget{background:#1e1e1e;color:#e0e0e0;}QMainWindow{background:#1a1a1a;}"));
    QPalette pal;
    pal.setColor(QPalette::Window, QColor(QStringLiteral("#1e1e1e")));
    pal.setColor(QPalette::WindowText, QColor(QStringLiteral("#e0e0e0")));
    pal.setColor(QPalette::Base, QColor(QStringLiteral("#1a1a1a")));
    pal.setColor(QPalette::Text, QColor(QStringLiteral("#e0e0e0")));
    pal.setColor(QPalette::Button, QColor(QStringLiteral("#3c3c3c")));
    pal.setColor(QPalette::ButtonText, Qt::white);
    pal.setColor(QPalette::Highlight, QColor(QStringLiteral("#094771")));
    pal.setColor(QPalette::HighlightedText, Qt::white);
    pal.setColor(QPalette::Link, QColor(QStringLiteral("#6cf")));
    app.setPalette(pal);
}

static void syncCertEnv()
{
    const QByteArray nix = qgetenv("NIX_SSL_CERT_FILE");
    const QByteArray ssl = qgetenv("SSL_CERT_FILE");
    if (!nix.isEmpty() && ssl.isEmpty())
        qputenv("SSL_CERT_FILE", nix);
    if (!ssl.isEmpty() && qgetenv("NIX_SSL_CERT_FILE").isEmpty())
        qputenv("NIX_SSL_CERT_FILE", ssl);
}

int main(int argc, char *argv[])
{
    syncCertEnv();
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    QApplication app(argc, argv);
    scrapeff::applySslFromEnv();
    applyDark(app);
    QLoggingCategory::setFilterRules(
        QStringLiteral("qt.network.http2.debug=false;qt.network.http2.info=false;qt.network.http2.warning=false"));

    MainWindow w;
    w.show();

    const QStringList args = QCoreApplication::arguments();
    if (args.size() > 1) {
        const QString p = QFileInfo(args.at(1)).absoluteFilePath();
        if (QFile::exists(p))
            w.loadFile(p);
        else {
            w.statusBar()->showMessage(QStringLiteral("No such file: %1").arg(p));
            QMessageBox::information(nullptr, QStringLiteral("File not found"), p);
        }
    } else if (QFile::exists(MainWindow::defaultJsonPath())) {
        w.loadFile(MainWindow::defaultJsonPath());
    } else {
        w.beginDefaultScrape(MainWindow::defaultJsonPath());
    }
    return QCoreApplication::exec();
}
