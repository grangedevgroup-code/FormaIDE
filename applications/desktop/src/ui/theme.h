#pragma once

#include <QColor>
#include <QFont>
#include <QHash>
#include <QIcon>
#include <QObject>
#include <QString>

class QApplication;

namespace forma::ui {

struct ThemeColors {
    QColor window;
    QColor panel;
    QColor panelRaised;
    QColor editor;
    QColor canvas;
    QColor border;
    QColor borderStrong;
    QColor text;
    QColor textMuted;
    QColor textFaint;
    QColor accent;
    QColor accentHover;
    QColor accentText;
    QColor hover;
    QColor pressed;
    QColor selection;
    QColor selectionText;
    QColor danger;
    QColor warning;
    QColor success;
    QColor generated;
};

class Theme : public QObject {
    Q_OBJECT

public:
    static Theme& instance();

    void apply(QApplication& application);
    void setMode(const QString& mode);
    QString mode() const { return mode_; }
    bool isDark() const { return dark_; }
    const ThemeColors& colors() const { return colors_; }
    QFont codeFont(int pointSize = 0) const;
    QIcon icon(const QString& name, const QColor& tint = QColor()) const;
    QPixmap pixmap(const QString& name, int size, const QColor& tint = QColor(), qreal ratio = 1.0) const;
    QString styleSheet() const;

Q_SIGNALS:
    void changed();

private:
    Theme() = default;
    void refresh();

    QString mode_ = QStringLiteral("system");
    mutable QHash<QString, QIcon> iconCache_;
    bool dark_ = false;
    ThemeColors colors_;
    QApplication* application_ = nullptr;
};

inline const ThemeColors& colors()
{
    return Theme::instance().colors();
}

inline QIcon icon(const QString& name)
{
    return Theme::instance().icon(name);
}

}
