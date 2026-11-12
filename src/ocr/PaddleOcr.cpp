#include "PaddleOcr.h"

#include "Barcode.h"
#include "Settings.h"

#include <onnxruntime/onnxruntime_c_api.h>

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QLibrary>
#include <QLibraryInfo>
#include <QLocale>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QProcess>
#include <QSaveFile>
#include <QSet>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QSysInfo>
#include <QThread>
#include <QTimer>
#include <QVersionNumber>
#include <QtConcurrent>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <utility>
#include <vector>

namespace {

// ---------------------------------------------------------------- pins

// PP-OCRv6's det + rec pair + dictionary per tier. The files are the official
// PaddleOCR models converted to ONNX (Apache-2.0); they are pinned by URL,
// size and SHA-256 so a mirror can never change under a user. The default is
// "small": 31 MB, reads Chinese (both scripts), Japanese and 48 Latin-script
// languages, about 1 s per page on a 4-core CPU.
struct FilePin {
    const char *name;
    qint64 bytes;
    const char *sha256;
};
struct TierPin {
    const char *id;
    FilePin files[3]; // det, rec, dictionary
};
const TierPin kTiers[] = {
    {"tiny",
     {{"PP-OCRv6_det_tiny.onnx", 1829618,
       "f42c0fbd294d95eac1a550e131b277dac97462c8025fa4b6c3cec1b7894bd3d5"},
      {"PP-OCRv6_rec_tiny.onnx", 4489813,
       "e16e242de5937ad92609223f19bc2aff3727ee40b095f996907c24749bad251b"},
      {"ppocrv6_tiny_dict.txt", 27156,
       "c5cbe34ef40c29c4df07ed012bf96569cb69a2d2a01a07027e9f13cb832bd9cd"}}},
    {"small",
     {{"PP-OCRv6_det_small.onnx", 9929594,
       "090f04abcd9d9a7498bc4ebf677e4cb9bdce1fe4197ddb7e529f1ef44e1ff94f"},
      {"PP-OCRv6_rec_small.onnx", 21234383,
       "6f327246b50388f3c176ae304bd95767ea6dc0c9ae92153ef8cbe210b3c14884"},
      {"ppocrv6_dict.txt", 74947,
       "b5f2bfe2bdd9448429e3e82b51c789775d9b42f2403d082b00662eb77e401c5d"}}},
    {"medium",
     {{"PP-OCRv6_det_medium.onnx", 62119454,
       "92078b7355007ccfffcd4c8cd441a3afd4538904d06881b29a155e1e679907c2"},
      {"PP-OCRv6_rec_medium.onnx", 76629984,
       "eef444829dbbe18d7fea59a3f6eb75647518d2b3a9568d27c92e42940204894b"},
      {"ppocrv6_dict.txt", 74947,
       "b5f2bfe2bdd9448429e3e82b51c789775d9b42f2403d082b00662eb77e401c5d"}}},
};
const TierPin *tierPin(const QString &id)
{
    for (const TierPin &t : kTiers)
        if (id == QLatin1String(t.id))
            return &t;
    return &kTiers[1]; // unknown/corrupt setting -> the default
}
QString tierId(const TierPin *pin)
{
    return QString::fromLatin1(pin->id);
}

// The models are served from ModelScope, the same pinned copies Snow Shot
// publishes. Swapping the mirror later means changing this base, the sizes and
// the hashes together - never one of them.
QString modelsBase()
{
    return QStringLiteral("https://www.modelscope.cn/models/mgchao/SnowShotOCR/resolve/master/PP-OCRv6/");
}

// ONNX Runtime: the same pinned release and destination "Remove background"
// uses, so one download serves both features.
constexpr const char *kRuntimeUrl =
    "https://github.com/microsoft/onnxruntime/releases/download/v1.31.1/"
    "onnxruntime-linux-x64-1.31.1.tgz";
constexpr qint64 kRuntimeBytes = 11413655;
constexpr const char *kRuntimeSha =
    "c849e36a1ef3e22d4ff217b9d1ce4a3cf5b9467b975610913ae959ad5b16bdc9";
constexpr const char *kRuntimeMember =
    "onnxruntime-linux-x64-1.31.1/lib/libonnxruntime.so.1.31.1";
constexpr const char *kRuntimeFile = "libonnxruntime.so.1.31.1";
// PP-OCRv6 is an opset 11 model: every ORT release newer than the vendored
// header runs it. Background removal needs 1.25, but OCR alone does not.
const QVersionNumber kMinRuntime(1, 17);

QString appDataDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
}
QString runtimeDir()
{
    return appDataDir() + QStringLiteral("/onnxruntime");
}
QString modelRoot()
{
    return appDataDir() + QStringLiteral("/paddleocr");
}

// ---------------------------------------------------------------- ORT glue

bool ortOk(OrtStatus *status, const OrtApi *api, QString *error)
{
    if (!status)
        return true;
    if (error)
        *error = QString::fromUtf8(api->GetErrorMessage(status));
    api->ReleaseStatus(status);
    return false;
}

// The PP-OCRv6 normalisation from PaddleOCR's own pipeline: BGR planes with
// the ImageNet mean/std for detection, and (x/255 - 0.5) / 0.5 for
// recognition. QImage is RGB, so the B plane is the third byte.
constexpr float kDetMean[3] = {0.485f, 0.456f, 0.406f};
constexpr float kDetStd[3] = {0.229f, 0.224f, 0.225f};

struct DetBox {
    QRectF rect;
    float score = 0.f;
    int line = 0;
};
struct LineOut {
    QString text;
    QVector<OcrWord> words;
    float confidence = 0.f;
};
struct RunOut {
    QString text;
    QVector<OcrWord> words;
    QString error;
};

