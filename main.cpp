#include <image_downloader.hpp>
#include <osc.hpp>
#include <QFile>
#include <QMessageBox>
#include <QProgressDialog>
#include <face_inference.hpp>
#include <main_window.hpp>
#include <QApplication>
#include <QThread>
#include <updater.hpp>


int main(int argc, char *argv[]) {
    system("chcp 65001");
    // Create ui application
    QApplication app(argc, argv);
    QFile qssFile("./resources/material.qss"); // 使用资源路径
    QIcon icon("./resources/window_icon.png");
    if (qssFile.open(QFile::ReadOnly)) {
        QString styleSheet = QLatin1String(qssFile.readAll());
        app.setStyleSheet(styleSheet);
        qssFile.close();
    } else {
        QMessageBox box;
        box.setWindowIcon(icon);
        box.setText(QObject::tr("无法打开 QSS 文件"));
        box.exec();
    }

    PaperTrackerMainWindow window;
    window.setWindowIcon(icon);  // 设置窗口图标
    window.show();

    int status = QApplication::exec();
    return status;
}