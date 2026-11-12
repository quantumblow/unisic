#include "Barcode.h"

#include <ZXing/ReadBarcode.h>

// DecodeHints was renamed to ReaderOptions in zxing-cpp 2.2; the version
// header itself moved names across releases, so probe the header directly.
#if __has_include(<ZXing/ReaderOptions.h>)
using ZXingOptions = ZXing::ReaderOptions;
#else
using ZXingOptions = ZXing::DecodeHints;
#endif

// Result::text() returns std::string in zxing-cpp 2.x but std::wstring in
// 1.x (still what openSUSE Leap 15.6 ships) - overloads pick the right
// conversion at compile time.
// maybe_unused: exactly ONE of the two is called per zxing-cpp version, so the
// other is always dead and would warn on every build.
[[maybe_unused]] static QString zxingText(const std::string &s) { return QString::fromStdString(s); }
[[maybe_unused]] static QString zxingText(const std::wstring &s) { return QString::fromStdWString(s); }

QString scanBarcode(const QImage &img)
{
    if (img.isNull())
        return {};
    // Grayscale keeps the ImageView format portable across zxing-cpp versions.
    const QImage gray = img.convertToFormat(QImage::Format_Grayscale8);
    ZXing::ImageView view(gray.constBits(), gray.width(), gray.height(),
                          ZXing::ImageFormat::Lum, int(gray.bytesPerLine()));
    ZXingOptions opts;
    opts.setTryHarder(true);
    opts.setTryRotate(true);
    const auto res = ZXing::ReadBarcode(view, opts);
    if (!res.isValid())
        return {};
    return zxingText(res.text()).trimmed();
}
