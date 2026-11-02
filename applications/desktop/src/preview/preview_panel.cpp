#include "preview/preview_panel.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>

#include "canvas/canvas_renderer.h"
#include "forma/canvas/display_list.h"
#include "forma/design/design_engine.h"
#include "ui/theme.h"

namespace forma::ui {

PreviewPanel::PreviewPanel(ProjectController* controller, QWidget* parent) : QWidget(parent), controller_(controller)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* header = new QFrame(this);
    header->setProperty("role", "panelHeader");
    auto* bar = new QHBoxLayout(header);
    bar->setContentsMargins(10, 6, 10, 6);
    bar->addWidget(new QLabel(tr("Screen"), header));
    screen_ = new QComboBox(header);
    screen_->setObjectName(QStringLiteral("previewScreen"));
    bar->addWidget(screen_);
    bar->addWidget(new QLabel(tr("Device"), header));
    device_ = new QComboBox(header);
    device_->setObjectName(QStringLiteral("previewDevice"));
    device_->addItem(tr("Design size"), QString());
    for (const auto& preset : design::devicePresets())
        device_->addItem(QStringLiteral("%1 (%2 x %3)").arg(toQ(preset.name)).arg(preset.width).arg(preset.height), toQ(preset.id));
    bar->addWidget(device_);
    bar->addWidget(new QLabel(tr("Appearance"), header));
    appearance_ = new QComboBox(header);
    appearance_->setObjectName(QStringLiteral("previewAppearance"));
    appearance_->addItem(tr("Project theme"), QString());
    appearance_->addItem(tr("Light"), QStringLiteral("light"));
    appearance_->addItem(tr("Dark"), QStringLiteral("dark"));
    bar->addWidget(appearance_);
    bar->addWidget(new QLabel(tr("Zoom"), header));
    zoom_ = new QSlider(Qt::Horizontal, header);
    zoom_->setRange(25, 200);
    zoom_->setValue(100);
    zoom_->setFixedWidth(120);
    bar->addWidget(zoom_);
    bar->addStretch(1);
    run_ = new QPushButton(tr("Build and run the real app"), header);
    run_->setObjectName(QStringLiteral("previewRun"));
    run_->setProperty("role", "primary");
    bar->addWidget(run_);
    layout->addWidget(header);
    notice_ = new QLabel(tr("Simulated preview: this draws your design with FormaIDE's own renderer. Fonts and native controls can differ in the real app."), this);
    notice_->setObjectName(QStringLiteral("previewNotice"));
    notice_->setProperty("role", "muted");
    notice_->setContentsMargins(10, 4, 10, 4);
    notice_->setWordWrap(true);
    layout->addWidget(notice_);
    scroll_ = new QScrollArea(this);
    scroll_->setObjectName(QStringLiteral("previewScroll"));
    scroll_->setAlignment(Qt::AlignCenter);
    scroll_->setFrameShape(QFrame::NoFrame);
    image_view_ = new QLabel(scroll_);
    image_view_->setObjectName(QStringLiteral("previewImage"));
    scroll_->setWidget(image_view_);
    scroll_->setWidgetResizable(false);
    layout->addWidget(scroll_, 1);

    auto update = [this] {
        if (!updating_)
            refresh();
    };
    connect(screen_, &QComboBox::currentIndexChanged, this, update);
    connect(device_, &QComboBox::currentIndexChanged, this, update);
    connect(appearance_, &QComboBox::currentIndexChanged, this, update);
    connect(zoom_, &QSlider::valueChanged, this, update);
    connect(run_, &QPushButton::clicked, this, &PreviewPanel::runRequested);
    connect(controller_, &ProjectController::projectOpened, this, &PreviewPanel::refreshChoices);
    connect(controller_, &ProjectController::modelChanged, this, [this](const ir::ChangeEvent&) {
        if (isVisible())
            refreshChoices();
    });
    connect(controller_, &ProjectController::screenChanged, this, [this] {
        int index = screen_->findData(controller_->currentScreen());
        if (index >= 0)
            screen_->setCurrentIndex(index);
    });
}

void PreviewPanel::setDevice(const QString& id)
{
    device_->setCurrentIndex(std::max(0, device_->findData(id)));
}

void PreviewPanel::setAppearance(const QString& mode)
{
    appearance_->setCurrentIndex(std::max(0, appearance_->findData(mode)));
}

void PreviewPanel::refreshChoices()
{
    updating_ = true;
    QString previous = screen_->currentData().toString();
    screen_->clear();
    if (controller_->hasProject()) {
        for (const auto& screen : controller_->model()->screens)
            screen_->addItem(toQ(screen.name), toQ(screen.root.id));
    }
    int index = screen_->findData(previous.isEmpty() ? controller_->currentScreen() : previous);
    if (index >= 0)
        screen_->setCurrentIndex(index);
    updating_ = false;
    refresh();
}

QImage PreviewPanel::render(const ir::Screen& screen, double width, double height, double scale) const
{
    ir::ApplicationModel model = *controller_->model();
    QString mode = appearance_->currentData().toString();
    if (!mode.isEmpty()) {
        for (const auto& theme : model.themes) {
            if (theme.mode == fromQ(mode)) {
                model.activeTheme = theme.id;
                break;
            }
        }
    }
    QtTextMeasurer measurer;
    layout::LayoutOptions options;
    options.model = &model;
    options.measurer = &measurer;
    if (width > 0)
        options.breakpoint = layout::breakpointForWidth(model, width);
    layout::LayoutResult result;
    if (width > 0)
        result = layout::computeLayout(screen.root, {width, height}, options);
    else
        result = layout::computeScreenLayout(screen, options);
    canvas::DisplayList list = canvas::buildDisplayList(result, model);
    QSize size(static_cast<int>(std::ceil(result.size.width * scale)), static_cast<int>(std::ceil(result.size.height * scale)));
    if (size.isEmpty())
        return {};
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    QPainter painter(&image);
    painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
    painter.scale(scale, scale);
    PainterBackend backend(painter, controller_->rootPath());
    backend.draw(list, {});
    painter.end();
    return image;
}

void PreviewPanel::refresh()
{
    if (!controller_->hasProject())
        return;
    const ir::Screen* screen = controller_->model()->findScreen(fromQ(screen_->currentData().toString()));
    if (screen == nullptr) {
        image_view_->clear();
        image_view_->setText(tr("This project has no screens to preview."));
        image_view_->adjustSize();
        image_ = {};
        return;
    }
    double width = 0;
    double height = 0;
    if (const design::DevicePreset* preset = design::findDevicePreset(fromQ(device_->currentData().toString()))) {
        width = preset->width;
        height = preset->height;
    }
    image_ = render(*screen, width, height, zoom_->value() / 100.0);
    image_view_->setPixmap(QPixmap::fromImage(image_));
    image_view_->adjustSize();
}

}
