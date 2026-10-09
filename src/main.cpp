#include <iostream>
#include <QStringList>
#include <QDebug>

#if defined(BB_CASCADES) || defined(Q_OS_BLACKBERRY) || defined(__QNX__)
#include <bb/cascades/Application>
#include <bb/cascades/QmlDocument>
#include <bb/cascades/AbstractPane>
#include <QDeclarativeError>
#define CASCADES_ENABLED 1
#else
#include <QCoreApplication>
#endif

#include "app/Application.h"
#include "ui/UiController.h"

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

    std::cout << "LocalSend for BlackBerry 10 starting..." << std::endl;

    localsend::Application coreApp;
    QString err;
    if (!coreApp.start(&err)) {
        std::cerr << "Failed to start LocalSend core engine: " << err.toStdString() << std::endl;
        return 1;
    }

    localsend::UiController controller(&coreApp);

#if defined(CASCADES_ENABLED)
    bb::cascades::QmlDocument *qml = bb::cascades::QmlDocument::create("asset:///main.qml").parent(&app);
    if (!qml) {
        std::cerr << "Failed to load asset:///main.qml" << std::endl;
        return 1;
    }

    qml->setContextProperty("app", &controller);

    bb::cascades::AbstractPane *root = qml->createRootObject<bb::cascades::AbstractPane>();
    if (!root) {
        std::cerr << "Failed to create root pane from main.qml" << std::endl;
        if (qml->hasErrors()) {
            QList<QDeclarativeError> errs = qml->errors();
            for (int i = 0; i < errs.size(); ++i) {
                std::cerr << "QML Error: " << errs.at(i).toString().toStdString() << std::endl;
            }
        }
        return 1;
    }

    app.setScene(root);
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
