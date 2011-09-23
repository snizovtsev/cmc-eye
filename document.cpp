#include "document.h"

#include <QtCore>
#include <QColor>

Document::Document(QObject *parent)
    : QObject(parent), m_image(), m_source(), m_modified(false), m_selection(-1, -1, -1, -1)
{
}

void Document::setModified(bool modified)
{
    if (m_modified != modified) {
        m_modified = modified;
        emit modifiedChanged();
    }
}

void Document::setSource(const QString& filename)
{
    if (!m_image.load(filename))
        return;

    setSelection(m_image.rect());
    setModified(false);

    m_source = filename;
    emit sourceChanged();

    emit repaint(m_image.rect());
}

bool Document::save(const QString& filename)
{
    if (m_image.save(filename)) {
        if (m_source != filename) {
            m_source = filename;
            emit sourceChanged();
        }

        setModified(false);
        return true;
    }
    return false;
}

void Document::setSelection(QRect selection)
{
    m_selection = selection.isValid() ?
                selection.intersected(m_image.rect()) :
                m_image.rect();
    emit selectionChanged();
}

/*
  Contrast editing functions
 */

class ConcurrentMapLine {
    QImage& m_image;
    const QPoint m_position;
    const int m_length;
    const Document::PixelMapper& m_mapper;
public:
    ConcurrentMapLine(QImage& image, QPoint position, int length, const Document::PixelMapper& mapper)
        : m_image(image), m_position(position), m_length(length), m_mapper(mapper) { }
    const ConcurrentMapLine& operator =(const ConcurrentMapLine& other) { return other; }
    void map() {
        QRgb* data = reinterpret_cast <QRgb*> (m_image.scanLine(m_position.y()));
        data += m_position.x();
        QPoint cur = m_position;
        for (int i = 0; i < m_length; ++i, ++data, ++cur.rx())
            *data = m_mapper(*data, cur);
    }
    static void map(ConcurrentMapLine& line) { line.map(); }
};

void Document::concurrentMap(const PixelMapper& func)
{
    QList <ConcurrentMapLine> lines;
    for (int i = m_selection.top(); i <= m_selection.bottom(); ++i)
        lines.append(ConcurrentMapLine(m_image, QPoint(m_selection.left(), i), m_selection.width(), func));
    QtConcurrent::blockingMap(lines, ConcurrentMapLine::map);

    setModified(true);
    emit repaint(m_selection);
}

struct ChannelCorrector {
    virtual uchar correct(uchar value) const = 0;
    uchar operator()(uchar value) const { return correct(value); }
};

class LinearCorrector: public ChannelCorrector {
    uchar m_low, m_high;
public:
    LinearCorrector(uchar low, uchar high): m_low (low), m_high(high) { }
    uchar correct(uchar value) const {
        if (value <= m_low)
            return 0;
        if (value >= m_high)
            return 255;
        return (uchar) ((value - m_low) * 255.0 / (m_high - m_low));
    }
};

struct RedCorrector: Document::PixelMapper {
    const ChannelCorrector& m_corrector;
    explicit RedCorrector(const ChannelCorrector& corrector): m_corrector(corrector) { }
    QRgb map(QRgb pixel, QPoint) const {
        return qRgb(m_corrector(qRed(pixel)), qGreen(pixel), qBlue(pixel));
    }
};

struct GreenCorrector: Document::PixelMapper {
    const ChannelCorrector& m_corrector;
    explicit GreenCorrector(const ChannelCorrector& corrector): m_corrector(corrector) { }
    QRgb map(QRgb pixel, QPoint) const {
        return qRgb(qRed(pixel), m_corrector(qGreen(pixel)), qBlue(pixel));
    }
};

struct BlueCorrector: Document::PixelMapper {
    const ChannelCorrector& m_corrector;
    explicit BlueCorrector(const ChannelCorrector& corrector): m_corrector(corrector) { }
    QRgb map(QRgb pixel, QPoint) const {
        return qRgb(qRed(pixel), qGreen(pixel), m_corrector(qBlue(pixel)));
    }
};

