#include <iostream>
#include <QStringList>
#include <QDebug>
#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QDir>

#if defined(BB_CASCADES) || defined(Q_OS_BLACKBERRY) || defined(__QNX__)
#include <bb/cascades/Application>
#include <bb/cascades/QmlDocument>
#include <bb/cascades/AbstractPane>
#include <bb/cascades/Container>
#include <QDeclarativeError>
#define CASCADES_ENABLED 1
#else
#include <QCoreApplication>
#endif

#include "app/Application.h"
#include "ui/UiController.h"

static QString g_logFilePath;

static void writeLog(const QString &msg)
{
    std::cout << msg.toStdString() << std::endl;
    if (!g_logFilePath.isEmpty()) {
        QFile file(g_logFilePath);
        if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            QTextStream out(&file);
            out << QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz ")
                << msg << "\n";
            file.flush();
        }
    }
}

#if QT_VERSION < QT_VERSION_CHECK(5, 0, 0)
static void qtLogHandler(QtMsgType type, const char *msg)
{
    QString prefix;
    switch (type) {
    case QtDebugMsg:    prefix = "[DEBUG] "; break;
    case QtWarningMsg:  prefix = "[WARN] "; break;
    case QtCriticalMsg: prefix = "[CRIT] "; break;
    case QtFatalMsg:    prefix = "[FATAL] "; break;
    default:            prefix = "[LOG] "; break;
    }
    writeLog(prefix + QString::fromUtf8(msg));
}
#endif

#if defined(CASCADES_ENABLED)
Q_DECL_EXPORT int main(int argc, char **argv)
#else
int main(int argc, char **argv)
#endif
{
#if defined(CASCADES_ENABLED)
    bb::cascades::Application app(argc, argv);
#else
    QCoreApplication app(argc, argv);
#endif

#if defined(CASCADES_ENABLED)
    QString docDir = "/accounts/1000/shared/documents";
    if (QDir(docDir).exists()) {
        g_logFilePath = docDir + "/localsend_launch.txt";
    } else {
        QDir("data").mkpath(".");
        g_logFilePath = "data/localsend_launch.txt";
    }
#else
    g_logFilePath = "localsend_launch.txt";
#endif

#if QT_VERSION < QT_VERSION_CHECK(5, 0, 0)
    qInstallMsgHandler(qtLogHandler);
#endif

    writeLog("==========================================");
    writeLog("LocalSend BB10 Starting...");
    writeLog(QString("Timestamp: %1").arg(QDateTime::currentDateTime().toString(Qt::ISODate)));

    localsend::Application coreApp;
    QString err;
    if (!coreApp.start(&err)) {
        writeLog(QString("FATAL: Failed to start LocalSend core engine: %1").arg(err));
        return 1;
    }
    writeLog(QString("Core engine started successfully. Port: %1, Directory: %2")
             .arg(coreApp.localInfo().port)
             .arg(coreApp.receiveDirectory()));

    localsend::UiController controller(&coreApp);

#if defined(CASCADES_ENABLED)
    writeLog("Loading asset:///main.qml...");
    bb::cascades::QmlDocument *qml = bb::cascades::QmlDocument::create("asset:///main.qml").parent(&app);
    if (!qml) {
        writeLog("FATAL: Failed to load asset:///main.qml");
        return 1;
    }

    qml->setContextProperty("app", &controller);

    writeLog("Creating root pane from main.qml...");
    bb::cascades::AbstractPane *root = qml->createRootObject<bb::cascades::AbstractPane>();
    if (!root) {
        writeLog("FATAL: Failed to create root pane from main.qml");
        if (qml->hasErrors()) {
            QList<QDeclarativeError> errs = qml->errors();
            for (int i = 0; i < errs.size(); ++i) {
                writeLog(QString("QML Error [%1]: %2").arg(i).arg(errs.at(i).toString()));
            }
        }
        return 1;
    }
    writeLog("Root pane created successfully!");

    bb::cascades::Container *peersContainer = root->findChild<bb::cascades::Container*>("peersContainer");
    if (peersContainer) {
        writeLog("Found peersContainer. Connecting to UiController.");
        controller.setPeersContainer(peersContainer);
    } else {
        writeLog("WARN: peersContainer not found in root pane!");
    }

    app.setScene(root);
    writeLog("Scene set. Starting application event loop...");
    return bb::cascades::Application::exec();
#else
    // Host / Desktop CLI support
    QStringList args = app.arguments();
    for (int i = 1; i < args.size(); ++i) {
        if (args.at(i) == "--send" && i + 2 < args.size()) {
            QString file = args.at(i + 1);
            QString target = args.at(i + 2);
            i += 2;
            std::cout << "Command-line send requested: " << file.toStdString() << " -> " << target.toStdString() << std::endl;
            controller.setSelectedFilePath(file);
            QStringList parts = target.split(":");
            if (parts.size() == 2) {
                controller.addManualPeer(parts.at(0), parts.at(1).toInt());
            }
        }
    }

    std::cout << "LocalSend running. Device: " << coreApp.localInfo().alias.toStdString()
              << " on port: " << coreApp.localInfo().port << std::endl;
    return app.exec();
#endif
}
