#include <QByteArray>
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QSvgRenderer>
#include <cstdio>
#include <string>
#include <vector>

#include "forma/assets/asset_manager.h"

static QByteArray pngOf(QSvgRenderer& renderer, int size)
{
    QImage image(size, size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    renderer.render(&painter);
    painter.end();
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

static void writeFile(const QString& path, const QByteArray& data)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size())
        std::fprintf(stderr, "cannot write %s\n", qPrintable(path));
}

static void appendBigEndian(QByteArray& out, quint32 value)
{
    out.append(static_cast<char>((value >> 24) & 255));
    out.append(static_cast<char>((value >> 16) & 255));
    out.append(static_cast<char>((value >> 8) & 255));
    out.append(static_cast<char>(value & 255));
}

static QImage wideImage(const QString& svgPath, int width, int height, const QColor& background)
{
    QSvgRenderer renderer(svgPath);
    QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
    image.fill(background);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    QSize natural = renderer.defaultSize();
    int logoWidth = width * 6 / 10;
    int logoHeight = natural.height() * logoWidth / natural.width();
    renderer.render(&painter, QRectF((width - logoWidth) / 2.0, (height - logoHeight) / 2.0, logoWidth, logoHeight));
    return image;
}

int main(int argc, char** argv)
{
    QGuiApplication application(argc, argv);
    if (argc < 3) {
        std::fprintf(stderr, "usage: forma_iconbake <branding dir> <packaging dir>\n");
        return 2;
    }
    QString branding = QString::fromLocal8Bit(argv[1]);
    QDir packaging(QString::fromLocal8Bit(argv[2]));
    packaging.mkpath(QStringLiteral("windows"));
    packaging.mkpath(QStringLiteral("macos"));
    packaging.mkpath(QStringLiteral("linux"));
    QSvgRenderer renderer(QDir(branding).filePath(QStringLiteral("icon.svg")));
    if (!renderer.isValid()) {
        std::fprintf(stderr, "cannot read the icon\n");
        return 1;
    }
    std::vector<std::pair<int, std::string>> icoImages;
    for (int size : {16, 24, 32, 48, 64, 128, 256})
        icoImages.emplace_back(size, pngOf(renderer, size).toStdString());
    std::string ico = forma::assets::buildIco(icoImages);
    writeFile(packaging.filePath(QStringLiteral("windows/formaide.ico")), QByteArray(ico.data(), static_cast<qsizetype>(ico.size())));
    writeFile(packaging.filePath(QStringLiteral("windows/formaide.rc")), QByteArrayLiteral("IDI_ICON1 ICON \"formaide.ico\"\n"));
    QByteArray body;
    const std::pair<const char*, int> entries[] = {{"ic07", 128}, {"ic08", 256}, {"ic09", 512}, {"ic10", 1024}};
    for (const auto& [type, size] : entries) {
        QByteArray png = pngOf(renderer, size);
        body.append(type, 4);
        appendBigEndian(body, static_cast<quint32>(png.size() + 8));
        body.append(png);
    }
    QByteArray icns("icns", 4);
    appendBigEndian(icns, static_cast<quint32>(body.size() + 8));
    icns.append(body);
    writeFile(packaging.filePath(QStringLiteral("macos/FormaIDE.icns")), icns);
    writeFile(packaging.filePath(QStringLiteral("linux/formaide.png")), pngOf(renderer, 512));
    wideImage(QDir(branding).filePath(QStringLiteral("logo-light.svg")), 1280, 640, QColor(QStringLiteral("#F6F7F9"))).save(QDir(branding).filePath(QStringLiteral("social-preview.png")));
    return 0;
}