// PP-OCRv6 expects ink on a light page. A transparent editor result (background
// removal) otherwise detects on whatever the transparent pixels carry, so
// composite it onto white first. Opaque screenshots are returned as-is (QImage
// is implicitly shared), which is the normal path.
QImage flattenOnWhite(const QImage &img)
{
    if (!img.hasAlphaChannel())
        return img;
    QImage out(img.size(), QImage::Format_RGB32);
    out.fill(Qt::white);
    QPainter p(&out);
    p.drawImage(0, 0, img);
    return out;
}

// Connected components over the det probability map. DB's output already
// groups characters into text lines, so a component is a line; its bounding
// box plus DB's unclip expansion is the box PaddleOCR itself would return.
// 8-connectivity keeps a line whose anti-aliased pixels only touch diagonally
// in one piece.
std::vector<DetBox> components(const float *prob, int w, int h, const QImage &src, float scaleX,
                               float scaleY)
{
    std::vector<DetBox> boxes;
    std::vector<int> label(size_t(w) * h, -1);
    std::vector<int> stack;
    stack.reserve(size_t(w) * h / 8);
    constexpr int dx[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
    constexpr int dy[8] = {-1, -1, -1, 0, 0, 1, 1, 1};
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int start = y * w + x;
            if (prob[start] <= 0.3f || label[start] >= 0)
                continue;
            label[start] = start;
            stack.clear();
            stack.push_back(start);
            int minX = x, maxX = x, minY = y, maxY = y, count = 0;
            double sum = 0;
            while (!stack.empty()) {
                const int p = stack.back();
                stack.pop_back();
                const int px = p % w, py = p / w;
                ++count;
                sum += prob[p];
                minX = std::min(minX, px);
                maxX = std::max(maxX, px);
                minY = std::min(minY, py);
                maxY = std::max(maxY, py);
                for (int k = 0; k < 8; ++k) {
                    const int nx = px + dx[k], ny = py + dy[k];
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h)
                        continue;
                    const int n = ny * w + nx;
                    if (prob[n] <= 0.3f || label[n] >= 0)
                        continue;
                    label[n] = start;
                    stack.push_back(n);
                }
            }
            if (count < 6)
                continue;
            const int bw = maxX - minX + 1, bh = maxY - minY + 1;
            if (bw < 3 || bh < 3)
                continue;
            const float score = float(sum / count);
            // PaddleOCR's det_db_box_thresh. Below this a component is usually
            // an icon edge or a compression artifact, not a line of text.
            if (score < 0.5f)
                continue;
            // DB unclip: move each edge out by area * ratio / perimeter.
            const float d = float(bw) * bh * 1.5f / (2.f * (bw + bh));
            const float x0 = std::max(0.f, minX - d);
            const float y0 = std::max(0.f, minY - d);
            const float x1 = std::min(float(w - 1), maxX + d);
            const float y1 = std::min(float(h - 1), maxY + d);
            // Component boxes are in padded-canvas pixels; scaleX/scaleY fold
            // the output -> canvas and canvas -> source steps into one factor.
            QRectF r(x0 * scaleX, y0 * scaleY, (x1 - x0 + 1) * scaleX, (y1 - y0 + 1) * scaleY);
            r = r.intersected(QRectF(src.rect()));
            if (r.width() < 4 || r.height() < 4)
                continue;
            boxes.push_back({r, score, 0});
        }
    }
    // Reading order: sort by vertical centre, then group boxes whose vertical
    // centres are within half a line height of each other, then left to right.
    std::sort(boxes.begin(), boxes.end(), [](const DetBox &a, const DetBox &b) {
        return a.rect.center().y() < b.rect.center().y();
    });
    std::vector<std::vector<DetBox>> lines;
    for (const DetBox &b : boxes) {
        bool placed = false;
        if (!lines.empty()) {
            std::vector<DetBox> &last = lines.back();
            float cy = 0, hh = 0, left = 1e9f, right = -1e9f;
            for (const DetBox &o : last) {
                cy += float(o.rect.center().y());
                hh = std::max(hh, float(o.rect.height()));
                left = std::min(left, float(o.rect.left()));
                right = std::max(right, float(o.rect.right()));
            }
            cy /= float(last.size());
            const float gap = b.rect.left() > right ? float(b.rect.left()) - right
                             : b.rect.right() < left ? left - float(b.rect.right())
                                                     : 0.f;
            // Same visual row AND close enough horizontally. A sidebar entry
            // and the page text beside it share a y but are separate columns;
            // merging them underlined both at once in the OCR text picker.
            if (std::abs(float(b.rect.center().y()) - cy)
                    <= 0.5f * std::max(hh, float(b.rect.height()))
                && gap <= 2.f * std::max(hh, float(b.rect.height()))) {
                last.push_back(b);
                placed = true;
            }
        }
        if (!placed)
            lines.push_back({b});
    }
    boxes.clear();
    int lineNo = 0;
    for (std::vector<DetBox> &line : lines) {
        std::sort(line.begin(), line.end(),
                  [](const DetBox &a, const DetBox &b) { return a.rect.left() < b.rect.left(); });
        for (DetBox &b : line) {
            b.line = lineNo;
            boxes.push_back(b);
        }
        ++lineNo;
    }
    return boxes;
}

} // namespace

// ---------------------------------------------------------------- Impl

struct PaddleOcr::Impl {
    struct Session {
        OrtSession *session = nullptr;
        QByteArray inName;
        QByteArray outName;
    };

    QLibrary lib;
    const OrtApi *api = nullptr;
    OrtEnv *env = nullptr;
    QString version;

    Session det;
    Session rec;
    QVector<QString> dict;
    QString loadedTier;

    std::mutex mutex;
    std::atomic_bool cancelled{false};

