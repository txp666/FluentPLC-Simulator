#include "core/address.h"
#include "core/i18n.h"

#include <QtCore/QRegularExpression>

namespace fplc {
namespace {
bool fail(QString *error, const QString &message)
{
    if (error) *error = message;
    return false;
}

MemoryArea areaOf(QChar letter)
{
    if (letter == QLatin1Char('M')) return MemoryArea::Marker;
    if (letter == QLatin1Char('I')) return MemoryArea::Input;
    return MemoryArea::Output;
}

int width(QChar letter)
{
    if (letter == QLatin1Char('W')) return 2;
    if (letter == QLatin1Char('D')) return 4;
    return 1;
}
} // namespace

QString Address::areaKey() const
{
    switch (area) {
    case MemoryArea::DataBlock: return QStringLiteral("DB%1").arg(dbNumber);
    case MemoryArea::Marker: return QStringLiteral("M");
    case MemoryArea::Input: return QStringLiteral("I");
    case MemoryArea::Output: return QStringLiteral("Q");
    }
    return {};
}

bool parseAddress(const QString &text, Address *address, QString *error)
{
    const QString source = text.trimmed().toUpper();
    static const QRegularExpression dbBit(QStringLiteral("^DB([0-9]+)\\.DBX([0-9]+)\\.([0-7])$"));
    static const QRegularExpression dbWord(QStringLiteral("^DB([0-9]+)\\.DB([BWD])([0-9]+)$"));
    static const QRegularExpression areaBit(QStringLiteral("^([MIQ])X?([0-9]+)\\.([0-7])$"));
    static const QRegularExpression areaWord(QStringLiteral("^([MIQ])([BWD])([0-9]+)$"));

    Address out;
    QString offset;
    QString dbNumber;
    auto match = dbBit.match(source);
    if (match.hasMatch()) {
        dbNumber = match.captured(1);
        offset = match.captured(2);
        out.bit = match.captured(3).toInt();
    } else if ((match = dbWord.match(source)).hasMatch()) {
        dbNumber = match.captured(1);
        out.byteCount = width(match.captured(2).at(0));
        offset = match.captured(3);
    } else if ((match = areaBit.match(source)).hasMatch()) {
        out.area = areaOf(match.captured(1).at(0));
        offset = match.captured(2);
        out.bit = match.captured(3).toInt();
    } else if ((match = areaWord.match(source)).hasMatch()) {
        out.area = areaOf(match.captured(1).at(0));
        out.byteCount = width(match.captured(2).at(0));
        offset = match.captured(3);
    } else {
        return fail(error, I18n::tr("Invalid S7 address: %1 (examples: DB1.DBX0.0, DB1.DBD4, M0.0, QW2)").arg(text));
    }
    bool ok = false;
    const qulonglong byte = offset.toULongLong(&ok);
    // S7ANY carries a 24-bit bit address. The registered memory size is checked
    // separately by Project, so this parser is also usable for client fixtures.
    if (!ok || byte > 2097151ULL || byte + qulonglong(out.byteCount) > 2097152ULL)
        return fail(error, I18n::tr("S7 byte offset is out of range: %1").arg(text));
    out.byteOffset = int(byte);
    if (out.area == MemoryArea::DataBlock) {
        const qulonglong number = dbNumber.toULongLong(&ok);
        if (!ok || number < 1 || number > 65535)
            return fail(error, I18n::tr("DB number must be between 1 and 65535: %1").arg(text));
        out.dbNumber = int(number);
    }
    const QString prefix = out.area == MemoryArea::DataBlock
                               ? QStringLiteral("DB%1.DB").arg(out.dbNumber) : out.areaKey();
    if (out.bit >= 0) {
        out.canonical = prefix + (out.area == MemoryArea::DataBlock ? QStringLiteral("X") : QString())
                        + QString::number(out.byteOffset) + QLatin1Char('.') + QString::number(out.bit);
    } else {
        const QChar letter = out.byteCount == 4 ? QLatin1Char('D')
                                               : (out.byteCount == 2 ? QLatin1Char('W') : QLatin1Char('B'));
        out.canonical = prefix + letter + QString::number(out.byteOffset);
    }
    if (address) *address = out;
    if (error) error->clear();
    return true;
}
} // namespace fplc
