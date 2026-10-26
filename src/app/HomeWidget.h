#pragma once

#include <QObject>
#include <QTimer>

namespace sc {

// Android's home-screen widget (packaging/android/.../HomeWidget.java):
// Continue reading and watching, and how many new chapters wait. After the
// library changes (a few seconds later, in the background) the widget's
// data is written to files/widget.json with small copies of the covers,
// and the widget is told to draw again. Its rows open scancore://continue/
// <series>, which Main resumes. Elsewhere it does nothing.
class HomeWidget : public QObject {
    Q_OBJECT

public:
    static HomeWidget *instance();
    void start();

private:
    explicit HomeWidget(QObject *parent = nullptr);
    void write();

    QTimer m_later;
    bool m_writing = false;
    bool m_again = false;
};

} // namespace sc
