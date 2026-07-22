#pragma once

#include <QWidget>
#include <QTimer>
#include <QPixmap>

extern "C" {
#include "obs-grayscale-histogram.h"
}

class GrayscaleHistogramDock : public QWidget {
	Q_OBJECT

    public:
	explicit GrayscaleHistogramDock(QWidget *parent = nullptr);
	~GrayscaleHistogramDock();

protected:
	void paintEvent(QPaintEvent *event) override;
	QSize minimumSizeHint() const override;
	QSize sizeHint() const override;

private:
	void drawBackground(QPainter &painter);
	void drawBars(QPainter &painter, const struct histogram_data &data);

	QPixmap backgroundCache_;
	bool backgroundDirty_ = true;
};

extern "C" void create_histogram_dock(void);