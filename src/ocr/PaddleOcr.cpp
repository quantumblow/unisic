#include "PaddleOcr.h"

#include "Barcode.h"
#include "Settings.h"

#include <onnxruntime/onnxruntime_c_api.h>

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QFutureWatcher>
#include <QLibrary>
#include <QLibraryInfo>
#include <QLocale>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QProcess>
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

// PP-OCRv5's per-script recognizers. The official ONNX package has no separate
// .txt dictionary: inference.yml carries the character_dict, and the model's
// output classes are exactly entries + 2 (blank + space). Each URL is revision
// pinned; the SHA-256 still decides whether a mirror may serve the bytes.
//
// Detection stays PP-OCRv6's DB model, so only the rec+dict pair is per script.
struct ScriptFilePin {
    const char *name;
    qint64 bytes;
    const char *sha256;
    const char *url;
};
struct ScriptPin {
    const char *id;
    int entries;
    ScriptFilePin files[2]; // inference.onnx, inference.yml
};
const ScriptPin kScripts[] = {
    {"korean", 11945,
     {{"inference.onnx", 13418787,
       "92f0b7785e64fc9090106a241cf4c1eb97472824558272751b88a2a4476d3a08",
       "https://www.modelscope.cn/models/PaddlePaddle/korean_PP-OCRv5_mobile_rec_onnx/resolve/"
       "680fae0e83e0b3866d54bd8a4ea1684c9786e19e/inference.onnx"},
      {"inference.yml", 96039,
       "f757fa1c40e99edcf27e9cce879b93eb2a51fa46f5ef39095689b8c37dd75998",
       "https://www.modelscope.cn/models/PaddlePaddle/korean_PP-OCRv5_mobile_rec_onnx/resolve/"
       "680fae0e83e0b3866d54bd8a4ea1684c9786e19e/inference.yml"}}},
    {"arabic", 747,
     {{"inference.onnx", 7998947,
       "799113ebf267fbe742deb99eb36e8d42c9ddc5291ceacf92add41b4d52a59110",
       "https://www.modelscope.cn/models/PaddlePaddle/arabic_PP-OCRv5_mobile_rec_onnx/resolve/"
       "359e599dfbc299d025184914c423d571adb70fc4/inference.onnx"},
      {"inference.yml", 6165,
       "21368419e6c016c31db55d316d59e11c128e1913e6e6fe10287084710043d3a6",
       "https://www.modelscope.cn/models/PaddlePaddle/arabic_PP-OCRv5_mobile_rec_onnx/resolve/"
       "359e599dfbc299d025184914c423d571adb70fc4/inference.yml"}}},
    {"cyrillic", 850,
     {{"inference.onnx", 8048799,
       "5371ee1ddaa7983cc62d0818d99e982b6804638c85e4f960d59a574094e172e5",
       "https://www.modelscope.cn/models/PaddlePaddle/cyrillic_PP-OCRv5_mobile_rec_onnx/resolve/"
       "a4528d69e41a699dac521f47c8ea41741a12e6f9/inference.onnx"},
      {"inference.yml", 6991,
       "5c76cc91fa98410178a09f498db10050d0ec1634a660053d3005ab7be581f501",
       "https://www.modelscope.cn/models/PaddlePaddle/cyrillic_PP-OCRv5_mobile_rec_onnx/resolve/"
       "a4528d69e41a699dac521f47c8ea41741a12e6f9/inference.yml"}}},
    {"thai", 524,
     {{"inference.onnx", 7891015,
       "27618be66018f8598ac0a526a593f9f1cebf794e7eded93428e8fb016e537f5f",
       "https://www.modelscope.cn/models/PaddlePaddle/th_PP-OCRv5_mobile_rec_onnx/resolve/"
       "484b1b156c94afe243654f64ee374a7568d522a5/inference.onnx"},
      {"inference.yml", 4667,
       "f6ba7fefc38ca1ff398ddafa75d67d16e0b3757c4e6c833adffee98a981766c9",
       "https://www.modelscope.cn/models/PaddlePaddle/th_PP-OCRv5_mobile_rec_onnx/resolve/"
       "484b1b156c94afe243654f64ee374a7568d522a5/inference.yml"}}},
    {"devanagari", 568,
     {{"inference.onnx", 7912311,
       "cb789212ce96c69d3e74728ae4309d179281d68cb3945d0616b67cafab41c986",
       "https://www.modelscope.cn/models/PaddlePaddle/devanagari_PP-OCRv5_mobile_rec_onnx/resolve/"
       "41c3c9e8780dcffc6aea99b1de3b7895d0105849/inference.onnx"},
      {"inference.yml", 5027,
       "9bd172dd26440c8ce94d1cde5d5baea6aefdc7cf3c5c8492e0beedef656d4e54",
       "https://www.modelscope.cn/models/PaddlePaddle/devanagari_PP-OCRv5_mobile_rec_onnx/resolve/"
       "41c3c9e8780dcffc6aea99b1de3b7895d0105849/inference.yml"}}},
    {"tamil", 513,
     {{"inference.onnx", 7885691,
       "c6d2b682d2a0ea4cb1fccdba295976f93fd439964d16cdc666cadef531accbee",
       "https://www.modelscope.cn/models/PaddlePaddle/ta_PP-OCRv5_mobile_rec_onnx/resolve/"
       "2e1aa287642bb6630167bdcb6918e3ccafbb33b0/inference.onnx"},
      {"inference.yml", 4579,
       "88a28f5a1bb30cabe38a0985cb5e6619fa4f0c7c78e57a08274674228c5219a6",
       "https://www.modelscope.cn/models/PaddlePaddle/ta_PP-OCRv5_mobile_rec_onnx/resolve/"
       "2e1aa287642bb6630167bdcb6918e3ccafbb33b0/inference.yml"}}},
    {"telugu", 540,
     {{"inference.onnx", 7898759,
       "8238bfc46d4cffe720ed6706e3842802467343497428693ff2bfb4e6b3caa36b",
       "https://www.modelscope.cn/models/PaddlePaddle/te_PP-OCRv5_mobile_rec_onnx/resolve/"
       "f9cc8f880b41b638098ac88851f706df7cf841e4/inference.onnx"},
      {"inference.yml", 4795,
       "acebbe53f1831bf28ddfed75aedf58225d7aa5d09100c1d5a9140a2a53b137ce",
       "https://www.modelscope.cn/models/PaddlePaddle/te_PP-OCRv5_mobile_rec_onnx/resolve/"
       "f9cc8f880b41b638098ac88851f706df7cf841e4/inference.yml"}}},
    {"greek", 354,
     {{"inference.onnx", 7808735,
       "2acf17fcaea2bc81b878e311e6263b8885f48bb03796f75f9f30ed3242bbaa6d",
       "https://www.modelscope.cn/models/PaddlePaddle/el_PP-OCRv5_mobile_rec_onnx/resolve/"
       "a4cfdc1e7e6f02f09418ac8481b56e1ca6ed6de0/inference.onnx"},
      {"inference.yml", 3317,
       "17d85b2fe2d2f24cd4ab07bcbc33e0c126859b956ced36e281dc65e2d0c1f0bf",
       "https://www.modelscope.cn/models/PaddlePaddle/el_PP-OCRv5_mobile_rec_onnx/resolve/"
       "a4cfdc1e7e6f02f09418ac8481b56e1ca6ed6de0/inference.yml"}}},
};
const ScriptPin *scriptPin(const QString &id)
{
    for (const ScriptPin &p : kScripts)
        if (id == QLatin1String(p.id))
            return &p;
    return nullptr;
}
QString mirrorBase()
{
    const QString mirror = qEnvironmentVariable("UNISIC_PADDLE_MODEL_BASE");
    if (mirror.isEmpty())
        return {};
    return mirror.endsWith(QLatin1Char('/')) ? mirror : mirror + QLatin1Char('/');
}
QString scriptFileUrl(const ScriptPin &pin, const ScriptFilePin &file)
{
    const QString mirror = mirrorBase();
    if (!mirror.isEmpty())
        return mirror + QStringLiteral("v5/") + QLatin1String(pin.id) + QLatin1Char('/')
               + QLatin1String(file.name);
    return QString::fromLatin1(file.url);
}