    ~Impl()
    {
        releaseSessions();
        if (env && api)
            api->ReleaseEnv(env);
    }

    void close(Session &s)
    {
        if (s.session && api)
            api->ReleaseSession(s.session);
        s.session = nullptr;
        s.inName.clear();
        s.outName.clear();
    }

    void releaseSessions()
    {
        std::lock_guard<std::mutex> lock(mutex);
        close(det);
        close(rec);
        dict.clear();
        loadedTier.clear();
    }

    // The idle timer runs on the GUI thread; never wait there for a run to
    // finish, just leave the sessions for the next idle tick.
    void releaseSessionsIfIdle()
    {
        std::unique_lock<std::mutex> lock(mutex, std::try_to_lock);
        if (!lock.owns_lock())
            return;
        close(det);
        close(rec);
        dict.clear();
        loadedTier.clear();
    }

    bool openSession(const QString &path, Session &out, QString *error);
    bool ensureLoaded(const QString &tier, QString *error);
    std::vector<DetBox> detect(const QImage &img, QString *error);
    LineOut recognizeLine(const QImage &img, const DetBox &box, QString *error);
    RunOut run(const QImage &img, const QString &tier, bool barcodeFirst);
};

bool PaddleOcr::Impl::openSession(const QString &path, Session &out, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = PaddleOcr::tr("Cannot read OCR model %1: %2").arg(path, file.errorString());
        return false;
    }
    const QByteArray bytes = file.readAll();
    if (bytes.isEmpty()) {
        *error = PaddleOcr::tr("OCR model %1 is empty").arg(path);
        return false;
    }
    OrtSessionOptions *opts = nullptr;
    OrtAllocator *alloc = nullptr;
    char *inName = nullptr, *outName = nullptr;
    const auto cleanup = qScopeGuard([&] {
        if (opts)
            api->ReleaseSessionOptions(opts);
        if (inName && alloc)
            alloc->Free(alloc, inName);
        if (outName && alloc)
            alloc->Free(alloc, outName);
    });
    const int threads = qBound(1, QThread::idealThreadCount(), 8);
    bool ok = ortOk(api->CreateSessionOptions(&opts), api, error)
              && ortOk(api->SetSessionGraphOptimizationLevel(opts, ORT_ENABLE_ALL), api, error)
              && ortOk(api->SetIntraOpNumThreads(opts, threads), api, error)
              && ortOk(api->CreateSessionFromArray(env, bytes.constData(), size_t(bytes.size()), opts,
                                                   &out.session),
                       api, error);
    if (!ok)
        return false;
    if (!ortOk(api->GetAllocatorWithDefaultOptions(&alloc), api, error))
        return false;
    if (!ortOk(api->SessionGetInputName(out.session, 0, alloc, &inName), api, error)
        || !ortOk(api->SessionGetOutputName(out.session, 0, alloc, &outName), api, error))
        return false;
    out.inName = QByteArray(inName);
    out.outName = QByteArray(outName);
    return true;
}

bool PaddleOcr::Impl::ensureLoaded(const QString &tier, QString *error)
{
    if (det.session && rec.session && loadedTier == tier)
        return true;
    close(det);
    close(rec);
    dict.clear();
    const TierPin *pin = tierPin(tier);
    const QString dir = PaddleOcr::modelDir(tier);
    if (!openSession(dir + QLatin1Char('/') + QLatin1String(pin->files[0].name), det, error)
        || !openSession(dir + QLatin1Char('/') + QLatin1String(pin->files[1].name), rec, error))
        return false;
    QFile dictFile(dir + QLatin1Char('/') + QLatin1String(pin->files[2].name));
    if (!dictFile.open(QIODevice::ReadOnly)) {
        *error = PaddleOcr::tr("Cannot read OCR dictionary %1: %2")
                     .arg(dictFile.fileName(), dictFile.errorString());
        return false;
    }
    QByteArray raw = dictFile.readAll();
    if (raw.endsWith('\n'))
        raw.chop(1);
    const QList<QByteArray> lines = raw.split('\n');
    dict.reserve(lines.size());
    for (QByteArray line : lines) {
        if (line.endsWith('\r'))
            line.chop(1);
        dict.append(QString::fromUtf8(line));
    }
    if (dict.isEmpty()) {
        *error = PaddleOcr::tr("OCR dictionary %1 is empty").arg(dictFile.fileName());
        return false;
    }
    loadedTier = tier;
    return true;
}

