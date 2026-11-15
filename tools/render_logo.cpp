#include <QtCore/QCoreApplication>
#include <QtCore/QTextStream>
#include <QtGui/QImage>
#include <QtGui/QPainter>
#include <QtSvg/QSvgRenderer>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() != 4) {
        QTextStream(stderr) << "Usage: render_logo source.svg output.png size\n";
        return 2;
    }
    bool ok = false;
    const int size = args.at(3).toInt(&ok);
    if (!ok || size < 16 || size > 2048) return 2;
    QSvgRenderer svg(args.at(1));
    if (!svg.isValid()) return 1;
    QImage image(size, size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    { QPainter painter(&image); svg.render(&painter); }
    return image.save(args.at(2)) ? 0 : 1;
}
