#include <QApplication>
#include <QFileInfo>
#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QSvgRenderer>

#include "app/app_context.h"
#include "forma/build/setup.h"
#include "forma/resources/resources.h"
#include "shell/main_window.h"
#include "ui/theme.h"

static QIcon applicationIcon()
{
    auto data = forma::resources::find("branding/icon.svg");
    if (!data)
        return {};
    QSvgRenderer renderer(QByteArray(data->data(), static_cast<qsizetype>(data->size())));
    QIcon result;
    for (int size : {16, 24, 32, 48, 64, 128, 256}) {
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        renderer.render(&painter);
        painter.end();
        result.addPixmap(pixmap);
    }
    return result;
}

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    QApplication::setApplicationName(QStringLiteral("FormaIDE"));
    QApplication::setOrganizationName(QStringLiteral("Forma"));
    QApplication::setApplicationVersion(QStringLiteral(FORMA_VERSION));
    QApplication::setWindowIcon(applicationIcon());

    forma::build::refreshEnvironmentFromSystem();
    auto& context = forma::ui::AppContext::instance();
    auto& theme = forma::ui::Theme::instance();
    theme.setMode(forma::ui::toQ(context.settings().getString("appearance.theme", "system")));
    theme.apply(application);

    forma::ui::MainWindow window;
    window.show();
    QStringList arguments = QApplication::arguments();
    if (arguments.size() > 1 && QFileInfo(arguments.at(1)).isDir())
        window.openProjectFolder(arguments.at(1));
    return QApplication::exec();
}