std::vector<DetBox> PaddleOcr::Impl::detect(const QImage &img, QString *error)
{
    const int maxSide = std::max(img.width(), img.height());
    // Snow Shot's default policy, adapted: small screenshots stay at native
    // resolution, a normal 1080p/1440p shot at native or 2000 px, a 4K shot
    // downscaled. Detection is the expensive half, so this is the main knob.
    const int limit = maxSide < 960 ? 960 : maxSide < 1500 ? 1500 : 2000;
    const double scale = std::min(1.0, double(limit) / maxSide);
    const int drawW = std::max(1, int(std::lround(img.width() * scale)));
    const int drawH = std::max(1, int(std::lround(img.height() * scale)));
    const int w = ((drawW + 31) / 32) * 32;
    const int h = ((drawH + 31) / 32) * 32;

    QImage scaled = img.scaled(drawW, drawH, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                        .convertToFormat(QImage::Format_RGB888);
    QImage canvas;
    const QImage *input = &scaled;
    if (w != drawW || h != drawH) {
        canvas = QImage(w, h, QImage::Format_RGB888);
        canvas.fill(Qt::black);
        QPainter p(&canvas);
        p.drawImage(0, 0, scaled);
        p.end();
        input = &canvas;
    }

    const size_t plane = size_t(w) * h;
    std::vector<float> tensor(plane * 3);
    for (int y = 0; y < h; ++y) {
        const uchar *line = input->constScanLine(y);
        for (int x = 0; x < w; ++x) {
            const uchar *px = line + size_t(x) * 3;
            const size_t o = size_t(y) * w + x;
            tensor[o] = (px[2] / 255.f - kDetMean[0]) / kDetStd[0];
            tensor[plane + o] = (px[1] / 255.f - kDetMean[1]) / kDetStd[1];
            tensor[plane * 2 + o] = (px[0] / 255.f - kDetMean[2]) / kDetStd[2];
        }
    }

    OrtMemoryInfo *mem = nullptr;
    OrtValue *in = nullptr, *out = nullptr;
    const auto cleanup = qScopeGuard([&] {
        if (out)
            api->ReleaseValue(out);
        if (in)
            api->ReleaseValue(in);
        if (mem)
            api->ReleaseMemoryInfo(mem);
    });
    const int64_t shape[4] = {1, 3, h, w};
    if (!ortOk(api->CreateCpuMemoryInfo(OrtDeviceAllocator, OrtMemTypeDefault, &mem), api, error)
        || !ortOk(api->CreateTensorWithDataAsOrtValue(mem, tensor.data(), tensor.size() * sizeof(float),
                                                      shape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
                                                      &in),
                  api, error))
        return {};
    const char *inName = det.inName.constData();
    const char *outName = det.outName.constData();
    if (!ortOk(api->Run(det.session, nullptr, &inName, &in, 1, &outName, 1, &out), api, error))
        return {};

    OrtTensorTypeAndShapeInfo *info = nullptr;
    ONNXTensorElementDataType type = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
    size_t rank = 0;
    int64_t dims[4] = {};
    float *prob = nullptr;
    bool shaped = ortOk(api->GetTensorTypeAndShape(out, &info), api, error)
                  && ortOk(api->GetTensorElementType(info, &type), api, error)
                  && ortOk(api->GetDimensionsCount(info, &rank), api, error) && rank == 4
                  && ortOk(api->GetDimensions(info, dims, 4), api, error);
    if (info)
        api->ReleaseTensorTypeAndShapeInfo(info);
    if (!shaped || type != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT || dims[0] != 1 || dims[1] != 1
        || dims[2] <= 0 || dims[3] <= 0
        || !ortOk(api->GetTensorMutableData(out, reinterpret_cast<void **>(&prob)), api, error)) {
        if (error->isEmpty())
            *error = PaddleOcr::tr("The PaddleOCR detection model returned an unexpected shape");
        return {};
    }
    const int ow = int(dims[3]), oh = int(dims[2]);
    // Two separate scales: output -> padded canvas, then padded canvas ->
    // source. The source image occupies drawW/drawH of the padded canvas, so
    // collapsing them into img/ow silently shifted every box when the pad
    // rounded the canvas up (a 1000 px screenshot detects on a 1024 px map,
    // which moved text 16 px up and made the recognizer crop the wrong row).
    const float sx = float(img.width()) * float(w) / (float(ow) * float(drawW));
    const float sy = float(img.height()) * float(h) / (float(oh) * float(drawH));
    return components(prob, ow, oh, img, sx, sy);
}

LineOut PaddleOcr::Impl::recognizeLine(const QImage &img, const DetBox &box, QString *error)
{
    LineOut line;
    const QRect boxRect = box.rect.toAlignedRect();
    const int pad = std::max(2, boxRect.height() / 10);
    const QRect cropRect = boxRect.adjusted(-pad, -pad, pad, pad).intersected(img.rect());
    if (cropRect.width() < 2 || cropRect.height() < 2)
        return line;
    const QImage crop = img.copy(cropRect).convertToFormat(QImage::Format_RGB888);
    const int nw = qBound(8, int(std::lround(crop.width() * 48.0 / crop.height())), 2048);
    const QImage resized = crop.scaled(nw, 48, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                               .convertToFormat(QImage::Format_RGB888);

    const size_t plane = size_t(nw) * 48;
    std::vector<float> tensor(plane * 3);
    for (int y = 0; y < 48; ++y) {
        const uchar *sl = resized.constScanLine(y);
        for (int x = 0; x < nw; ++x) {
            const uchar *px = sl + size_t(x) * 3;
            const size_t o = size_t(y) * nw + x;
            tensor[o] = (px[2] / 255.f - 0.5f) / 0.5f;
            tensor[plane + o] = (px[1] / 255.f - 0.5f) / 0.5f;
            tensor[plane * 2 + o] = (px[0] / 255.f - 0.5f) / 0.5f;
        }
    }

    OrtMemoryInfo *mem = nullptr;
    OrtValue *in = nullptr, *out = nullptr;
    const auto cleanup = qScopeGuard([&] {
        if (out)
            api->ReleaseValue(out);
        if (in)
            api->ReleaseValue(in);
        if (mem)
            api->ReleaseMemoryInfo(mem);
    });
    const int64_t shape[4] = {1, 3, 48, nw};
    if (!ortOk(api->CreateCpuMemoryInfo(OrtDeviceAllocator, OrtMemTypeDefault, &mem), api, error)
        || !ortOk(api->CreateTensorWithDataAsOrtValue(mem, tensor.data(), tensor.size() * sizeof(float),
                                                      shape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
                                                      &in),
                  api, error))
        return line;
    const char *inName = rec.inName.constData();
    const char *outName = rec.outName.constData();
    if (!ortOk(api->Run(rec.session, nullptr, &inName, &in, 1, &outName, 1, &out), api, error))
        return line;

    OrtTensorTypeAndShapeInfo *info = nullptr;
    ONNXTensorElementDataType type = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
    size_t rank = 0;
    int64_t dims[3] = {};
    float *logits = nullptr;
    bool shaped = ortOk(api->GetTensorTypeAndShape(out, &info), api, error)
                  && ortOk(api->GetTensorElementType(info, &type), api, error)
                  && ortOk(api->GetDimensionsCount(info, &rank), api, error) && rank == 3
                  && ortOk(api->GetDimensions(info, dims, 3), api, error);
    if (info)
        api->ReleaseTensorTypeAndShapeInfo(info);
    const int classes = int(dims[2]);
    if (!shaped || type != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT || dims[0] != 1 || dims[1] <= 0
        || classes != dict.size() + 2
        || !ortOk(api->GetTensorMutableData(out, reinterpret_cast<void **>(&logits)), api, error)) {
        if (error->isEmpty())
            *error = PaddleOcr::tr("The PaddleOCR recognition model does not match its dictionary");
        return line;
    }
    const int steps = int(dims[1]);
    std::vector<int> best(steps);
    std::vector<float> bestProb(steps);
    for (int t = 0; t < steps; ++t) {
        const float *row = logits + size_t(t) * classes;
        int bi = 0;
        float bp = row[0];
        for (int c = 1; c < classes; ++c)
            if (row[c] > bp) {
                bp = row[c];
                bi = c;
            }
        best[t] = bi;
        bestProb[t] = bp;
    }

    // CTC alignment is a peak per character, not a span: on a crisp UI line
    // the model often emits one timestep for a character with blanks around
    // it. Using the peak run width made every glyph box a third of its real
    // width, which made the text picker nearly impossible to aim at. Record
    // each token's CENTRE and cut the line at the midpoints between neighbours
    // instead, so the boxes tile the line and a click lands on a real glyph.
    struct Token {
        QString text;
        int center = 0;
        float prob = 0;
        bool space = false;
    };
    std::vector<Token> tokens;
    int prev = -1;
    for (int t = 0; t < steps;) {
        const int idx = best[t];
        const int start = t;
        while (t < steps && best[t] == idx)
            ++t;
        const int end = t - 1;
        if (idx == 0) {
            prev = 0;
            continue;
        }
        if (idx == prev) {
            prev = idx;
            continue;
        }
        prev = idx;
        if (idx == classes - 1) { // the dictionary has no space; it is appended
            tokens.push_back({QString(), (start + end) / 2, bestProb[start], true});
            continue;
        }
        if (idx < 1 || idx > dict.size())
            continue;
        const QString ch = dict.at(idx - 1);
        if (ch.isEmpty())
            continue;
        tokens.push_back({ch, (start + end) / 2, bestProb[start], false});
    }
    QString text;
    float confSum = 0;
    int confCount = 0;
    for (size_t i = 0; i < tokens.size(); ++i) {
        const Token &tok = tokens[i];
        if (tok.space)
            continue;
        const double before = i > 0 ? (tokens[i - 1].center + tok.center) / 2.0
                                    : tok.center - 0.5;
        const double after = i + 1 < tokens.size() ? (tokens[i + 1].center + tok.center) / 2.0
                                                   : tok.center + 0.5;
        const double xa = std::clamp(before, 0.0, double(steps)) / steps;
        const double xb = std::clamp(after, 0.0, double(steps)) / steps;
        const int x0 = cropRect.x() + int(std::lround(xa * cropRect.width()));
        const int x1 = cropRect.x() + int(std::lround(xb * cropRect.width()));
        OcrWord w;
        w.rect = QRect(x0, cropRect.y(), std::max(1, x1 - x0), cropRect.height());
        w.text = tok.text;
        w.confidence = tok.prob * 100.f;
        w.spaceBefore = i > 0 && tokens[i - 1].space;
        line.words.append(w);
        if (w.spaceBefore)
            text += QLatin1Char(' ');
        text += w.text;
        confSum += tok.prob;
        ++confCount;
    }
    line.text = text;
    line.confidence = confCount ? confSum / confCount : 0.f;
    return line;
}

RunOut PaddleOcr::Impl::run(const QImage &img, const QString &tier, bool barcodeFirst)
{
    RunOut r;
    if (cancelled.load())
        return r;
    std::lock_guard<std::mutex> lock(mutex);
    if (cancelled.load())
        return r;
    if (barcodeFirst) {
        const QString payload = scanBarcode(img);
        if (!payload.isEmpty()) {
            r.text = payload;
            return r;
        }
    }
    if (!ensureLoaded(tier, &r.error))
        return r;
    const QImage source = flattenOnWhite(img);
    const std::vector<DetBox> boxes = detect(source, &r.error);
    if (!r.error.isEmpty())
        return r;
    for (const DetBox &box : boxes) {
        if (cancelled.load())
            return {};
        LineOut out = recognizeLine(source, box, &r.error);
        if (!r.error.isEmpty())
            return r;
        if (out.words.isEmpty() || out.confidence < 0.5f)
            continue;
        for (OcrWord w : std::as_const(out.words)) {
            w.line = box.line;
            r.words.append(w);
        }
    }
    // Separate det boxes on one line are joined with a space only when the gap
    // is a real word gap; Chinese boxes are usually adjacent and must not get
    // one. The recognizer's own space token already sets spaceBefore.
    for (int i = 1; i < r.words.size(); ++i) {
        OcrWord &w = r.words[i];
        const OcrWord &prev = r.words[i - 1];
        if (w.line == prev.line && !w.spaceBefore
            && w.rect.left() - prev.rect.right() > std::max(4, prev.rect.height() / 2))
            w.spaceBefore = true;
    }
    QString joined;
    int lastLine = -1;
    for (const OcrWord &w : std::as_const(r.words)) {
        if (w.line != lastLine) {
            if (lastLine >= 0)
                joined += QLatin1Char('\n');
            lastLine = w.line;
        } else if (w.spaceBefore) {
            joined += QLatin1Char(' ');
        }
        joined += w.text;
    }
    r.text = joined.trimmed();
    return r;
}

// ---------------------------------------------------------------- PaddleOcr

PaddleOcr::PaddleOcr(Settings *settings, QObject *parent)
    : QObject(parent), m_settings(settings), m_impl(std::make_shared<Impl>())
{
    m_idle = new QTimer(this);
    m_idle->setSingleShot(true);
    m_idle->setInterval(2 * 60 * 1000);
    connect(m_idle, &QTimer::timeout, this, [this] { m_impl->releaseSessionsIfIdle(); });
    if (m_settings)
        connect(m_settings, &Settings::paddleTierChanged, this, [this] {
            emit tierChanged();
            emit stateChanged();
        });
}

PaddleOcr::~PaddleOcr()
{
    m_impl->cancelled.store(true);
    m_impl->releaseSessions();
}

bool PaddleOcr::available()
{
    // The download/extract path uses tar and an x86_64 ONNX Runtime tarball;
    // Windows keeps Tesseract only, like background removal.
    return QSysInfo::kernelType() == QLatin1String("linux");
}

QString PaddleOcr::tier() const
{
    // A hand-edited config can name anything; always resolve to a real tier so
    // modelDir()/detPath() and the pins cannot disagree.
    return tierId(tierPin(m_settings ? m_settings->paddleTier() : QStringLiteral("small")));
}

QString PaddleOcr::modelDir(const QString &tier)
{
    return modelRoot() + QLatin1Char('/') + tier;
}

QString PaddleOcr::detPath(const QString &tier)
{
    return modelDir(tier) + QLatin1Char('/') + QLatin1String(tierPin(tier)->files[0].name);
}

QString PaddleOcr::recPath(const QString &tier)
{
    return modelDir(tier) + QLatin1Char('/') + QLatin1String(tierPin(tier)->files[1].name);
}

QString PaddleOcr::dictPath(const QString &tier)
{
    return modelDir(tier) + QLatin1Char('/') + QLatin1String(tierPin(tier)->files[2].name);
}

bool PaddleOcr::runtimeFound()
{
    if (!m_probed)
        probe();
    return m_impl->api != nullptr;
}

QString PaddleOcr::runtimeInfo()
{
    runtimeFound();
    return m_runtimeInfo;
}

bool PaddleOcr::canDownloadRuntime() const
{
    return available() && QSysInfo::currentCpuArchitecture() == QLatin1String("x86_64")
           && !QFile::exists(QStringLiteral("/.flatpak-info"));
}

bool PaddleOcr::modelInstalled() const
{
    const TierPin *pin = tierPin(tier());
    for (const FilePin &f : pin->files) {
        const QFileInfo fi(modelDir(tier()) + QLatin1Char('/') + QLatin1String(f.name));
        if (!fi.exists() || fi.size() != f.bytes)
            return false;
    }
    return true;
}

bool PaddleOcr::hasDownloads() const
{
    return QFile::exists(detPath(tier())) || QFile::exists(recPath(tier()))
           || QFile::exists(dictPath(tier()));
}

QString PaddleOcr::downloadSizeText()
{
    qint64 bytes = 0;
    if (!runtimeFound() && canDownloadRuntime())
        bytes += kRuntimeBytes;
    const TierPin *pin = tierPin(tier());
    for (const FilePin &f : pin->files) {
        const QFileInfo fi(modelDir(tier()) + QLatin1Char('/') + QLatin1String(f.name));
        if (!fi.exists() || fi.size() != f.bytes)
            bytes += f.bytes;
    }
    return QLocale().formattedDataSize(bytes, 0, QLocale::DataSizeTraditionalFormat);
}

bool PaddleOcr::supportsLanguages(const QString &langs, bool autoScript) const
{
    if (autoScript)
        return true;
    // PP-OCRv6 reads Chinese (simplified and traditional), Japanese and 48
    // Latin-script languages. Everything else is still Tesseract's job: the v6
    // recogniser would output confident-looking garbage for Cyrillic/Arabic/
    // Korean/Indic/Thai text. Keep this list in step with the scripts in
    // OcrEngine::scriptCodeTable().
    static const QSet<QString> notInV6 = {
        QStringLiteral("ara"), QStringLiteral("fas"), QStringLiteral("urd"), QStringLiteral("pus"),
        QStringLiteral("uig"), QStringLiteral("snd"), QStringLiteral("ckb"), QStringLiteral("heb"),
        QStringLiteral("yid"), QStringLiteral("kor"), QStringLiteral("kor_vert"),
        QStringLiteral("hin"), QStringLiteral("mar"), QStringLiteral("san"), QStringLiteral("nep"),
        QStringLiteral("ben"), QStringLiteral("asm"), QStringLiteral("tam"), QStringLiteral("tel"),
        QStringLiteral("kan"), QStringLiteral("mal"), QStringLiteral("guj"), QStringLiteral("pan"),
        QStringLiteral("tha"), QStringLiteral("lao"), QStringLiteral("khm"), QStringLiteral("mya"),
        QStringLiteral("sin"), QStringLiteral("bod"), QStringLiteral("ell"), QStringLiteral("grc"),
        QStringLiteral("rus"), QStringLiteral("ukr"), QStringLiteral("bul"), QStringLiteral("srp"),
        QStringLiteral("mkd"), QStringLiteral("bel"), QStringLiteral("kaz"), QStringLiteral("kir"),
        QStringLiteral("mon"), QStringLiteral("tgk"), QStringLiteral("kat"), QStringLiteral("hye"),
        QStringLiteral("amh"), QStringLiteral("tir"), QStringLiteral("chi_sim_vert"),
        QStringLiteral("chi_tra_vert"), QStringLiteral("jpn_vert"),
    };
    const QStringList parts = langs.split(QLatin1Char('+'), Qt::SkipEmptyParts);
    for (const QString &p : parts)
        if (notInV6.contains(p))
            return false;
    return true;
}

void PaddleOcr::probe()
{
    m_probed = true;
    if (m_impl->api)
        return;
    QList<std::pair<QString, int>> candidates{{QStringLiteral("onnxruntime"), 1}};
    const QDir qtLibs(QLibraryInfo::path(QLibraryInfo::LibrariesPath));
    for (const QFileInfo &fi : qtLibs.entryInfoList({QStringLiteral("libonnxruntime.so.1.*")},
                                                    QDir::Files, QDir::Name | QDir::Reversed))
        candidates.append({fi.absoluteFilePath(), -1});
    candidates.append({runtimeDir() + QLatin1Char('/') + QLatin1String(kRuntimeFile), -1});

    m_runtimeInfo.clear();
    auto impl = std::make_shared<Impl>();
    for (const auto &[name, version] : candidates) {
        if (version >= 0)
            impl->lib.setFileNameAndVersion(name, version);
        else
            impl->lib.setFileName(name);
        if (!impl->lib.load())
            continue;
        using GetApiBase = const OrtApiBase *(ORT_API_CALL *)();
        const auto getBase = reinterpret_cast<GetApiBase>(impl->lib.resolve("OrtGetApiBase"));
        const OrtApiBase *base = getBase ? getBase() : nullptr;
        if (!base) {
            impl->lib.unload();
            continue;
        }
        const QString ver = QString::fromUtf8(base->GetVersionString());
        impl->api = QVersionNumber::fromString(ver) >= kMinRuntime ? base->GetApi(ORT_API_VERSION) : nullptr;
        if (!impl->api) {
            m_runtimeInfo = tr("ONNX Runtime %1 is too old, %2 or newer is needed")
                                .arg(ver, kMinRuntime.toString());
            impl->lib.unload();
            continue;
        }
        if (OrtStatus *s = impl->api->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "unisic-paddle", &impl->env)) {
            m_runtimeInfo = QString::fromUtf8(impl->api->GetErrorMessage(s));
            impl->api->ReleaseStatus(s);
            impl->api = nullptr;
            impl->lib.unload();
            continue;
        }
        impl->version = ver;
        m_impl = impl;
        m_runtimeInfo = tr("ONNX Runtime %1").arg(ver);
        qInfo().noquote() << "paddleocr: ONNX Runtime" << ver << "from" << impl->lib.fileName();
        return;
    }
    impl->api = nullptr;
    if (m_runtimeInfo.isEmpty())
        m_runtimeInfo = tr("ONNX Runtime: not installed");
}

QNetworkAccessManager *PaddleOcr::nam()
{
    if (!m_nam)
        m_nam = new QNetworkAccessManager(this);
    return m_nam;
}

void PaddleOcr::fail(const QString &message)
{
    qWarning().noquote() << "paddleocr download:" << message;
    m_queue.clear();
    m_error = message;
    emit stateChanged();
}

void PaddleOcr::download()
{
    if (downloading())
        return;
    m_error.clear();
    m_queue.clear();
    m_cancelled = false;
    m_downloadTier = tier();
    if (!runtimeFound()) {
        if (!canDownloadRuntime()) {
            fail(tr("PaddleOCR needs ONNX Runtime. Install the onnxruntime package, then try again."));
            return;
        }
        m_queue << -1;
    }
    const TierPin *pin = tierPin(m_downloadTier);
    for (int i = 0; i < 3; ++i) {
        const QFileInfo fi(modelDir(m_downloadTier) + QLatin1Char('/')
                           + QLatin1String(pin->files[i].name));
        if (!fi.exists() || fi.size() != pin->files[i].bytes)
            m_queue << i;
    }
    if (m_queue.isEmpty()) {
        emit stateChanged();
        return;
    }
    m_totalBytes = 0;
    for (int i : std::as_const(m_queue)) {
        if (i < 0)
            m_totalBytes += kRuntimeBytes;
        else
            m_totalBytes += pin->files[i].bytes;
    }
    m_doneBytes = 0;
    m_progress = 0;
    emit progressChanged();
    fetchNext();
}

void PaddleOcr::fetchNext()
{
    if (m_queue.isEmpty()) {
        m_progress = 1;
        emit progressChanged();
        emit stateChanged();
        return;
    }
    const int job = m_queue.takeFirst();
    const bool runtime = job < 0;
    const TierPin *pin = tierPin(m_downloadTier);
    const FilePin &fp = runtime ? FilePin{kRuntimeFile, kRuntimeBytes, kRuntimeSha} : pin->files[job];
    const QString dest = runtime ? runtimeDir() + QStringLiteral("/runtime.tgz")
                                 : modelDir(m_downloadTier) + QLatin1Char('/') + QLatin1String(fp.name);
    const QUrl url(runtime ? QString::fromLatin1(kRuntimeUrl)
                           : modelsBase() + m_downloadTier + QLatin1Char('/') + QLatin1String(fp.name));
    auto file = std::make_shared<QSaveFile>(dest);
    if (!QDir().mkpath(QFileInfo(dest).absolutePath()) || !file->open(QIODevice::WriteOnly)) {
        fail(tr("Cannot write %1: %2").arg(dest, file->errorString()));
        return;
    }
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("Unisic/" UNISIC_VERSION " (screenshot tool)"));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(60000);
    QNetworkReply *reply = nam()->get(req);
    m_reply = reply;
    emit stateChanged();

    auto hash = std::make_shared<QCryptographicHash>(QCryptographicHash::Sha256);
    auto received = std::make_shared<qint64>(0);
    auto problem = std::make_shared<QString>();
    auto take = [reply, file, hash, received, problem, bytes = fp.bytes] {
        const QByteArray chunk = reply->readAll();
        *received += chunk.size();
        if (*received > bytes)
            *problem = tr("The download is larger than expected, so it was not kept");
        else if (file->write(chunk) != chunk.size())
            *problem = tr("Cannot write %1: %2").arg(file->fileName(), file->errorString());
        else
            hash->addData(chunk);
        return problem->isEmpty();
    };
    connect(reply, &QNetworkReply::readyRead, this, [reply, take] {
        if (!take())
            reply->abort();
    });
    connect(reply, &QNetworkReply::downloadProgress, this, [this](qint64 got, qint64) {
        m_progress = m_totalBytes > 0 ? qreal(m_doneBytes + got) / m_totalBytes : 0;
        emit progressChanged();
    });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, file, hash, received, problem, take, runtime, dest, bytes = fp.bytes,
             sha = QByteArray(fp.sha256)] {
                reply->deleteLater();
                m_reply = nullptr;
                const QNetworkReply::NetworkError error = reply->error();
                if (error == QNetworkReply::NoError)
                    take();
                if (error != QNetworkReply::NoError || !problem->isEmpty()) {
                    file->cancelWriting();
                    if (!problem->isEmpty())
                        fail(*problem);
                    else if (error != QNetworkReply::OperationCanceledError)
                        fail(tr("Download failed: %1").arg(reply->errorString()));
                    else if (!m_cancelled)
                        fail(tr("Download stopped: nothing arrived for a minute"));
                    else
                        emit stateChanged();
                    return;
                }
                if (*received != bytes || hash->result().toHex() != sha) {
                    file->cancelWriting();
                    fail(tr("The download does not match the expected file, so it was not kept"));
                    return;
                }
                if (!file->commit()) {
                    fail(tr("Cannot write %1: %2").arg(dest, file->errorString()));
                    return;
                }
                m_doneBytes += bytes;
                if (runtime)
                    installRuntimeArchive(dest);
                else
                    fetchNext();
            });
}

