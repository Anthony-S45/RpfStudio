#include "MainWindow.h"
#include <QStyle>
#include <QApplication>
#include <QSurfaceFormat>

int main(int argc, char** argv)
{
    QSurfaceFormat fmt;
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setDepthBufferSize(24);
    QSurfaceFormat::setDefaultFormat(fmt);
    QApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QApplication app(argc, argv);
    QApplication::setApplicationName("rpfstudio");
    QApplication::setApplicationDisplayName("RpfStudio");
    QApplication::setOrganizationName("RpfStudio");
    QApplication::setApplicationVersion("0.1.0");
    MainWindow w;
    w.show();
    if (argc > 1) w.openPath(QString::fromLocal8Bit(argv[1]));
    return app.exec();
}
