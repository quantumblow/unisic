#pragma once
#include <QImage>
#include <QString>

// The first QR/bar code payload found in `img`, or an empty string when the
// picture holds no readable code. zxing-cpp is a hard build requirement, so
// this is always compiled in. Runs on the calling thread: an OCR worker calls
// it before text recognition so a code's pixels are never OCR-ed into garbage.
QString scanBarcode(const QImage &img);
