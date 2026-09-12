#ifndef TOGGLESWITCH_H
#define TOGGLESWITCH_H

#include <QAbstractButton>

// A small iOS/Android-style pill toggle (green when checked, dark gray track
// otherwise), used throughout the "Game controls" panel in place of a plain
// QCheckBox to match the Figma design. Fully driven by QAbstractButton's own
// checkable/checked state, so setChecked()/isChecked()/toggled() all work
// exactly like any other checkable button.
class ToggleSwitch : public QAbstractButton
{
    Q_OBJECT
public:
    explicit ToggleSwitch(QWidget *parent = nullptr);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    static constexpr int kWidth = 40;
    static constexpr int kHeight = 22;
};

#endif // TOGGLESWITCH_H
