#pragma once

#include <QImage>
#include <QScrollArea>
#include <QWidget>

#include "app/project_controller.h"

class QComboBox;
class QLabel;
class QPushButton;
class QSlider;

namespace forma::ui {

class PreviewPanel : public QWidget {
    Q_OBJECT

public:
    explicit PreviewPanel(ProjectController* controller, QWidget* parent = nullptr);

    void refresh();
    QImage lastImage() const { return image_; }
    void setDevice(const QString& id);
    void setAppearance(const QString& mode);

Q_SIGNALS:
    void runRequested();

private:
    QImage render(const ir::Screen& screen, double width, double height, double scale) const;
    void refreshChoices();

    ProjectController* controller_;
    QComboBox* screen_;
    QComboBox* device_;
    QComboBox* appearance_;
    QSlider* zoom_;
    QLabel* notice_;
    QLabel* image_view_;
    QScrollArea* scroll_;
    QPushButton* run_;
    QImage image_;
    bool updating_ = false;
};

}