void PaddleOcr::installRuntimeArchive(const QString &archive)
{
    m_extractingRuntime = true;
    auto *tar = new QProcess(this);
    m_tar = tar;
    tar->setProcessChannelMode(QProcess::MergedChannels);
    auto doneOnce = std::make_shared<bool>(false);
    auto done = [this, tar, archive, doneOnce](const QString &problem) {
        if (*doneOnce)
            return;
        *doneOnce = true;
        tar->deleteLater();
        m_tar = nullptr;
        m_extractingRuntime = false;
        QFile::remove(archive);
        if (!problem.isEmpty()) {
            fail(tr("Cannot unpack ONNX Runtime: %1").arg(problem));
            return;
        }
        m_probed = false;
        probe();
        emit stateChanged();
        fetchNext();
    };
    connect(tar, &QProcess::errorOccurred, this, [tar, done](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            done(tar->errorString());
    });
    connect(tar, &QProcess::finished, this, [this, tar, done](int code, QProcess::ExitStatus status) {
        const bool unpacked = status == QProcess::NormalExit && code == 0
                              && QFile::exists(runtimeDir() + QLatin1Char('/')
                                               + QLatin1String(kRuntimeFile));
        done(unpacked ? QString() : QString::fromLocal8Bit(tar->readAll()).trimmed());
    });
    tar->start(QStringLiteral("tar"),
               {QStringLiteral("-xzf"), archive, QStringLiteral("-C"), runtimeDir(),
                QStringLiteral("--strip-components=2"), QLatin1String(kRuntimeMember)});
}

