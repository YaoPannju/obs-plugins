#include "grayscale-histogram-dock.hpp"

#include <QPainter>
#include <QPaintEvent>
#include <QApplication>

#include <obs-module.h>
#include <obs-frontend-api.h>

GrayscaleHistogramDock::GrayscaleHistogramDock(QWidget *parent)
    : QWidget(parent)
{
	setAttribute(Qt::WA_OpaquePaintEvent);
	setMinimumSize(280, 140);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

	auto *timer = new QTimer(this);
	timer->setTimerType(Qt::PreciseTimer);
	timer->start(16);
	connect(timer, &QTimer::timeout, this, [this]() {
	    update();
	});
}

GrayscaleHistogramDock::~GrayscaleHistogramDock() {}

QSize GrayscaleHistogramDock::minimumSizeHint() const
{
	return QSize(280, 140);
}

QSize GrayscaleHistogramDock::sizeHint() const
{
	return QSize(320, 160);
}

void GrayscaleHistogramDock::paintEvent(QPaintEvent *)
{
	/* 从共享状态读取直方图数据 */
	struct histogram_data data;
	pthread_mutex_lock(&g_histogram_state.mutex);
	data = g_histogram_state.data;
	pthread_mutex_unlock(&g_histogram_state.mutex);

	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing, false);

	int w = width();
	int h = height();

	/* 背景缓存：只在尺寸变化时重画 */
	if (backgroundDirty_ || backgroundCache_.size() != size()) {
		backgroundCache_ = QPixmap(size() * devicePixelRatioF());
		backgroundCache_.setDevicePixelRatio(devicePixelRatioF());
		backgroundCache_.fill(palette().color(QPalette::Window));
		QPainter bp(&backgroundCache_);
		drawBackground(bp);
		backgroundDirty_ = false;
	}
	painter.drawPixmap(0, 0, backgroundCache_);

	/* 画直方图柱子 */
	if (data.valid && data.total_pixels > 0)
		drawBars(painter, data);

	Q_UNUSED(w);
	Q_UNUSED(h);
}

void GrayscaleHistogramDock::drawBackground(QPainter &p)
{
	const int margin = 12;
	const int labelH = 16;
	QRect r(margin, margin, width() - margin * 2, height() - margin - labelH);

	/* 底色 */
	p.fillRect(r, QColor(30, 30, 30));

	/* 边框 */
	p.setPen(QColor(80, 80, 80));
	p.drawRect(r);

	/* 横向网格线 25% 50% 75% */
	p.setPen(QColor(45, 45, 45));
	for (int i = 1; i <= 3; i++) {
		int y = r.top() + r.height() * i / 4;
		p.drawLine(r.left(), y, r.right(), y);
	}

	/* 底部标签 */
	QFont f = font();
	f.setPixelSize(10);
	p.setFont(f);
	p.setPen(QColor(160, 160, 160));
	p.drawText(r.left(), r.bottom() + 13, "0");
	p.drawText(r.right() - 24, r.bottom() + 13, "255");

	QString mid = "128";
	int midW = p.fontMetrics().horizontalAdvance(mid);
	p.drawText(r.left() + r.width() / 2 - midW / 2, r.bottom() + 13, mid);
}

void GrayscaleHistogramDock::drawBars(QPainter &p,
				       const struct histogram_data &data)
{
	const int margin = 12;
	const int labelH = 16;
	QRect r(margin, margin, width() - margin * 2, height() - margin - labelH);

	int barW = r.width();
	int plotH = r.height();

	/* 找最大值 */
	uint32_t maxVal = 1;
	for (int i = 0; i < HISTOGRAM_BINS; i++)
		if (data.bins[i] > maxVal)
			maxVal = data.bins[i];

	p.setPen(QColor(200, 200, 200));

	if (barW >= HISTOGRAM_BINS) {
		/* 宽度够：一像素一桶 */
		for (int i = 0; i < HISTOGRAM_BINS; i++) {
			int h = (int)((uint64_t)data.bins[i] * plotH / maxVal);
			if (h > 0)
				p.drawLine(r.left() + i, r.bottom(),
					   r.left() + i, r.bottom() - h);
		}
	} else {
		/* 宽度不够：多桶合并到一像素 */
		int binsPerPx = (HISTOGRAM_BINS + barW - 1) / barW;
		for (int x = 0; x < barW; x++) {
			uint32_t groupMax = 0;
			int start = x * binsPerPx;
			int end = start + binsPerPx;
			for (int b = start; b < end && b < HISTOGRAM_BINS; b++)
				if (data.bins[b] > groupMax)
					groupMax = data.bins[b];
			int h = (int)((uint64_t)groupMax * plotH / maxVal);
			if (h > 0)
				p.drawLine(r.left() + x, r.bottom(),
					   r.left() + x, r.bottom() - h);
		}
	}

	/* 右上角统计信息 */
	p.setPen(QColor(140, 220, 140));
	QFont f = font();
	f.setPixelSize(10);
	p.setFont(f);
	QString info = QString("pixels:%1  max:%2")
			   .arg(data.total_pixels)
			   .arg(maxVal);
	p.drawText(r.right() - p.fontMetrics().horizontalAdvance(info),
		   r.top() - 4, info);
}

extern "C" void create_histogram_dock(void)
{
	obs_frontend_push_ui_translation(obs_module_get_string);

	auto *dock = new GrayscaleHistogramDock();
	obs_frontend_add_dock_by_id(
	    "grayscale_histogram_dock",
	    obs_module_text("GrayscaleHistogram"),
	    (void *)dock);

	obs_frontend_pop_ui_translation();
}