// MemAvailable from /proc/meminfo in MiB, -1 where there is no such file (not
// Linux), which skips the check rather than refusing to run. The text parse is
// PaddleOcr::parseMemAvailableMiB(), pure so a test can pin it.
qint64 availableMiB()
{
    QFile file(QStringLiteral("/proc/meminfo"));
    if (!file.open(QIODevice::ReadOnly))
        return -1;
    return PaddleOcr::parseMemAvailableMiB(file.readAll());
}

QString memoryError(const QString &tier, const QString &script = {})
{
    const qint64 free = availableMiB();
    if (free < 0)
        return {};
    // A v5 rec replaces the v6 rec, so this is the larger of the two paths,
    // never a sum. The script table is measured with the det model loaded.
    const qint64 need = std::max(PaddleOcr::tierRunMiB(tier),
                                 script.isEmpty() ? 0 : PaddleOcr::scriptRunMiB(script));
    if (free >= need)
        return {};
    const QLocale loc;
    return PaddleOcr::tr("PaddleOCR needs about %1 GB of free memory and %2 GB is free - close something and try again")
        .arg(loc.toString(need / 1024.0, 'f', 1), loc.toString(free / 1024.0, 'f', 1));
}

// The v6 tiers are served from ModelScope, the same pinned copies Snow Shot
// publishes. The v5 recognizers carry their own revision-pinned URLs in
// kScripts (see scriptFileUrl); the override below replaces the origin for
// both layouts. Swapping a source means changing its URL, sizes and hashes
// together - never one of them.
QString modelsBase()
{
    // T6.5: a mirror is a supported, documented procedure instead of a second
    // hardcoded base (the tiers only exist on ModelScope). The override is an
    // operator escape hatch for an outage, never a UI setting; every file is
    // still checked against the pinned size and SHA-256, so a mirror can serve
    // the bytes but cannot change them. The trailing slash is required in the
    // URL we build, so add it rather than trusting the caller.
    const QString mirror = mirrorBase();
    if (!mirror.isEmpty())
        return mirror;
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
// The tier that ships inside the binary through the qrc built from
// third_party/paddleocr/tiny/ (see CMakeLists.txt). Only
// the smallest one: 6.3 MB is affordable in every build, 31 MB is not. See
// third_party/paddleocr/NOTICE for provenance and CMakeLists.txt for the pins.
constexpr const char *kBundledTier = "tiny";
QString bundledPath(const QString &tier, const char *name)
{
    return QStringLiteral(":/paddleocr/") + tier + QLatin1Char('/') + QLatin1String(name);
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

// Arabic is the one script whose v5 recognizer emits visual order. PaddleX's
// pred_reverse() keeps Latin/digit/punctuation/space runs as units and reverses
// everything else; this is the word-level form of that rule, so the text and
// the glyph boxes stay in the same (logical) order. Pure Arabic lines are the
// common case; a mixed Arabic/Latin line keeps its Latin word intact.
bool asciiRtlWord(const QString &text)
{
    if (text.isEmpty())
        return false;
    for (const QChar c : text) {
        const ushort u = c.unicode();
        const bool ascii = (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z')
                           || (u >= '0' && u <= '9') || c == QLatin1Char(':')
                           || c == QLatin1Char('*') || c == QLatin1Char('.')
                           || c == QLatin1Char('/') || c == QLatin1Char('%')
                           || c == QLatin1Char('+') || c == QLatin1Char('-');
        if (!ascii)
            return false;
    }
    return true;
}

void reorderRtlWordsImpl(QVector<OcrWord> &words)
{
    if (words.size() < 2)
        return;
    struct Unit {
        int start = 0;
        int end = 0; // exclusive
    };
    QVector<Unit> units;
    for (int i = 0; i < words.size();) {
        const bool ascii = asciiRtlWord(words[i].text);
        int j = i + 1;
        // A spaceBefore on a later ASCII word is a real gap, not part of the
        // same word run; keep the run contiguous and let the gap split it.
        while (ascii && j < words.size() && asciiRtlWord(words[j].text)
               && !words[j].spaceBefore)
            ++j;
        units.append({i, j});
        i = j;
    }
    QVector<bool> gapBefore;
    gapBefore.reserve(units.size());
    for (const Unit &u : units) {
        gapBefore.append(words[u.start].spaceBefore);
        words[u.start].spaceBefore = false;
    }
    QVector<OcrWord> out;
    out.reserve(words.size());
    for (int k = units.size() - 1; k >= 0; --k) {
        const Unit &u = units[k];
        QVector<OcrWord> chunk = words.mid(u.start, u.end - u.start);
        // The gap that was before the original NEXT unit becomes the gap
        // before this one in the reversed order; the first logical word has
        // no leading space.
        if (!chunk.isEmpty())
            chunk[0].spaceBefore = (k + 1 < units.size()) ? gapBefore[k + 1] : false;
        out += chunk;
    }
    words = out;
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
    QString loadedKey;

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
        loadedKey.clear();
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
        loadedKey.clear();
    }

    bool openSession(const QString &path, Session &out, QString *error);
    bool ensureLoaded(const QString &tier, const QString &script, QString *error);
    std::vector<DetBox> detect(const QImage &img, QString *error);
    LineOut recognizeLine(const QImage &img, const DetBox &box, QString *error);
    RunOut run(const QImage &img, const QString &tier, const QString &script,
               bool barcodeFirst);
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

bool PaddleOcr::Impl::ensureLoaded(const QString &tier, const QString &script, QString *error)
{
    const QString key = tier + QLatin1Char('\n') + script;
    if (det.session && rec.session && loadedKey == key)
        return true;
    close(det);
    close(rec);
    dict.clear();
    const ScriptPin *scriptModel = script.isEmpty() ? nullptr : scriptPin(script);
    if (!script.isEmpty() && !scriptModel) {
        *error = PaddleOcr::tr("Unknown OCR script %1").arg(script);
        return false;
    }
    const QString recFile = script.isEmpty() ? PaddleOcr::recPath(tier)
                                             : PaddleOcr::scriptRecPath(script);
    if (!openSession(PaddleOcr::detPath(tier), det, error) || !openSession(recFile, rec, error))
        return false;
    const QString dictFile = script.isEmpty() ? PaddleOcr::dictPath(tier)
                                              : PaddleOcr::scriptDictPath(script);
    QFile file(dictFile);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = PaddleOcr::tr("Cannot read OCR dictionary %1: %2")
                     .arg(file.fileName(), file.errorString());
        return false;
    }
    const QByteArray raw = file.readAll();
    if (script.isEmpty()) {
        QByteArray text = raw;
        if (text.endsWith('\n'))
            text.chop(1);
        const QList<QByteArray> lines = text.split('\n');
        dict.reserve(lines.size());
        for (QByteArray line : lines) {
            if (line.endsWith('\r'))
                line.chop(1);
            dict.append(QString::fromUtf8(line));
        }
    } else {
        dict = PaddleOcr::parseDictYml(raw, error);
        if (dict.size() != scriptModel->entries) {
            *error = PaddleOcr::tr("The OCR dictionary %1 has %2 characters, expected %3")
                         .arg(dictFile)
                         .arg(dict.size())
                         .arg(scriptModel->entries);
            return false;
        }
    }
    if (dict.isEmpty()) {
        *error = PaddleOcr::tr("OCR dictionary %1 is empty").arg(dictFile);
        return false;
    }
    loadedKey = key;
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

RunOut PaddleOcr::Impl::run(const QImage &img, const QString &tier, const QString &script,
                           bool barcodeFirst)
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
    if (!ensureLoaded(tier, script, &r.error))
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
    // one. The recognizer's own space token already sets spaceBefore. This runs
    // in VISUAL order, before an RTL line is reversed.
    for (int i = 1; i < r.words.size(); ++i) {
        OcrWord &w = r.words[i];
        const OcrWord &prev = r.words[i - 1];
        if (w.line == prev.line && !w.spaceBefore
            && w.rect.left() - prev.rect.right() > std::max(4, prev.rect.height() / 2))
            w.spaceBefore = true;
    }
    // The Arabic recognizer returns the line in visual order. Reverse each
    // line's words and keep the box attached to its glyph, so the joined text
    // is logical and matches the glyph order the result window stores.
    if (script == QLatin1String("arabic")) {
        int start = 0;
        while (start < r.words.size()) {
            int end = start + 1;
            while (end < r.words.size() && r.words[end].line == r.words[start].line)
                ++end;
            QVector<OcrWord> lineWords = r.words.mid(start, end - start);
            PaddleOcr::reorderRtlWords(lineWords);
            for (int i = 0; i < lineWords.size(); ++i)
                r.words[start + i] = lineWords[i];
            start = end;
        }
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

QString PaddleOcr::bundledTier()
{
    // Read the qrc, not a constant: on Windows nothing is embedded (the bytes
    // would be dead weight, PaddleOcr::available() is false there) and this
    // has to come back empty rather than promise files that are not there.
    const QString id = QString::fromLatin1(kBundledTier);
    return bundledComplete(id) ? id : QString();
}

bool PaddleOcr::bundledComplete(const QString &tier)
{
    for (const FilePin &f : tierPin(tier)->files) {
        const QFileInfo fi(bundledPath(tier, f.name));
        if (!fi.exists() || fi.size() != f.bytes)
            return false;
    }
    return true;
}

bool PaddleOcr::downloadedComplete(const QString &tier)
{
    for (const FilePin &f : tierPin(tier)->files) {
        const QFileInfo fi(modelDir(tier) + QLatin1Char('/') + QLatin1String(f.name));
        if (!fi.exists() || fi.size() != f.bytes)
            return false;
    }
    return true;
}

QString PaddleOcr::modelDir(const QString &tier)
{
    return modelRoot() + QLatin1Char('/') + tier;
}

QString PaddleOcr::modelFilePath(const QString &tier, int index)
{
    // One rule for every reader: a copy embedded in the binary wins over the
    // data directory. For tiny that is always the qrc (an older install's
    // downloaded copy is unused, and deleteDownloads() still removes it); for
    // small and medium there is no qrc file, so this is the downloaded path and
    // nothing changes.
    const FilePin &f = tierPin(tier)->files[index];
    const QString embedded = bundledPath(tier, f.name);
    if (QFileInfo::exists(embedded))
        return embedded;
    return modelDir(tier) + QLatin1Char('/') + QLatin1String(f.name);
}

QString PaddleOcr::detPath(const QString &tier)
{
    return modelFilePath(tier, 0);
}

QString PaddleOcr::recPath(const QString &tier)
{
    return modelFilePath(tier, 1);
}

QString PaddleOcr::dictPath(const QString &tier)
{
    return modelFilePath(tier, 2);
}

QStringList PaddleOcr::scriptIds()
{
    QStringList ids;
    for (const ScriptPin &p : kScripts)
        ids << QString::fromLatin1(p.id);
    return ids;
}

QString PaddleOcr::scriptName(const QString &script)
{
    if (script == QLatin1String("korean"))
        return tr("Korean");
    if (script == QLatin1String("arabic"))
        return tr("Arabic");
    if (script == QLatin1String("cyrillic"))
        return tr("Cyrillic");
    if (script == QLatin1String("thai"))
        return tr("Thai");
    if (script == QLatin1String("devanagari"))
        return tr("Devanagari");
    if (script == QLatin1String("tamil"))
        return tr("Tamil");
    if (script == QLatin1String("telugu"))
        return tr("Telugu");
    if (script == QLatin1String("greek"))
        return tr("Greek");
    return script;
}

QString PaddleOcr::scriptDir(const QString &script)
{
    // Only known ids produce a path: an id from QML or a hand-edited setting
    // must never become a "../" path.
    if (!scriptPin(script))
        return {};
    return modelRoot() + QStringLiteral("/scripts/") + script;
}

QString PaddleOcr::scriptRecPath(const QString &script)
{
    const ScriptPin *pin = scriptPin(script);
    return pin ? scriptDir(script) + QLatin1Char('/') + QLatin1String(pin->files[0].name)
               : QString();
}

QString PaddleOcr::scriptDictPath(const QString &script)
{
    const ScriptPin *pin = scriptPin(script);
    return pin ? scriptDir(script) + QLatin1Char('/') + QLatin1String(pin->files[1].name)
               : QString();
}

QString PaddleOcr::scriptDownloadUrl(const QString &script, const QString &fileName)
{
    const ScriptPin *pin = scriptPin(script);
    if (!pin)
        return {};
    for (const ScriptFilePin &file : pin->files)
        if (fileName == QLatin1String(file.name))
            return scriptFileUrl(*pin, file);
    return {};
}

bool PaddleOcr::scriptInstalled(const QString &script) const
{
    const ScriptPin *pin = scriptPin(script);
    if (!pin)
        return false;
    const QFileInfo rec(scriptRecPath(script));
    const QFileInfo dict(scriptDictPath(script));
    return rec.exists() && rec.size() == pin->files[0].bytes && dict.exists()
           && dict.size() == pin->files[1].bytes;
}

bool PaddleOcr::hasInstalledScripts() const
{
    for (const ScriptPin &p : kScripts)
        if (scriptInstalled(QString::fromLatin1(p.id)))
            return true;
    return false;
}

QVariantList PaddleOcr::scriptStates() const
{
    QVariantList out;
    const QLocale loc;
    for (const ScriptPin &p : kScripts) {
        const QString id = QString::fromLatin1(p.id);
        const bool rec = QFileInfo(scriptRecPath(id)).size() == p.files[0].bytes
                         && QFileInfo::exists(scriptRecPath(id));
        const bool dict = QFileInfo::exists(scriptDictPath(id))
                          && QFileInfo(scriptDictPath(id)).size() == p.files[1].bytes;
        const bool installed = rec && dict;
        bool partial = false;
        for (const ScriptFilePin &f : p.files) {
            const QString path = scriptDir(id) + QLatin1Char('/') + QLatin1String(f.name);
            if (QFileInfo::exists(path) || QFileInfo::exists(path + QStringLiteral(".part")))
                partial = true;
        }
        QVariantMap entry;
        entry.insert(QStringLiteral("id"), id);
        entry.insert(QStringLiteral("name"), scriptName(id));
        // Stable, untranslated tokens; the wording lives in QML.
        entry.insert(QStringLiteral("state"), installed ? QStringLiteral("downloaded")
                                         : partial   ? QStringLiteral("partial")
                                                     : QStringLiteral("missing"));
        entry.insert(QStringLiteral("installed"), installed);
        entry.insert(QStringLiteral("bytes"), p.files[0].bytes + p.files[1].bytes);
        entry.insert(QStringLiteral("sizeText"),
                     loc.formattedDataSize(p.files[0].bytes + p.files[1].bytes, 0,
                                           QLocale::DataSizeTraditionalFormat));
        out.append(entry);
    }
    return out;
}

QString PaddleOcr::scriptDownloadSizeText(const QString &script)
{
    const ScriptPin *pin = scriptPin(script);
    if (!pin)
        return {};
    qint64 bytes = 0;
    if (!runtimeFound() && canDownloadRuntime())
        bytes += kRuntimeBytes;
    for (const ScriptFilePin &f : pin->files) {
        const QFileInfo fi(scriptDir(script) + QLatin1Char('/') + QLatin1String(f.name));
        if (!fi.exists() || fi.size() != f.bytes)
            bytes += f.bytes;
    }
    return QLocale().formattedDataSize(bytes, 0, QLocale::DataSizeTraditionalFormat);
}

QString PaddleOcr::scriptForTesseractLanguages(const QString &langs)
{
    // Every Tesseract code that maps to one PP-OCRv5 recognizer. Anything not
    // here (a Latin language, Hebrew, Bengali, vertical Korean, grc, ...) makes
    // the spec ineligible: one rec cannot read the whole page.
    static const QHash<QString, QString> codeToScript = {
        {QStringLiteral("ara"), QStringLiteral("arabic")},
        {QStringLiteral("fas"), QStringLiteral("arabic")},
        {QStringLiteral("urd"), QStringLiteral("arabic")},
        {QStringLiteral("pus"), QStringLiteral("arabic")},
        {QStringLiteral("uig"), QStringLiteral("arabic")},
        {QStringLiteral("snd"), QStringLiteral("arabic")},
        {QStringLiteral("ckb"), QStringLiteral("arabic")},
        {QStringLiteral("kor"), QStringLiteral("korean")},
        {QStringLiteral("rus"), QStringLiteral("cyrillic")},
        {QStringLiteral("ukr"), QStringLiteral("cyrillic")},
        {QStringLiteral("bul"), QStringLiteral("cyrillic")},
        {QStringLiteral("srp"), QStringLiteral("cyrillic")},
        {QStringLiteral("mkd"), QStringLiteral("cyrillic")},
        {QStringLiteral("bel"), QStringLiteral("cyrillic")},
        {QStringLiteral("kaz"), QStringLiteral("cyrillic")},
        {QStringLiteral("kir"), QStringLiteral("cyrillic")},
        {QStringLiteral("mon"), QStringLiteral("cyrillic")},
        {QStringLiteral("tgk"), QStringLiteral("cyrillic")},
        {QStringLiteral("tha"), QStringLiteral("thai")},
        {QStringLiteral("hin"), QStringLiteral("devanagari")},
        {QStringLiteral("mar"), QStringLiteral("devanagari")},
        {QStringLiteral("san"), QStringLiteral("devanagari")},
        {QStringLiteral("nep"), QStringLiteral("devanagari")},
        {QStringLiteral("tam"), QStringLiteral("tamil")},
        {QStringLiteral("tel"), QStringLiteral("telugu")},
        {QStringLiteral("ell"), QStringLiteral("greek")},
    };
    QString script;
    const QStringList parts = langs.split(QLatin1Char('+'), Qt::SkipEmptyParts);
    for (const QString &p : parts) {
        if (p == QLatin1String("eng"))
            continue; // every v5 recognizer carries English
        const auto it = codeToScript.constFind(p);
        if (it == codeToScript.constEnd())
            return {}; // a code one of these recognizers cannot read
        if (!script.isEmpty() && script != it.value())
            return {}; // two scripts in one spec; Tesseract keeps the page
        script = it.value();
    }
    return script;
}

QString PaddleOcr::scriptForOsd(const QString &osdScript)
{
    if (osdScript == QLatin1String("Hangul"))
        return QStringLiteral("korean");
    if (osdScript == QLatin1String("Arabic"))
        return QStringLiteral("arabic");
    if (osdScript == QLatin1String("Cyrillic"))
        return QStringLiteral("cyrillic");
    if (osdScript == QLatin1String("Thai"))
        return QStringLiteral("thai");
    if (osdScript == QLatin1String("Devanagari"))
        return QStringLiteral("devanagari");
    if (osdScript == QLatin1String("Tamil"))
        return QStringLiteral("tamil");
    if (osdScript == QLatin1String("Telugu"))
        return QStringLiteral("telugu");
    if (osdScript == QLatin1String("Greek"))
        return QStringLiteral("greek");
    return {};
}

bool PaddleOcr::scriptSupportsLanguages(const QString &script, const QString &langs)
{
    return !script.isEmpty() && scriptForTesseractLanguages(langs) == script;
}

QString PaddleOcr::reverseRtlText(const QString &text)
{
    // PaddleX pred_reverse(): Latin/digit/space/punctuation runs stay in order,
    // every other character is its own unit, then the units are reversed.
    QStringList units;
    QString run;
    for (const QChar c : text) {
        const ushort u = c.unicode();
        const bool ascii = (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z')
                           || (u >= '0' && u <= '9') || c == QLatin1Char(' ')
                           || c == QLatin1Char(':') || c == QLatin1Char('*')
                           || c == QLatin1Char('.') || c == QLatin1Char('/')
                           || c == QLatin1Char('%') || c == QLatin1Char('+')
                           || c == QLatin1Char('-');
        if (ascii) {
            run += c;
        } else {
            if (!run.isEmpty()) {
                units << run;
                run.clear();
            }
            units << QString(c);
        }
    }
    if (!run.isEmpty())
        units << run;
    std::reverse(units.begin(), units.end());
    return units.join(QString());
}

void PaddleOcr::reorderRtlWords(QVector<OcrWord> &words)
{
    reorderRtlWordsImpl(words);
}

qint64 PaddleOcr::scriptRunMiB(const QString &script)
{
    // Measured with ORT 1.31.1: the PP-OCRv6 tiny det model plus any one v5
    // script rec reaches about 184 MiB RSS on a 1600x1000 page; 320 MiB leaves
    // room for the detection tensors and the scaled copies. The v5 rec is
    // smaller than the v6 small/medium rec it replaces, which is why the guard
    // takes max(tier, script) instead of adding the two. Unknown ids read like
    // a known one, matching tierPin()'s fallback style.
    Q_UNUSED(script);
    return 320;
}

QVector<QString> PaddleOcr::parseDictYml(const QByteArray &raw, QString *error)
{
    const QList<QByteArray> lines = raw.split('\n');
    int start = -1;
    for (int i = 0; i < lines.size(); ++i) {
        QByteArray line = lines[i];
        if (line.endsWith('\r'))
            line.chop(1);
        if (line.trimmed() == QByteArrayLiteral("character_dict:")) {
            start = i + 1;
            break;
        }
    }
    if (start < 0) {
        if (error)
            *error = tr("The OCR dictionary has no character list");
        return {};
    }
    QVector<QString> out;
    for (int i = start; i < lines.size(); ++i) {
        QByteArray line = lines[i];
        if (line.endsWith('\r'))
            line.chop(1);
        if (!line.startsWith(QByteArrayLiteral("  - ")))
            break;
        QByteArray value = line.mid(4);
        if (value.size() >= 2 && value.startsWith('\'') && value.endsWith('\'')) {
            value = value.mid(1, value.size() - 2);
            value.replace("''", "'");
        } else if (value.size() >= 2 && value.startsWith('"') && value.endsWith('"')) {
            value = value.mid(1, value.size() - 2);
            value.replace("\\\"", "\"");
            value.replace("\\\\", "\\");
        }
        out.append(QString::fromUtf8(value));
    }
    if (out.isEmpty()) {
        if (error)
            *error = tr("The OCR dictionary is empty");
    }
    return out;
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
    // The SELECTED tier, from either source: shipped in the binary (tiny) or
    // present in the data directory (any tier). Routing asks effectiveTier().
    return bundledComplete(tier()) || downloadedComplete(tier());
}

QString PaddleOcr::effectiveTier() const
{
    // THE effective-tier rule, in one place (D3): the selected tier when it is
    // installed, otherwise the bundled tiny, otherwise no PP-OCRv6 at all. It is
    // what routing, the memory guard and the loader all follow; Settings still
    // shows the selected tier and says which one is really running.
    const QString selected = tier();
    if (bundledComplete(selected) || downloadedComplete(selected))
        return selected;
    return bundledTier();
}

bool PaddleOcr::tierBundled() const
{
    return bundledComplete(tier());
}

bool PaddleOcr::tierDownloaded() const
{
    return downloadedComplete(tier());
}

QVariantList PaddleOcr::tierStates() const
{
    QVariantList out;
    for (const TierPin &t : kTiers) {
        const QString id = QString::fromLatin1(t.id);
        QVariantMap entry;
        entry.insert(QStringLiteral("id"), id);
        // These tokens are stable and untranslated: the wording lives in QML,
        // so the catalogs translate it like any other string.
        entry.insert(QStringLiteral("state"),
                     bundledComplete(id)      ? QStringLiteral("included")
                     : downloadedComplete(id) ? QStringLiteral("downloaded")
                                              : QStringLiteral("missing"));
        out.append(entry);
    }
    return out;
}

bool PaddleOcr::hasDownloads() const
{
    // Disk only, so the bundled tier never reports files the user could delete.
    // A ".part" counts: a cancelled or interrupted download leaves bytes the
    // user can reclaim with Delete, even though the tier reads as missing.
    const QString dir = modelDir(tier());
    for (const FilePin &f : tierPin(tier())->files) {
        const QString path = dir + QLatin1Char('/') + QLatin1String(f.name);
        if (QFile::exists(path) || QFile::exists(path + QStringLiteral(".part")))
            return true;
    }
    return false;
}

QString PaddleOcr::downloadSizeText()
{
    qint64 bytes = 0;
    if (!runtimeFound() && canDownloadRuntime())
        bytes += kRuntimeBytes;
    // A bundled tier is already inside the binary; download() fetches nothing
    // for it, so the size must not advertise 6 MB that will not be downloaded.
    if (!bundledComplete(tier())) {
        for (const FilePin &f : tierPin(tier())->files) {
            const QFileInfo fi(modelDir(tier()) + QLatin1Char('/') + QLatin1String(f.name));
            if (!fi.exists() || fi.size() != f.bytes)
                bytes += f.bytes;
        }
    }
    return QLocale().formattedDataSize(bytes, 0, QLocale::DataSizeTraditionalFormat);
}

qint64 PaddleOcr::tierRunMiB(const QString &tier)
{
    // What a run needs free before it starts: the loaded pair, measured at
    // ~0.4 / 0.6 / 1.2 GB resident with ORT 1.31.1, plus the detection tensor
    // (a 2000 px side is 48 MB of floats) and the scaled copies of the
    // screenshot. Without the check a 4 GB machine picks medium and gets an
    // opaque ORT allocation failure or the OOM killer instead of a sentence it
    // can act on. Unknown/corrupt ids read as the default tier, like tierPin().
    if (tier == QLatin1String("tiny"))
        return 700;
    if (tier == QLatin1String("medium"))
        return 1800;
    return 1000; // small, the default
}

qint64 PaddleOcr::parseMemAvailableMiB(const QByteArray &meminfo)
{
    // "MemAvailable:   16384000 kB" -> 16000. -1, not 0, when the line is
    // missing or the number is unreadable: 0 would read as "no memory at all"
    // and refuse to run on a kernel that simply does not report it.
    for (const QByteArray &line : meminfo.split('\n')) {
        if (!line.startsWith("MemAvailable:"))
            continue;
        bool ok = false;
        const qint64 kb = line.mid(13).trimmed().split(' ').value(0).toLongLong(&ok);
        return ok && kb > 0 ? kb / 1024 : -1;
    }
    return -1;
}

bool PaddleOcr::tierReadsJapanese(const QString &tier)
{
    // PP-OCRv6 tiny's dictionary has 6,904 entries and no kana at all (small and
    // medium carry 15,565 Han plus the kana), so it cannot read Japanese. Tested
    // with the bundled files: "jpn" must stay on Tesseract until Small or Medium
    // is downloaded.
    return tier != QLatin1String("tiny");
}

bool PaddleOcr::supportsLanguages(const QString &langs, bool autoScript) const
{
    // Routing follows the tier that will actually run, not the one in the
    // picker: with tiny bundled, a fresh install runs tiny while Settings still
    // says Small.
    const QString running = effectiveTier();
    if (running.isEmpty())
        return false;
    if (!autoScript) {
        // A PP-OCRv5 script recognizer is a complete answer for its whole spec
        // only when it is actually installed; otherwise the v6 table answers.
        const QString script = scriptForTesseractLanguages(langs);
        if (!script.isEmpty())
            return scriptInstalled(script);
    }
    return tierSupportsLanguages(running, langs, autoScript);
}

bool PaddleOcr::tierSupportsLanguages(const QString &tier, const QString &langs, bool autoScript)
{
    if (tier.isEmpty())
        return false; // no PP-OCRv6 model at all
    const QStringList parts = langs.split(QLatin1Char('+'), Qt::SkipEmptyParts);
    const bool japanese = parts.contains(QStringLiteral("jpn"))
                          || parts.contains(QStringLiteral("jpn_vert"));
    if (autoScript) {
        // PP-OCRv6 has no script detection (that is Tesseract's osd), so "auto"
        // hands it every installed pack at once. With a tier that cannot read
        // kana and Japanese installed, the page MIGHT be Japanese, so auto stays
        // on Tesseract there instead of misreading it; Latin and Chinese still
        // go to Paddle for an install without a Japanese pack (D3).
        return tierReadsJapanese(tier) || !japanese;
    }
    if (japanese && !tierReadsJapanese(tier))
        return false;
    // PP-OCRv6 reads Chinese (simplified and traditional), Japanese and 48
    // Latin-script languages. Everything else is still Tesseract's job: the v6
    // recogniser would output confident-looking garbage for Cyrillic/Arabic/
    // Korean/Indic/Thai text. Keep this list in step with the scripts in
    // OcrEngine::scriptCodeTable(). "jpn"/"jpn_vert" are handled above, because
    // whether they are readable depends on the tier.
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
        QStringLiteral("chi_tra_vert"),
    };
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
    m_downloadScript.clear();
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
    m_downloadScript.clear();
    if (!runtimeFound()) {
        if (!canDownloadRuntime()) {
            fail(tr("PaddleOCR needs ONNX Runtime. Install the onnxruntime package, then try again."));
            return;
        }
        FetchJob runtime;
        runtime.url = QString::fromLatin1(kRuntimeUrl);
        runtime.dest = runtimeDir() + QStringLiteral("/runtime.tgz");
        runtime.bytes = kRuntimeBytes;
        runtime.sha256 = QByteArray(kRuntimeSha);
        runtime.runtime = true;
        m_queue << runtime;
    }
    const TierPin *pin = tierPin(m_downloadTier);
    // A bundled tier is in the binary already; only the runtime can be missing.
    if (!bundledComplete(m_downloadTier)) {
        for (int i = 0; i < 3; ++i) {
            const QString name = QString::fromLatin1(pin->files[i].name);
            const QString dest = modelDir(m_downloadTier) + QLatin1Char('/') + name;
            const QFileInfo fi(dest);
            if (!fi.exists() || fi.size() != pin->files[i].bytes) {
                FetchJob job;
                job.url = modelsBase() + m_downloadTier + QLatin1Char('/') + name;
                job.dest = dest;
                job.bytes = pin->files[i].bytes;
                job.sha256 = QByteArray(pin->files[i].sha256);
                m_queue << job;
            }
        }
    }
    if (m_queue.isEmpty()) {
        emit stateChanged();
        return;
    }
    m_totalBytes = 0;
    for (const FetchJob &job : std::as_const(m_queue))
        m_totalBytes += job.bytes;
    m_doneBytes = 0;
    m_progress = 0;
    emit progressChanged();
    fetchNext();
}

void PaddleOcr::downloadScript(const QString &script)
{
    if (downloading())
        return;
    const ScriptPin *pin = scriptPin(script);
    if (!pin)
        return;
    m_error.clear();
    m_queue.clear();
    m_cancelled = false;
    m_downloadTier = tier();
    m_downloadScript = script;
    if (!runtimeFound()) {
        if (!canDownloadRuntime()) {
            fail(tr("PaddleOCR needs ONNX Runtime. Install the onnxruntime package, then try again."));
            return;
        }
        FetchJob runtime;
        runtime.url = QString::fromLatin1(kRuntimeUrl);
        runtime.dest = runtimeDir() + QStringLiteral("/runtime.tgz");
        runtime.bytes = kRuntimeBytes;
        runtime.sha256 = QByteArray(kRuntimeSha);
        runtime.runtime = true;
        m_queue << runtime;
    }
    for (const ScriptFilePin &file : pin->files) {
        const QString name = QString::fromLatin1(file.name);
        const QString dest = scriptDir(script) + QLatin1Char('/') + name;
        const QFileInfo fi(dest);
        if (fi.exists() && fi.size() == file.bytes)
            continue;
        FetchJob job;
        job.url = scriptFileUrl(*pin, file);
        job.dest = dest;
        job.bytes = file.bytes;
        job.sha256 = QByteArray(file.sha256);
        m_queue << job;
    }
    if (m_queue.isEmpty()) {
        m_downloadScript.clear();
        emit stateChanged();
        return;
    }
    m_totalBytes = 0;
    for (const FetchJob &job : std::as_const(m_queue))
        m_totalBytes += job.bytes;
    m_doneBytes = 0;
    m_progress = 0;
    emit progressChanged();
    fetchNext();
}

void PaddleOcr::fetchNext()
{
    if (m_queue.isEmpty()) {
        m_progress = 1;
        m_downloadScript.clear();
        m_downloadTier.clear();
        emit progressChanged();
        emit stateChanged();
        return;
    }
    const FetchJob job = m_queue.takeFirst();
    const QString dest = job.dest;
    const qint64 bytes = job.bytes;
    const QByteArray sha = job.sha256;
    // Downloads are written to "<dest>.part" and only renamed into place after
    // the size and SHA-256 verify, so an interrupted transfer is a head start
    // instead of a restart (T6.4). A part at or past the pinned size is stale
    // (a crash after the last byte, or a pin that moved); a re-download is
    // cheaper than untangling it.
    const QString partPath = dest + QStringLiteral(".part");
    qint64 offset = 0;
    if (QFileInfo::exists(partPath)) {
        const qint64 have = QFileInfo(partPath).size();
        if (have > 0 && have < bytes)
            offset = have;
        else
            QFile::remove(partPath);
    }
    auto file = std::make_shared<QFile>(partPath);
    if (!QDir().mkpath(QFileInfo(dest).absolutePath())
            || !file->open(QIODevice::ReadWrite)
            || !file->seek(offset)) {
        fail(tr("Cannot write %1: %2").arg(partPath, file->errorString()));
        return;
    }
    // The kept prefix has to enter the hash too, or the finished file would
    // never verify. A part that cannot be read back is dropped and the request
    // goes out as a plain one from byte 0.
    auto hash = std::make_shared<QCryptographicHash>(QCryptographicHash::Sha256);
    if (offset > 0) {
        bool ok = file->seek(0);
        qint64 left = ok ? offset : 0;
        while (ok && left > 0) {
            const QByteArray prefix = file->read(qMin<qint64>(left, 1 << 20));
            if (prefix.isEmpty()) {
                ok = false;
                break;
            }
            hash->addData(prefix);
            left -= prefix.size();
        }
        if (!ok || !file->seek(offset)) {
            if (!file->resize(0) || !file->seek(0)) {
                file->close();
                fail(tr("Cannot write %1: %2").arg(partPath, file->errorString()));
                return;
            }
            hash->reset();
            offset = 0;
        }
    }
    QNetworkRequest req(QUrl(job.url));
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("Unisic/" UNISIC_VERSION " (screenshot tool)"));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(60000);
    if (offset > 0)
        req.setRawHeader("Range", "bytes=" + QByteArray::number(offset) + "-");
    QNetworkReply *reply = nam()->get(req);
    m_reply = reply;
    emit stateChanged();

    auto received = std::make_shared<qint64>(offset);
    auto resumed = std::make_shared<bool>(offset > 0);
    auto problem = std::make_shared<QString>();
    auto take = [reply, file, hash, received, resumed, problem, bytes] {
        if (*resumed) {
            // Look at the answer once. 206 means the server honoured the Range;
            // anything else (200, a proxy that dropped it, no HTTP at all) means
            // the body starts at byte 0, so the kept prefix is thrown away.
            *resumed = false;
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (status != 206) {
                if (!file->resize(0) || !file->seek(0)) {
                    *problem = tr("Cannot write %1: %2").arg(file->fileName(), file->errorString());
                    return false;
                }
                hash->reset();
                *received = 0;
            }
        }
        const QByteArray chunk = reply->readAll();
        if (chunk.isEmpty())
            return true;
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
    connect(reply, &QNetworkReply::downloadProgress, this, [this, received](qint64, qint64) {
        // received counts the kept prefix, so a resumed file starts where it
        // stopped instead of at 0. A server that ignores the Range resets it.
        m_progress = m_totalBytes > 0
                         ? qMin<qreal>(1.0, qreal(m_doneBytes + *received) / m_totalBytes) : 0;
        emit progressChanged();
    });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, file, hash, received, problem, take, runtime = job.runtime, dest, partPath,
             bytes, sha] {
                reply->deleteLater();
                m_reply = nullptr;
                const QNetworkReply::NetworkError error = reply->error();
                if (error == QNetworkReply::NoError)
                    take();
                if (error != QNetworkReply::NoError || !problem->isEmpty()) {
                    file->close();
                    if (!problem->isEmpty()) {
                        // A rejected answer (too large, unwritable) is not a head
                        // start: drop the part so the next attempt starts clean.
                        QFile::remove(partPath);
                        fail(*problem);
                    } else if (error != QNetworkReply::OperationCanceledError) {
                        // Network error or the 60 s stall guard: keep the part,
                        // the next Download continues from it.
                        fail(tr("Download failed: %1").arg(reply->errorString()));
                    } else if (!m_cancelled) {
                        fail(tr("Download stopped: nothing arrived for a minute"));
                    } else {
                        // User cancel: keep the part for the next attempt.
                        emit stateChanged();
                    }
                    return;
                }
                if (*received != bytes || hash->result().toHex() != sha) {
                    file->close();
                    QFile::remove(partPath);
                    fail(tr("The download does not match the expected file, so it was not kept"));
                    return;
                }
                file->close();
                if (QFileInfo(partPath).size() != bytes) {
                    QFile::remove(partPath);
                    fail(tr("The download does not match the expected file, so it was not kept"));
                    return;
                }
                if (QFileInfo::exists(dest))
                    QFile::remove(dest);
                QFile part(partPath);
                if (!part.rename(dest)) {
                    fail(tr("Cannot write %1: %2").arg(dest, part.errorString()));
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
    m_downloadScript.clear();
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

void PaddleOcr::deleteScriptDownloads(const QString &script)
{
    if (!scriptPin(script))
        return;
    if (m_downloadScript == script && downloading())
        cancelDownload();
    QDir(scriptDir(script)).removeRecursively();
    m_error.clear();
    emit stateChanged();
}

void PaddleOcr::recognize(const QImage &img, Result cb)
{
    if (img.isNull()) {
        cb({}, tr("No image to recognize"));
        return;
    }
    if (!runtimeFound()) {
        cb({}, tr("PaddleOCR needs ONNX Runtime. Install the onnxruntime package, then try again."));
        return;
    }
    // The tier that will actually run, not the one in the picker: with tiny
    // bundled those differ on an install that picked Small and never downloaded
    // it, and both the memory guard and the loader have to follow the real one.
    const QString running = effectiveTier();
    if (running.isEmpty()) {
        cb({}, tr("PaddleOCR models are not downloaded - see Settings > General"));
        return;
    }
    if (const QString memory = memoryError(running); !memory.isEmpty()) {
        cb({}, memory);
        return;
    }
    auto impl = m_impl;
    auto *fw = new QFutureWatcher<RunOut>(this);
    connect(fw, &QFutureWatcher<RunOut>::finished, this, [this, fw, cb] {
        const RunOut r = fw->result();
        fw->deleteLater();
        if (m_idle)
            m_idle->start();
        cb(r.text, r.error);
    });
    fw->setFuture(QtConcurrent::run([impl, img, running] {
        return impl->run(img, running, QString(), true);
    }));
}

void PaddleOcr::recognizeBoxes(const QImage &img, BoxResult cb)
{
    if (img.isNull()) {
        cb({}, tr("No image to recognize"));
        return;
    }
    if (!runtimeFound()) {
        cb({}, tr("PaddleOCR needs ONNX Runtime. Install the onnxruntime package, then try again."));
        return;
    }
    const QString running = effectiveTier();
    if (running.isEmpty()) {
        cb({}, tr("PaddleOCR models are not downloaded - see Settings > General"));
        return;
    }
    if (const QString memory = memoryError(running); !memory.isEmpty()) {
        cb({}, memory);
        return;
    }
    auto impl = m_impl;
    auto *fw = new QFutureWatcher<RunOut>(this);
    connect(fw, &QFutureWatcher<RunOut>::finished, this, [this, fw, cb] {
        const RunOut r = fw->result();
        fw->deleteLater();
        if (m_idle)
            m_idle->start();
        cb(r.words, r.error);
    });
    fw->setFuture(QtConcurrent::run([impl, img, running] {
        return impl->run(img, running, QString(), false);
    }));
}

void PaddleOcr::recognizeScript(const QImage &img, const QString &script, Result cb)
{
    if (img.isNull()) {
        cb({}, tr("No image to recognize"));
        return;
    }
    if (!runtimeFound()) {
        cb({}, tr("PaddleOCR needs ONNX Runtime. Install the onnxruntime package, then try again."));
        return;
    }
    const QString running = effectiveTier();
    if (running.isEmpty()) {
        cb({}, tr("PaddleOCR models are not downloaded - see Settings > General"));
        return;
    }
    if (!scriptInstalled(script)) {
        cb({}, tr("The %1 OCR model is not downloaded - see Settings > General")
                    .arg(scriptName(script)));
        return;
    }
    if (const QString memory = memoryError(running, script); !memory.isEmpty()) {
        cb({}, memory);
        return;
    }
    auto impl = m_impl;
    auto *fw = new QFutureWatcher<RunOut>(this);
    connect(fw, &QFutureWatcher<RunOut>::finished, this, [this, fw, cb] {
        const RunOut r = fw->result();
        fw->deleteLater();
        if (m_idle)
            m_idle->start();
        cb(r.text, r.error);
    });
    fw->setFuture(QtConcurrent::run([impl, img, running, script] {
        return impl->run(img, running, script, true);
    }));
}

void PaddleOcr::recognizeScriptBoxes(const QImage &img, const QString &script, BoxResult cb)
{
    if (img.isNull()) {
        cb({}, tr("No image to recognize"));
        return;
    }
    if (!runtimeFound()) {
        cb({}, tr("PaddleOCR needs ONNX Runtime. Install the onnxruntime package, then try again."));
        return;
    }
    const QString running = effectiveTier();
    if (running.isEmpty()) {
        cb({}, tr("PaddleOCR models are not downloaded - see Settings > General"));
        return;
    }
    if (!scriptInstalled(script)) {
        cb({}, tr("The %1 OCR model is not downloaded - see Settings > General")
                    .arg(scriptName(script)));
        return;
    }
    if (const QString memory = memoryError(running, script); !memory.isEmpty()) {
        cb({}, memory);
        return;
    }
    auto impl = m_impl;
    auto *fw = new QFutureWatcher<RunOut>(this);
    connect(fw, &QFutureWatcher<RunOut>::finished, this, [this, fw, cb] {
        const RunOut r = fw->result();
        fw->deleteLater();
        if (m_idle)
            m_idle->start();
        cb(r.words, r.error);
    });
    fw->setFuture(QtConcurrent::run([impl, img, running, script] {
        return impl->run(img, running, script, false);
    }));
}