struct ValueCorrector: Document::PixelMapper {
    const ChannelCorrector& m_corrector;
    explicit ValueCorrector(const ChannelCorrector& corrector): m_corrector(corrector) { }
    QRgb map(QRgb pixel, QPoint) const {
        QColor color(pixel);
        int h, s, v;
        color.getHsv(&h, &s, &v);
        color.setHsv(h, s, m_corrector(v));
        return color.rgb();
    }
};

void Document::adjustContrast(uchar low, uchar high, QString channel)
{
    qDebug() << "LinearCorrection(" << low << "," << high << "," << channel << ")";

    if (channel == "red")
        concurrentMap(RedCorrector(LinearCorrector(low, high)));
    else if (channel == "green")
        concurrentMap(GreenCorrector(LinearCorrector(low, high)));
    else if (channel == "blue")
        concurrentMap(BlueCorrector(LinearCorrector(low, high)));
    else if (channel == "value")
        concurrentMap(ValueCorrector(LinearCorrector(low, high)));
    else {
        qCritical() << "Unknown channel name" << channel;
        return;
    }
}

/*
  Translation-based effects
 */

struct FloatRgb {
    qreal r, g, b, a;
    FloatRgb(qreal _r, qreal _g, qreal _b, qreal _a)
        : r(_r), g(_g), b(_b), a(_a) { }
    FloatRgb(QRgb color)
        : r(qRed(color)), g(qGreen(color)), b(qBlue(color)), a(qAlpha(color)) { }
    inline FloatRgb operator+ (const FloatRgb& other)
    {
        return FloatRgb(r + other.r, g + other.g, b + other.b, a + other.a);
    }
    inline FloatRgb operator* (qreal k)
    {
        return FloatRgb(r * k, g * k, b * k, a * k);
    }
    inline operator QRgb()
    {
        return qRgba((int) r, (int) g, (int) b, (int) a);
    }
};

inline static FloatRgb safeGetPixel(const QImage &image, int x, int y)
{
    return image.valid(x, y) ? image.pixel(x, y) : qRgba(0, 0, 0, 0);
}

inline static QRgb interpolate(const QImage &orig, const QPointF &pixel)
{
    int x = (int) floor(pixel.x());
    int y = (int) floor(pixel.y());
    //return safeGetPixel(orig, x, y); // non-interpolated

    qreal dx = pixel.x() - x;
    qreal dy = pixel.y() - y;
    qreal dxdy = dx * dy;
    return  safeGetPixel(orig, x, y) * (1 - dx - dy + dxdy) +
            safeGetPixel(orig, x+1, y) * (dx - dxdy) +
            safeGetPixel(orig, x, y+1) * (dy - dxdy) +
            safeGetPixel(orig, x+1, y+1) * dxdy;
}

struct TranslateMapper: Document::PixelMapper {
    const Document::PixelTranslator& translate;
    const QImage& orig;
    TranslateMapper(const QImage& original, const Document::PixelTranslator &translator)
        : translate (translator), orig(original) { }
    QRgb map(QRgb, QPoint point) const {
        return interpolate(orig, translate(point));
    }
};

void Document::translatePixels(const PixelTranslator &func)
{
    QImage original = m_image.copy();
    concurrentMap(TranslateMapper(original, func));
}

struct WaveEffect: Document::PixelTranslator {
    QPointF translate(QPoint point) const {
        // x(k; l) = k + 20sin(2pil / 128); y(k; l) = l;
        return QPointF(point.x() + 20 * sin(2 * M_PI * point.y() / 128),
                       point.y());
    }
};

void Document::waveEffect()
{
    translatePixels(WaveEffect());
}


struct Rotation: Document::PixelTranslator {
    const qreal x0, y0;
    const qreal cos_mu, sin_mu;

    Rotation(qreal x, qreal y, qreal angle)
        : x0(x), y0(y), cos_mu(cos(angle)), sin_mu(sin(angle)) { }

    QPointF translate(QPoint point) const {
        return QPointF((point.x() - x0) * cos_mu - (point.y() - y0) * sin_mu + x0,
                       (point.x() - x0) * sin_mu + (point.y() - y0) * cos_mu + y0);
    }
};


void Document::rotate(qreal x, qreal y, qreal angle)
{
    qDebug() << "rotate(" << x << "," << y << "," << angle << ")";
    translatePixels(Rotation(x, y, angle));
}