void PaddleOcr::cancelDownload()
{
    m_queue.clear();
    m_cancelled = true;
    if (m_reply)
        m_reply->abort();
}

void PaddleOcr::deleteDownloads()
{
    cancelDownload();
    QDir(modelDir(tier())).removeRecursively();
    m_error.clear();
    emit stateChanged();
}

void PaddleOcr::recognize(const QImage &img, Result cb)
{
    if (img.isNull()) {
        cb({}, tr("No image to recognize"));
        return;
    }
    if (!runtimeFound() || !modelInstalled()) {
        cb({}, tr("PaddleOCR models are not downloaded - see Settings > General"));
        return;
    }
    auto impl = m_impl;
    const QString selected = tier();
    auto *fw = new QFutureWatcher<RunOut>(this);
    connect(fw, &QFutureWatcher<RunOut>::finished, this, [this, fw, cb] {
        const RunOut r = fw->result();
        fw->deleteLater();
        if (m_idle)
            m_idle->start();
        cb(r.text, r.error);
    });
    fw->setFuture(QtConcurrent::run([impl, img, selected] { return impl->run(img, selected, true); }));
}

void PaddleOcr::recognizeBoxes(const QImage &img, BoxResult cb)
{
    if (img.isNull()) {
        cb({}, tr("No image to recognize"));
        return;
    }
    if (!runtimeFound() || !modelInstalled()) {
        cb({}, tr("PaddleOCR models are not downloaded - see Settings > General"));
        return;
    }
    auto impl = m_impl;
    const QString selected = tier();
    auto *fw = new QFutureWatcher<RunOut>(this);
    connect(fw, &QFutureWatcher<RunOut>::finished, this, [this, fw, cb] {
        const RunOut r = fw->result();
        fw->deleteLater();
        if (m_idle)
            m_idle->start();
        cb(r.words, r.error);
    });
    fw->setFuture(QtConcurrent::run([impl, img, selected] { return impl->run(img, selected, false); }));
}
