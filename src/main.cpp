#include <iostream>
#include <QStringList>
#include <QDebug>

#if defined(BB_CASCADES) || defined(qnx) || defined(Q_OS_QNX)
#include <bb/cascades/Application>
#include <bb/cascades/QmlDocument>
#include <bb/cascades/AbstractPane>
#else
#include <QCoreApplication>
#endif

#include "app/Application.h"
#include "ui/UiController.h"

int main(int argc, char **argv)
{
#if defined(BB_CASCADES) || defined(qnx) || defined(Q_OS_QNX)
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

#if defined(BB_CASCADES) || defined(qnx) || defined(Q_OS_QNX)
    bb::cascades::QmlDocument *qml = bb::cascades::QmlDocument::create("asset:///main.qml").parent(&app);
    if (!qml) {
        std::cerr << "Failed to load asset:///main.qml" << std::endl;
        return 1;
    }

    qml->setContextProperty("app", &controller);

    bb::cascades::AbstractPane *root = qml->createRootObject<bb::cascades::AbstractPane>();
    if (!root) {
        std::cerr << "Failed to create root pane from main.qml" << std::endl;
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
