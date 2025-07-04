//
// Created by JellyfishKnight on 25-3-11.
//
/*
 * PaperTracker - 面部追踪应用程序
 * Copyright (C) 2025 PAPER TRACKER
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * This file contains code from projectbabble:
 * Copyright 2023 Sameer Suri
 * Licensed under the Apache License, Version 2.0
 */

#include <opencv2/imgproc.hpp>
#include "face_tracker_window.hpp"
#include "ui_face_tracker_window.h"
#include <QMessageBox>
#include <codecvt>
#include <locale>
#include <windows.h>
#include <QTimer>
#include <QProcess>
#include <QCoreApplication>
#include <roi_event.hpp>
#include <QInputDialog>

PaperFaceTrackerWindow::PaperFaceTrackerWindow(QWidget *parent)
    : QWidget(parent)
{
    if (instance == nullptr)
        instance = this;
    else
        throw std::exception("当前已经打开了面捕窗口，请不要重复打开");
    // 基本UI设置
    setFixedSize(848, 538);
    ui.setupUi(this);
    setWindowFlags(windowFlags() | Qt::WindowMinimizeButtonHint);
    // 创建超链接标签
    QLabel* tutorialLink = new QLabel(ui.page);
    tutorialLink->setGeometry(QRect(550, 250, 200, 41)); // 增加宽度以适应更大的字体
    tutorialLink->setText("<a href='https://fcnk6r4c64fa.feishu.cn/wiki/VSlnw4Zr0iVzXFkvT8TcbQFMn7c' style='color: #0066cc; font-size: 14pt; font-weight: bold;'>面捕调整教程</a>");
    tutorialLink->setOpenExternalLinks(true); // 允许打开外部链接
    tutorialLink->setTextFormat(Qt::RichText); // 使用富文本格式显示
    tutorialLink->setStyleSheet("background-color: #f0f0f0; padding: 5px; border-radius: 5px;"); // 添加背景色和内边距
    ui.LogText->setMaximumBlockCount(200);
    append_log_window(ui.LogText);
    LOG_INFO("系统初始化中...");
    // 初始化串口连接状态
    ui.SerialConnectLabel->setText(tr("有线模式未连接"));
    ui.WifiConnectLabel->setText(tr("无线模式未连接"));
    // 初始化页面导航
    bound_pages();

    // 初始化亮度控制相关成员
    current_brightness = 0;
    // 连接信号和槽
    connect_callbacks();
    // 添加输入框焦点事件处理
    ui.SSIDText->installEventFilter(this);
    ui.PasswordText->installEventFilter(this);
    // 允许Tab键在输入框之间跳转
    ui.SSIDText->setTabChangesFocus(true);
    ui.PasswordText->setTabChangesFocus(true);
    // 清除所有控件的初始焦点，确保没有文本框自动获得焦点
    setFocus();
    config_writer = std::make_shared<ConfigWriter>("./config.json");

    // 添加ROI事件
    auto *roiFilter = new ROIEventFilter([this] (QRect rect, bool isEnd, int tag)
    {
        int x = rect.x ();
        int y = rect.y ();
        int width = rect.width ();
        int height = rect.height ();

        // 规范化宽度和高度为正值
        if (width < 0) {
            x += width;
            width = -width;
        }
        if (height < 0) {
            y += height;
            height = -height;
        }

        // 裁剪坐标到图像边界内
        if (x < 0) {
            width += x;  // 减少宽度
            x = 0;       // 将 x 设为 0
        }
        if (y < 0) {
            height += y; // 减少高度
            y = 0;       // 将 y 设为 0
        }

        // 确保 ROI 不超出图像边界
        if (x + width > 280) {
            width = 280 - x;
        }
        if (y + height > 280) {
            height = 280 - y;
        }
        // 确保最终的宽度和高度为正值
        width = max(0, width);
        height = max(0, height);

        // 更新 roi_rect
        roi_rect.is_roi_end = isEnd;
        roi_rect = Rect (x, y, width, height);
    },ui.ImageLabel);
    ui.ImageLabel->installEventFilter(roiFilter);
    ui.ImageLabelCal->installEventFilter(roiFilter);
    inference = std::make_shared<FaceInference>();
    osc_manager = std::make_shared<OscManager>();
    set_config();
    // Load model
    LOG_INFO("正在加载推理模型...");
    try {
        inference->load_model("");
        LOG_INFO("模型加载完成");
    } catch (const std::exception& e) {
        // 使用Qt方式记录日志，而不是minilog
        LOG_ERROR("错误: 模型加载异常: {}", e.what());
    }
    // 初始化OSC管理器
    LOG_INFO("正在初始化OSC...");
    if (osc_manager->init("127.0.0.1", 8888)) {
        osc_manager->setLocationPrefix("");
        LOG_INFO("OSC初始化成功");
    } else {
        LOG_ERROR("OSC初始化失败，请检查网络连接");
    }
    // 初始化串口和wifi
    serial_port_manager = std::make_shared<SerialPortManager>();
    image_downloader = std::make_shared<ESP32VideoStream>();
    LOG_INFO("初始化有线模式");
    serial_port_manager->init();
    // init serial port manager
    serial_port_manager->registerCallback(
        PACKET_DEVICE_STATUS,
        [this](const std::string& ip, int brightness, int power, int version) {
            if (version != 1)
            {
                static bool version_warning = false;
                QString version_str = version == 2 ? "左眼追" : "右眼追";
                if (!version_warning)
                {
                    QMessageBox msgBox;
                    msgBox.setWindowIcon(this->windowIcon());
                    msgBox.setText(tr("检测到") + version_str + tr("设备，请打开眼追界面进行设置"));
                    msgBox.exec();
                    version_warning = true;
                }
                serial_port_manager->stop();
                return ;
            }
            // 使用Qt的线程安全方式更新UI
            QMetaObject::invokeMethod(this, [ip, brightness, power, version, this]() {
                // 只在 IP 地址变化时更新显示
                if (current_ip_ != "http://" + ip)
                {
                    current_ip_ = "http://" + ip;
                    // 更新IP地址显示，添加 http:// 前缀
                    this->setIPText(QString::fromStdString(current_ip_));
                    LOG_INFO("IP地址已更新: {}", current_ip_);
                    start_image_download();
                }
                firmware_version = std::to_string(version);
                // 可以添加其他状态更新的日志，如果需要的话
            }, Qt::QueuedConnection);
        }
    );
    showSerialData = false; // 确保初始状态为false
    serial_port_manager->registerRawDataCallback([this](const std::string& data) {
        if (showSerialData) {
            serialRawDataLog.push_back(data);
            LOG_INFO("串口原始数据: {}", data);
        }
    });
    LOG_DEBUG("等待有线模式面捕连接");
    while (serial_port_manager->status() == SerialStatus::CLOSED) {}
    LOG_DEBUG("有线模式面捕连接完毕");

    if (serial_port_manager->status() == SerialStatus::FAILED)
    {
        setSerialStatusLabel("有线模式面捕连接失败");
        LOG_WARN("有线模式面捕未连接，尝试从配置文件中读取地址...");
        if (!config.wifi_ip.empty())
        {
            LOG_INFO("从配置文件中读取地址成功");
            current_ip_ = config.wifi_ip;
            start_image_download();
        } else
        {
            QMessageBox msgBox;
            msgBox.setWindowIcon(this->windowIcon());
            msgBox.setText(tr("未找到配置文件信息，请将面捕通过数据线连接到电脑进行首次配置"));
            msgBox.exec();
        }
    } else
    {
        LOG_INFO("有线模式面捕连接成功");
        setSerialStatusLabel("有线模式面捕连接成功");
    }
    // 读取配置文件
    set_config();
    setupKalmanFilterControls();
    create_sub_threads();
    // 创建自动保存配置的定时器
    auto_save_timer = new QTimer(this);
    connect(auto_save_timer, &QTimer::timeout, this, [this]() {
        config = generate_config();
        config_writer->write_config(config);
        LOG_DEBUG("面捕配置已自动保存");
    });
    auto_save_timer->start(10000); // 10000毫秒 = 10秒
}

void PaperFaceTrackerWindow::setVideoImage(const cv::Mat& image)
{
    if (image.empty())
    {
        QMetaObject::invokeMethod(this, [this]()
        {
            if (ui.stackedWidget->currentIndex() == 0) {
                ui.ImageLabel->clear(); // 清除图片
                ui.ImageLabel->setText(tr("                         没有图像输入")); // 恢复默认文本
            } else if (ui.stackedWidget->currentIndex() == 1) {
                ui.ImageLabelCal->clear(); // 清除图片
                ui.ImageLabelCal->setText(tr("                         没有图像输入"));
            }
        }, Qt::QueuedConnection);
        return ;
    }
    QMetaObject::invokeMethod(this, [this, image = image.clone()]() {
        auto qimage = QImage(image.data, image.cols, image.rows, image.step, QImage::Format_RGB888);
        auto pix_map = QPixmap::fromImage(qimage);
        if (ui.stackedWidget->currentIndex() == 0)
        {
            ui.ImageLabel->setPixmap(pix_map);
            ui.ImageLabel->setScaledContents(true);
            ui.ImageLabel->update();
        } else if (ui.stackedWidget->currentIndex() == 1) {
            ui.ImageLabelCal->setPixmap(pix_map);
            ui.ImageLabelCal->setScaledContents(true);
            ui.ImageLabelCal->update();
        }
    }, Qt::QueuedConnection);
}

PaperFaceTrackerWindow::~PaperFaceTrackerWindow() {
    stop();
    if (auto_save_timer) {
        auto_save_timer->stop();
        delete auto_save_timer;
        auto_save_timer = nullptr;
    }
    config = generate_config();
    config_writer->write_config(config);
    LOG_INFO("正在关闭VRCFT");
    remove_log_window(ui.LogText);
    instance = nullptr;

}

void PaperFaceTrackerWindow::bound_pages() {
    // 页面导航逻辑
    connect(ui.MainPageButton, &QPushButton::clicked, [this] {
        ui.stackedWidget->setCurrentIndex(0);
    });
    connect(ui.CalibrationPageButton, &QPushButton::clicked, [this] {
        ui.stackedWidget->setCurrentIndex(1);
    });
}

// 添加事件过滤器实现
bool PaperFaceTrackerWindow::eventFilter(QObject *obj, QEvent *event)
{
    // 处理焦点获取事件
    if (event->type() == QEvent::FocusIn) {
        if (obj == ui.SSIDText) {
            if (ui.SSIDText->toPlainText() == "请输入WIFI名字（仅支持2.4ghz）") {
                ui.SSIDText->setPlainText("");
            }
        } else if (obj == ui.PasswordText) {
            if (ui.PasswordText->toPlainText() == "请输入WIFI密码") {
                ui.PasswordText->setPlainText("");
            }
        }
    }

    // 处理焦点失去事件
    if (event->type() == QEvent::FocusOut) {
        if (obj == ui.SSIDText) {
            if (ui.SSIDText->toPlainText().isEmpty()) {
                ui.SSIDText->setPlainText("请输入WIFI名字（仅支持2.4ghz）");
            }
        } else if (obj == ui.PasswordText) {
            if (ui.PasswordText->toPlainText().isEmpty()) {
                ui.PasswordText->setPlainText("请输入WIFI密码");
            }
        }
    }

    // 继续事件处理
    return QWidget::eventFilter(obj, event);
}

// 根据模型输出更新校准页面的进度条
void PaperFaceTrackerWindow::updateCalibrationProgressBars(
    const std::vector<float>& output,
    const std::unordered_map<std::string, size_t>& blendShapeIndexMap
) {
    if (output.empty() || ui.stackedWidget->currentIndex() != 1) {
        // 如果输出为空或者当前不在校准页面，则不更新
        return;
    }

    // 使用Qt的线程安全方式更新UI
    QMetaObject::invokeMethod(this, [this, output, &blendShapeIndexMap]() {
        // 将值缩放到0-100范围内用于进度条显示
        auto scaleValue = [](const float value) -> int {
            // 将值限制在0-1.0范围内，然后映射到0-100
            return static_cast<int>(value * 100);
        };

        // 更新各个进度条
        // 注意：这里假设输出数组中的索引与ARKit模型输出的顺序一致

        // 脸颊
        if (blendShapeIndexMap.contains("cheekPuffLeft") && blendShapeIndexMap.at("cheekPuffLeft") < output.size()) {
            ui.CheekPullLeftValue->setValue(scaleValue(output[blendShapeIndexMap.at("cheekPuffLeft")]));
        }
        if (blendShapeIndexMap.contains("cheekPuffRight") && blendShapeIndexMap.at("cheekPuffRight") < output.size()) {
            ui.CheekPullRightValue->setValue(scaleValue(output[blendShapeIndexMap.at("cheekPuffRight")]));
        }
        // 下巴
        if (blendShapeIndexMap.contains("jawOpen") && blendShapeIndexMap.at("jawOpen") < output.size()) {
            ui.JawOpenValue->setValue(scaleValue(output[blendShapeIndexMap.at("jawOpen")]));
        }
        if (blendShapeIndexMap.contains("jawLeft") && blendShapeIndexMap.at("jawLeft") < output.size()) {
            ui.JawLeftValue->setValue(scaleValue(output[blendShapeIndexMap.at("jawLeft")]));
        }
        if (blendShapeIndexMap.contains("jawRight") && blendShapeIndexMap.at("jawRight") < output.size()) {
            ui.JawRightValue->setValue(scaleValue(output[blendShapeIndexMap.at("jawRight")]));
        }
        // 嘴巴
        if (blendShapeIndexMap.contains("mouthLeft") && blendShapeIndexMap.at("mouthLeft") < output.size()) {
            ui.MouthLeftValue->setValue(scaleValue(output[blendShapeIndexMap.at("mouthLeft")]));
        }
        if (blendShapeIndexMap.contains("mouthRight") && blendShapeIndexMap.at("mouthRight") < output.size()) {
            ui.MouthRightValue->setValue(scaleValue(output[blendShapeIndexMap.at("mouthRight")]));
        }
        // 舌头
        if (blendShapeIndexMap.contains("tongueOut") && blendShapeIndexMap.at("tongueOut") < output.size()) {
            ui.TongueOutValue->setValue(scaleValue(output[blendShapeIndexMap.at("tongueOut")]));
        }
        if (blendShapeIndexMap.contains("tongueUp") && blendShapeIndexMap.at("tongueUp") < output.size()) {
            ui.TongueUpValue->setValue(scaleValue(output[blendShapeIndexMap.at("tongueUp")]));
        }
        if (blendShapeIndexMap.contains("tongueDown") && blendShapeIndexMap.at("tongueDown") < output.size()) {
            ui.TongueDownValue->setValue(scaleValue(output[blendShapeIndexMap.at("tongueDown")]));
        }
        if (blendShapeIndexMap.contains("tongueLeft") && blendShapeIndexMap.at("tongueLeft") < output.size()) {
            ui.TongueLeftValue->setValue(scaleValue(output[blendShapeIndexMap.at("tongueLeft")]));
        }
        if (blendShapeIndexMap.contains("tongueRight") && blendShapeIndexMap.at("tongueRight") < output.size()) {
            ui.TongueRightValue->setValue(scaleValue(output[blendShapeIndexMap.at("tongueRight")]));
        }
        if (blendShapeIndexMap.contains("mouthClose") && blendShapeIndexMap.at("mouthClose") < output.size()) {
            ui.MouthCloseValue->setValue(scaleValue(output[blendShapeIndexMap.at("mouthClose")]));
        }
        if (blendShapeIndexMap.contains("mouthFunnel") && blendShapeIndexMap.at("mouthFunnel") < output.size()) {
            ui.MouthFunnelValue->setValue(scaleValue(output[blendShapeIndexMap.at("mouthFunnel")]));
        }
        if (blendShapeIndexMap.contains("mouthPucker") && blendShapeIndexMap.at("mouthPucker") < output.size()) {
            ui.MouthPuckerValue->setValue(scaleValue(output[blendShapeIndexMap.at("mouthPucker")]));
        }
        if (blendShapeIndexMap.contains("mouthRollUpper") && blendShapeIndexMap.at("mouthRollUpper") < output.size()) {
            ui.MouthRollUpperValue->setValue(scaleValue(output[blendShapeIndexMap.at("mouthRollUpper")]));
        }
        if (blendShapeIndexMap.contains("mouthRollLower") && blendShapeIndexMap.at("mouthRollLower") < output.size()) {
            ui.MouthRollLowerValue->setValue(scaleValue(output[blendShapeIndexMap.at("mouthRollLower")]));
        }
        if (blendShapeIndexMap.contains("mouthShrugUpper") && blendShapeIndexMap.at("mouthShrugUpper") < output.size()) {
            ui.MouthShrugUpperValue->setValue(scaleValue(output[blendShapeIndexMap.at("mouthShrugUpper")]));
        }
        if (blendShapeIndexMap.contains("mouthShrugLower") && blendShapeIndexMap.at("mouthShrugLower") < output.size()) {
            ui.MouthShrugLowerValue->setValue(scaleValue(output[blendShapeIndexMap.at("mouthShrugLower")]));
        }
    }, Qt::QueuedConnection);
}

void PaperFaceTrackerWindow::connect_callbacks()
{
    brightness_timer = std::make_shared<QTimer>();
    brightness_timer->setSingleShot(true);
    connect(brightness_timer.get(), &QTimer::timeout, this, &PaperFaceTrackerWindow::onSendBrightnessValue);
    // functions
    connect(ui.BrightnessBar, &QScrollBar::valueChanged, this, &PaperFaceTrackerWindow::onBrightnessChanged);
    connect(ui.RotateImageBar, &QScrollBar::valueChanged, this, &PaperFaceTrackerWindow::onRotateAngleChanged);
    connect(ui.restart_Button, &QPushButton::clicked, this, &PaperFaceTrackerWindow::onRestartButtonClicked);
    connect(ui.FlashFirmwareButton, &QPushButton::clicked, this, &PaperFaceTrackerWindow::onFlashButtonClicked);
    connect(ui.UseFilterBox, &QCheckBox::checkStateChanged, this, &PaperFaceTrackerWindow::onUseFilterClicked);
    connect(ui.wifi_send_Button, &QPushButton::clicked, this, &PaperFaceTrackerWindow::onSendButtonClicked);
    connect(ui.EnergyModeBox, &QComboBox::currentIndexChanged, this, &PaperFaceTrackerWindow::onEnergyModeChanged);
    connect(ui.JawOpenBar, &QScrollBar::valueChanged, this, &PaperFaceTrackerWindow::onJawOpenChanged);
    connect(ui.JawLeftBar, &QScrollBar::valueChanged, this, &PaperFaceTrackerWindow::onJawLeftChanged);
    connect(ui.JawRightBar, &QScrollBar::valueChanged, this, &PaperFaceTrackerWindow::onJawRightChanged);
    connect(ui.MouthLeftBar, &QScrollBar::valueChanged, this, &PaperFaceTrackerWindow::onMouthLeftChanged);
    connect(ui.MouthRightBar, &QScrollBar::valueChanged, this, &PaperFaceTrackerWindow::onMouthRightChanged);
    connect(ui.TongueOutBar, &QScrollBar::valueChanged, this, &PaperFaceTrackerWindow::onTongueOutChanged);
    connect(ui.TongueLeftBar, &QScrollBar::valueChanged, this, &PaperFaceTrackerWindow::onTongueLeftChanged);
    connect(ui.TongueRightBar, &QScrollBar::valueChanged, this, &PaperFaceTrackerWindow::onTongueRightChanged);
    connect(ui.TongueUpBar, &QScrollBar::valueChanged, this, &PaperFaceTrackerWindow::onTongueUpChanged);
    connect(ui.TongueDownBar, &QScrollBar::valueChanged, this, &PaperFaceTrackerWindow::onTongueDownChanged);
    connect(ui.CheekPuffLeftBar, &QScrollBar::valueChanged, this, &PaperFaceTrackerWindow::onCheekPuffLeftChanged);
    connect(ui.CheekPuffRightBar, &QScrollBar::valueChanged, this, &PaperFaceTrackerWindow::onCheekPuffRightChanged);
    connect(ui.ShowSerialDataButton, &QPushButton::clicked, this, &PaperFaceTrackerWindow::onShowSerialDataButtonClicked);
    connect(ui.CheekPuffLeftOffset, &QLineEdit::editingFinished,
                this, &PaperFaceTrackerWindow::onCheekPuffLeftOffsetChanged);
    connect(ui.CheekPuffRightOffset, &QLineEdit::editingFinished,
            this, &PaperFaceTrackerWindow::onCheekPuffRightOffsetChanged);
    connect(ui.JawOpenOffset, &QLineEdit::editingFinished,
            this, &PaperFaceTrackerWindow::onJawOpenOffsetChanged);
    connect(ui.TongueOutOffset, &QLineEdit::editingFinished,
            this, &PaperFaceTrackerWindow::onTongueOutOffsetChanged);
    // 在connect_callbacks()函数中现有连接代码之后添加以下内容
    connect(ui.MouthCloseBar, &QScrollBar::valueChanged, this, &PaperFaceTrackerWindow::onMouthCloseChanged);
    connect(ui.MouthFunnelBar, &QScrollBar::valueChanged, this, &PaperFaceTrackerWindow::onMouthFunnelChanged);
    connect(ui.MouthPuckerBar, &QScrollBar::valueChanged, this, &PaperFaceTrackerWindow::onMouthPuckerChanged);
    connect(ui.MouthRollUpperBar, &QScrollBar::valueChanged, this, &PaperFaceTrackerWindow::onMouthRollUpperChanged);
    connect(ui.MouthRollLowerBar, &QScrollBar::valueChanged, this, &PaperFaceTrackerWindow::onMouthRollLowerChanged);
    connect(ui.MouthShrugUpperBar, &QScrollBar::valueChanged, this, &PaperFaceTrackerWindow::onMouthShrugUpperChanged);
    connect(ui.MouthShrugLowerBar, &QScrollBar::valueChanged, this, &PaperFaceTrackerWindow::onMouthShrugLowerChanged);

    connect(ui.MouthCloseOffset, &QLineEdit::editingFinished, this, &PaperFaceTrackerWindow::onMouthCloseOffsetChanged);
    connect(ui.MouthFunnelOffset, &QLineEdit::editingFinished, this, &PaperFaceTrackerWindow::onMouthFunnelOffsetChanged);
    connect(ui.MouthPuckerOffset, &QLineEdit::editingFinished, this, &PaperFaceTrackerWindow::onMouthPuckerOffsetChanged);
    connect(ui.MouthRollUpperOffset, &QLineEdit::editingFinished, this, &PaperFaceTrackerWindow::onMouthRollUpperOffsetChanged);
    connect(ui.MouthRollLowerOffset, &QLineEdit::editingFinished, this, &PaperFaceTrackerWindow::onMouthRollLowerOffsetChanged);
    connect(ui.MouthShrugUpperOffset, &QLineEdit::editingFinished, this, &PaperFaceTrackerWindow::onMouthShrugUpperOffsetChanged);
    connect(ui.MouthShrugLowerOffset, &QLineEdit::editingFinished, this, &PaperFaceTrackerWindow::onMouthShrugLowerOffsetChanged);
}

float PaperFaceTrackerWindow::getRotateAngle() const
{
    auto rotate_angle = static_cast<float>(current_rotate_angle);
    rotate_angle = rotate_angle / (static_cast<float>(ui.RotateImageBar->maximum()) -
        static_cast<float>(ui.RotateImageBar->minimum())) * 360.0f;
    return rotate_angle;
}


void PaperFaceTrackerWindow::setOnUseFilterClickedFunc(FuncWithVal func)
{
    onUseFilterClickedFunc = std::move(func);
}

void PaperFaceTrackerWindow::setSerialStatusLabel(const QString& text) const
{
    ui.SerialConnectLabel->setText(tr(text.toUtf8().constData()));
}

void PaperFaceTrackerWindow::setWifiStatusLabel(const QString& text) const
{
    ui.WifiConnectLabel->setText(text.toUtf8().constData());
}

void PaperFaceTrackerWindow::setIPText(const QString& text) const
{
    ui.textEdit->setText(tr(text.toUtf8().constData()));
}

QPlainTextEdit* PaperFaceTrackerWindow::getLogText() const
{
    return ui.LogText;
}

Rect PaperFaceTrackerWindow::getRoiRect()
{
    return roi_rect;
}

std::string PaperFaceTrackerWindow::getSSID() const
{
    return ui.SSIDText->toPlainText().toStdString();
}

std::string PaperFaceTrackerWindow::getPassword() const
{
    return ui.PasswordText->toPlainText().toStdString();
}

void PaperFaceTrackerWindow::onSendButtonClicked()
{
    // onSendButtonClickedFunc();
    // 获取SSID和密码
    auto ssid = getSSID();
    auto password = getPassword();

    // 输入验证
    if (ssid == "请输入WIFI名字（仅支持2.4ghz）" || ssid.empty()) {
        QMessageBox::warning(this, tr("输入错误"), tr("请输入有效的WIFI名字"));
        return;
    }

    if (password == "请输入WIFI密码" || password.empty()) {
        QMessageBox::warning(this, tr("输入错误"), tr("请输入有效的密码"));
        return;
    }

    // 构建并发送数据包
    LOG_INFO("已发送WiFi配置: SSID = {}, PWD = {}", ssid, password);
    LOG_INFO("等待数据被发送后开始自动重启ESP32...");
    serial_port_manager->sendWiFiConfig(ssid, password);

    QTimer::singleShot(3000, this, [this] {
        // 3秒后自动重启ESP32
        onRestartButtonClicked();
    });
}

void PaperFaceTrackerWindow::onRestartButtonClicked()
{
    serial_port_manager->stop_heartbeat_timer();
    image_downloader->stop_heartbeat_timer();
    serial_port_manager->restartESP32(this);
    serial_port_manager->start_heartbeat_timer();
    image_downloader->stop();
    image_downloader->start();
    image_downloader->start_heartbeat_timer();
}

void PaperFaceTrackerWindow::onUseFilterClicked(int value) const
{
    QTimer::singleShot(10, this, [this, value] {
        inference->set_use_filter(value);
    });
}

void PaperFaceTrackerWindow::onFlashButtonClicked()
{
    // 弹出固件选择对话框
    QStringList firmwareTypes;
    firmwareTypes << "普通版面捕固件 (face_tracker.bin)"
                  << "旧版面捕固件 (old_face_tracker.bin)"
                  << "轻薄板面捕固件 (light_face_tracker.bin)";

    bool ok;
    QString selectedType = QInputDialog::getItem(this, "选择固件类型",
                                                "请选择要烧录的固件类型:",
                                                firmwareTypes, 0, false, &ok);
    if (!ok || selectedType.isEmpty()) {
        // 用户取消操作
        return;
    }

    // 根据选择设置固件类型
    std::string firmwareType;
    if (selectedType.contains("普通版面捕固件")) {
        firmwareType = "face_tracker";
    } else if (selectedType.contains("旧版面捕固件")) {
        firmwareType = "old_face_tracker";
    } else if (selectedType.contains("轻薄板面捕固件")) {
        firmwareType = "light_face_tracker";
    }

    LOG_INFO("用户选择烧录固件类型: {}", firmwareType);

    serial_port_manager->stop_heartbeat_timer();
    image_downloader->stop_heartbeat_timer();
    serial_port_manager->flashESP32(this, firmwareType);
    serial_port_manager->start_heartbeat_timer();
    image_downloader->stop();
    image_downloader->start();
    image_downloader->start_heartbeat_timer();
}

void PaperFaceTrackerWindow::onBrightnessChanged(int value) {
    // 更新当前亮度值
    current_brightness = value;

    // 更新值后可以安全返回，即使系统未准备好也先保存用户设置
    if (!serial_port_manager || !brightness_timer) {
        return;
    }

    // 只在设备正确连接状态下才发送命令
    if (serial_port_manager->status() == SerialStatus::OPENED) {
        brightness_timer->start(100);
    } else {
        QMessageBox::warning(this, "警告", "面捕设备未连接，请先连接设备");
    }
}
void PaperFaceTrackerWindow::onRotateAngleChanged(int value)
{
    current_rotate_angle = value;
}

void PaperFaceTrackerWindow::onSendBrightnessValue() const
{
    // 发送亮度控制命令 - 确保亮度值为三位数字
    std::string brightness_str = std::to_string(current_brightness);
    // 补齐三位数字，前面加0
    while (brightness_str.length() < 3) {
        brightness_str = std::string("0") + brightness_str;
    }
    std::string packet = "A6" + brightness_str + "B6";
    serial_port_manager->write_data(packet);
    // 记录操作
    LOG_INFO("已设置亮度: {}", current_brightness);
}

bool PaperFaceTrackerWindow::is_running() const
{
    return app_is_running;
}

void PaperFaceTrackerWindow::stop()
{
    LOG_INFO("正在关闭系统...");
    app_is_running = false;
    if (update_thread.joinable())
    {
        update_thread.join();
    }
    if (inference_thread.joinable())
    {
        inference_thread.join();
    }
    if (osc_send_thread.joinable())
    {
        osc_send_thread.join();
    }
    if (brightness_timer) {
        brightness_timer->stop();
        brightness_timer.reset();
    }
    serial_port_manager->stop();
    image_downloader->stop();
    inference.reset();
    osc_manager->close();
    // 其他清理工作
    LOG_INFO("系统已安全关闭");
}

void PaperFaceTrackerWindow::onEnergyModeChanged(int index)
{
    if (index == 0)
    {
        max_fps = 38;
    } else if (index == 1)
    {
        max_fps = 15;
    } else if (index == 2)
    {
        max_fps = 70;
    }
}

int PaperFaceTrackerWindow::get_max_fps() const
{
    return max_fps;
}

PaperFaceTrackerConfig PaperFaceTrackerWindow::generate_config() const
{
    PaperFaceTrackerConfig res_config;
    res_config.brightness = current_brightness;
    res_config.rotate_angle = current_rotate_angle;
    res_config.energy_mode = ui.EnergyModeBox->currentIndex();
    res_config.use_filter = ui.UseFilterBox->isChecked();
    res_config.wifi_ip = ui.textEdit->toPlainText().toStdString();
    res_config.amp_map = {
        {"cheekPuffLeft", ui.CheekPuffLeftBar->value()},
        {"cheekPuffRight", ui.CheekPuffRightBar->value()},
        {"jawOpen", ui.JawOpenBar->value()},
        {"jawLeft", ui.JawLeftBar->value()},
        {"jawRight", ui.JawRightBar->value()},
        {"mouthLeft", ui.MouthLeftBar->value()},
        {"mouthRight", ui.MouthRightBar->value()},
        {"tongueOut", ui.TongueOutBar->value()},
        {"tongueUp", ui.TongueUpBar->value()},
        {"tongueDown", ui.TongueDownBar->value()},
        {"tongueLeft", ui.TongueLeftBar->value()},
        {"tongueRight", ui.TongueRightBar->value()},
    };
    // 添加偏置值
    res_config.cheek_puff_left_offset = cheek_puff_left_offset;
    res_config.cheek_puff_right_offset = cheek_puff_right_offset;
    res_config.jaw_open_offset = jaw_open_offset;
    res_config.tongue_out_offset = tongue_out_offset;
    res_config.rect = roi_rect;

    // 添加卡尔曼滤波参数
    res_config.dt = current_dt;
    res_config.q_factor = current_q_factor;
    res_config.r_factor = current_r_factor;
    res_config.mouth_close_offset = mouth_close_offset;
    res_config.mouth_funnel_offset = mouth_funnel_offset;
    res_config.mouth_pucker_offset = mouth_pucker_offset;
    res_config.mouth_roll_upper_offset = mouth_roll_upper_offset;
    res_config.mouth_roll_lower_offset = mouth_roll_lower_offset;
    res_config.mouth_shrug_upper_offset = mouth_shrug_upper_offset;
    res_config.mouth_shrug_lower_offset = mouth_shrug_lower_offset;
    return res_config;
}

void PaperFaceTrackerWindow::set_config()
{
    config = config_writer->get_config<PaperFaceTrackerConfig>();
    current_brightness = config.brightness;
    current_rotate_angle = config.rotate_angle == 0 ? 540 : config.rotate_angle;
    ui.BrightnessBar->setValue(config.brightness);
    ui.RotateImageBar->setValue(config.rotate_angle);
    ui.EnergyModeBox->setCurrentIndex(config.energy_mode);
    ui.UseFilterBox->setChecked(config.use_filter);
    ui.textEdit->setPlainText(QString::fromStdString(config.wifi_ip));

    // 设置偏置值
    cheek_puff_left_offset = config.cheek_puff_left_offset;
    cheek_puff_right_offset = config.cheek_puff_right_offset;
    jaw_open_offset = config.jaw_open_offset;
    tongue_out_offset = config.tongue_out_offset;

    // 设置新的偏置值（安全方式，避免使用未初始化的值）
    mouth_close_offset = 0.0f;  // 默认值
    mouth_funnel_offset = 0.0f;
    mouth_pucker_offset = 0.0f;
    mouth_roll_upper_offset = 0.0f;
    mouth_roll_lower_offset = 0.0f;
    mouth_shrug_upper_offset = 0.0f;
    mouth_shrug_lower_offset = 0.0f;
    // 加载卡尔曼滤波参数 - 这里可能缺少了这部分
    current_dt = config.dt;
    current_q_factor = config.q_factor;
    current_r_factor = config.r_factor;

    // 更新UI显示
    if (dtLineEdit) dtLineEdit->setText(QString::number(current_dt, 'f', 3));
    if (qFactorLineEdit) qFactorLineEdit->setText(QString::number(current_q_factor, 'f', 2));
    if (rFactorLineEdit) rFactorLineEdit->setText(QString::number(current_r_factor, 'f', 6));

    // 设置输入框的文本
    ui.CheekPuffLeftOffset->setText(QString::number(cheek_puff_left_offset));
    ui.CheekPuffRightOffset->setText(QString::number(cheek_puff_right_offset));
    ui.JawOpenOffset->setText(QString::number(jaw_open_offset));
    ui.TongueOutOffset->setText(QString::number(tongue_out_offset));

    // 设置新增的输入框文本
    ui.MouthCloseOffset->setText(QString::number(mouth_close_offset));
    ui.MouthFunnelOffset->setText(QString::number(mouth_funnel_offset));
    ui.MouthPuckerOffset->setText(QString::number(mouth_pucker_offset));
    ui.MouthRollUpperOffset->setText(QString::number(mouth_roll_upper_offset));
    ui.MouthRollLowerOffset->setText(QString::number(mouth_roll_lower_offset));
    ui.MouthShrugUpperOffset->setText(QString::number(mouth_shrug_upper_offset));
    ui.MouthShrugLowerOffset->setText(QString::number(mouth_shrug_lower_offset));

    // 更新偏置值到推理引擎
    updateOffsetsToInference();

    try
    {
        ui.CheekPuffLeftBar->setValue(config.amp_map.at("cheekPuffLeft"));
        ui.CheekPuffRightBar->setValue(config.amp_map.at("cheekPuffRight"));
        ui.JawOpenBar->setValue(config.amp_map.at("jawOpen"));
        ui.JawLeftBar->setValue(config.amp_map.at("jawLeft"));
        ui.JawRightBar->setValue(config.amp_map.at("jawRight"));
        ui.MouthLeftBar->setValue(config.amp_map.at("mouthLeft"));
        ui.MouthRightBar->setValue(config.amp_map.at("mouthRight"));
        ui.TongueOutBar->setValue(config.amp_map.at("tongueOut"));
        ui.TongueUpBar->setValue(config.amp_map.at("tongueUp"));
        ui.TongueDownBar->setValue(config.amp_map.at("tongueDown"));
        ui.TongueLeftBar->setValue(config.amp_map.at("tongueLeft"));
        ui.TongueRightBar->setValue(config.amp_map.at("tongueRight"));

        // 新增的滑动条设置（先检查是否存在对应的键，不存在则使用默认值）
        ui.MouthCloseBar->setValue(config.amp_map.count("mouthClose") > 0 ? config.amp_map.at("mouthClose") : 0);
        ui.MouthFunnelBar->setValue(config.amp_map.count("mouthFunnel") > 0 ? config.amp_map.at("mouthFunnel") : 0);
        ui.MouthPuckerBar->setValue(config.amp_map.count("mouthPucker") > 0 ? config.amp_map.at("mouthPucker") : 0);
        ui.MouthRollUpperBar->setValue(config.amp_map.count("mouthRollUpper") > 0 ? config.amp_map.at("mouthRollUpper") : 0);
        ui.MouthRollLowerBar->setValue(config.amp_map.count("mouthRollLower") > 0 ? config.amp_map.at("mouthRollLower") : 0);
        ui.MouthShrugUpperBar->setValue(config.amp_map.count("mouthShrugUpper") > 0 ? config.amp_map.at("mouthShrugUpper") : 0);
        ui.MouthShrugLowerBar->setValue(config.amp_map.count("mouthShrugLower") > 0 ? config.amp_map.at("mouthShrugLower") : 0);
    }
    catch (std::exception& e)
    {
        LOG_ERROR("配置文件中的振幅映射错误: {}", e.what());
    }
    roi_rect = config.rect;
}

void PaperFaceTrackerWindow::onCheekPuffLeftChanged(int value) const
{
    inference->set_amp_map(getAmpMap());
}

void PaperFaceTrackerWindow::onCheekPuffRightChanged(int value) const
{
    inference->set_amp_map(getAmpMap());
}

void PaperFaceTrackerWindow::onJawOpenChanged(int value) const
{
    inference->set_amp_map(getAmpMap());
}

void PaperFaceTrackerWindow::onJawLeftChanged(int value) const
{
    inference->set_amp_map(getAmpMap());
}

void PaperFaceTrackerWindow::onJawRightChanged(int value) const
{
    inference->set_amp_map(getAmpMap());
}

void PaperFaceTrackerWindow::onMouthLeftChanged(int value) const
{
    inference->set_amp_map(getAmpMap());
}

void PaperFaceTrackerWindow::onMouthRightChanged(int value)
{
    inference->set_amp_map(getAmpMap());
}

void PaperFaceTrackerWindow::onTongueOutChanged(int value)
{
    inference->set_amp_map(getAmpMap());
}

void PaperFaceTrackerWindow::onTongueLeftChanged(int value)
{
    inference->set_amp_map(getAmpMap());
}

void PaperFaceTrackerWindow::onTongueRightChanged(int value)
{
    inference->set_amp_map(getAmpMap());
}

void PaperFaceTrackerWindow::onTongueUpChanged(int value) const
{
    inference->set_amp_map(getAmpMap());
}

void PaperFaceTrackerWindow::onTongueDownChanged(int value) const
{
    inference->set_amp_map(getAmpMap());
}
void PaperFaceTrackerWindow::onMouthCloseChanged(int value) const
{
    inference->set_amp_map(getAmpMap());
}

void PaperFaceTrackerWindow::onMouthFunnelChanged(int value) const
{
    inference->set_amp_map(getAmpMap());
}

void PaperFaceTrackerWindow::onMouthPuckerChanged(int value) const
{
    inference->set_amp_map(getAmpMap());
}

void PaperFaceTrackerWindow::onMouthRollUpperChanged(int value) const
{
    inference->set_amp_map(getAmpMap());
}

void PaperFaceTrackerWindow::onMouthRollLowerChanged(int value) const
{
    inference->set_amp_map(getAmpMap());
}

void PaperFaceTrackerWindow::onMouthShrugUpperChanged(int value) const
{
    inference->set_amp_map(getAmpMap());
}

void PaperFaceTrackerWindow::onMouthShrugLowerChanged(int value) const
{
    inference->set_amp_map(getAmpMap());
}

void PaperFaceTrackerWindow::onMouthCloseOffsetChanged()
{
    bool ok;
    float value = ui.MouthCloseOffset->text().toFloat(&ok);
    if (ok) {
        mouth_close_offset = value;
        updateOffsetsToInference();
    }
}

void PaperFaceTrackerWindow::onMouthFunnelOffsetChanged()
{
    bool ok;
    float value = ui.MouthFunnelOffset->text().toFloat(&ok);
    if (ok) {
        mouth_funnel_offset = value;
        updateOffsetsToInference();
    }
}

void PaperFaceTrackerWindow::onMouthPuckerOffsetChanged()
{
    bool ok;
    float value = ui.MouthPuckerOffset->text().toFloat(&ok);
    if (ok) {
        mouth_pucker_offset = value;
        updateOffsetsToInference();
    }
}

void PaperFaceTrackerWindow::onMouthRollUpperOffsetChanged()
{
    bool ok;
    float value = ui.MouthRollUpperOffset->text().toFloat(&ok);
    if (ok) {
        mouth_roll_upper_offset = value;
        updateOffsetsToInference();
    }
}

void PaperFaceTrackerWindow::onMouthRollLowerOffsetChanged()
{
    bool ok;
    float value = ui.MouthRollLowerOffset->text().toFloat(&ok);
    if (ok) {
        mouth_roll_lower_offset = value;
        updateOffsetsToInference();
    }
}

void PaperFaceTrackerWindow::onMouthShrugUpperOffsetChanged()
{
    bool ok;
    float value = ui.MouthShrugUpperOffset->text().toFloat(&ok);
    if (ok) {
        mouth_shrug_upper_offset = value;
        updateOffsetsToInference();
    }
}

void PaperFaceTrackerWindow::onMouthShrugLowerOffsetChanged()
{
    bool ok;
    float value = ui.MouthShrugLowerOffset->text().toFloat(&ok);
    if (ok) {
        mouth_shrug_lower_offset = value;
        updateOffsetsToInference();
    }
}
std::unordered_map<std::string, int> PaperFaceTrackerWindow::getAmpMap() const
{
    return {
            {"cheekPuffLeft", ui.CheekPuffLeftBar->value()},
            {"cheekPuffRight", ui.CheekPuffRightBar->value()},
            {"jawOpen", ui.JawOpenBar->value()},
            {"jawLeft", ui.JawLeftBar->value()},
            {"jawRight", ui.JawRightBar->value()},
            {"mouthLeft", ui.MouthLeftBar->value()},
            {"mouthRight", ui.MouthRightBar->value()},
            {"tongueOut", ui.TongueOutBar->value()},
            {"tongueUp", ui.TongueUpBar->value()},
            {"tongueDown", ui.TongueDownBar->value()},
            {"tongueLeft", ui.TongueLeftBar->value()},
            {"tongueRight", ui.TongueRightBar->value()},
            // 添加新的形态键
            {"mouthClose", ui.MouthCloseBar->value()},
            {"mouthFunnel", ui.MouthFunnelBar->value()},
            {"mouthPucker", ui.MouthPuckerBar->value()},
            {"mouthRollUpper", ui.MouthRollUpperBar->value()},
            {"mouthRollLower", ui.MouthRollLowerBar->value()},
            {"mouthShrugUpper", ui.MouthShrugUpperBar->value()},
            {"mouthShrugLower", ui.MouthShrugLowerBar->value()},
        };
}

void PaperFaceTrackerWindow::start_image_download() const
{
    if (image_downloader->isStreaming())
    {
        image_downloader->stop();
    }
    // 开始下载图片 - 修改为支持WebSocket协议
    // 检查URL格式
    const std::string& url = current_ip_;
    if (url.substr(0, 7) == "http://" || url.substr(0, 8) == "https://" ||
        url.substr(0, 5) == "ws://" || url.substr(0, 6) == "wss://") {
        // URL已经包含协议前缀，直接使用
        image_downloader->init(url, DEVICE_TYPE_FACE);
    } else {
        // 添加默认ws://前缀
        image_downloader->init("ws://" + url, DEVICE_TYPE_FACE);
    }
    image_downloader->start();
}

void PaperFaceTrackerWindow::updateWifiLabel() const
{
    if (image_downloader->isStreaming())
    {
        setWifiStatusLabel("Wifi已连接");
    } else
    {
        setWifiStatusLabel("Wifi连接失败");
    }
}

void PaperFaceTrackerWindow::updateSerialLabel() const
{
    if (serial_port_manager->status() == SerialStatus::OPENED)
    {
        setSerialStatusLabel("面捕有线模式已连接");
    } else
    {
        setSerialStatusLabel("面捕有线模式连接失败");
    }
}

cv::Mat PaperFaceTrackerWindow::getVideoImage() const
{
    return std::move(image_downloader->getLatestFrame());
}

std::string PaperFaceTrackerWindow::getFirmwareVersion() const
{
    return firmware_version;
}

SerialStatus PaperFaceTrackerWindow::getSerialStatus() const
{
    return serial_port_manager->status();
}

void PaperFaceTrackerWindow::set_osc_send_thead(FuncWithoutArgs func)
{
    osc_send_thread = std::thread(std::move(func));
}
void PaperFaceTrackerWindow::updateBatteryStatus() const
{
    if (image_downloader && image_downloader->isStreaming())
    {
        float battery = image_downloader->getBatteryPercentage();
        QString batteryText = QString("电池电量: %1%").arg(battery, 0, 'f', 1);
        ui.BatteryStatusLabel->setText(batteryText);
    }
    else
    {
        ui.BatteryStatusLabel->setText("电池电量: 未知");
    }
}
void PaperFaceTrackerWindow::create_sub_threads()
{
    update_thread = std::thread([this]()
    {
        auto last_time = std::chrono::high_resolution_clock::now();
        double fps_total = 0;
        double fps_count = 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        while (is_running())
        {
            updateWifiLabel();
            updateSerialLabel();
            updateBatteryStatus();
            auto start_time = std::chrono::high_resolution_clock::now();
            try {
                if (fps_total > 1000)
                {
                    fps_count = 0;
                    fps_total = 0;
                }
                // caculate fps
                auto start = std::chrono::high_resolution_clock::now();
                auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(start - last_time);
                last_time = start;
                auto fps = 1000.0 / static_cast<double>(duration.count());
                fps_total += fps;
                fps_count += 1;
                fps = fps_total/fps_count;
                cv::Mat frame = getVideoImage();
                if (!frame.empty())
                {
                    auto rotate_angle = getRotateAngle();
                    cv::resize(frame, frame, cv::Size(280, 280), cv::INTER_NEAREST);
                    int y = frame.rows / 2;
                    int x = frame.cols / 2;
                    auto rotate_matrix = cv::getRotationMatrix2D(cv::Point(x, y), rotate_angle, 1);
                    cv::warpAffine(frame, frame, rotate_matrix, frame.size(), cv::INTER_NEAREST);

                    auto roi_rect = getRoiRect();
                    // 显示图像
                    cv::rectangle(frame, roi_rect.rect, cv::Scalar(0, 255, 0), 2);
                }
                // draw rect on frame
                cv::Mat show_image;
                if (!frame.empty())
                {
                    show_image = frame;
                }
                setVideoImage(show_image);
                // 控制帧率
            } catch (const std::exception& e) {
                // 使用Qt方式记录日志，而不是minilog
                QMetaObject::invokeMethod(this, [&e]() {
                    LOG_ERROR("错误, 视频处理异常: {}", e.what());
                }, Qt::QueuedConnection);
            }
            auto end_time = std::chrono::high_resolution_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count ();
            int delay_ms = max(0, static_cast<int>(1000.0 / min(get_max_fps() + 30, 50) - elapsed));
            // LOG_DEBUG("UIFPS:" +  std::to_string(min(get_max_fps() + 30, 60)));
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        }
    });

    inference_thread = std::thread([this] ()
    {
        auto last_time = std::chrono::high_resolution_clock::now();
        double fps_total = 0;
        double fps_count = 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        while (is_running())
        {
            if (fps_total > 1000)
            {
                fps_count = 0;
                fps_total = 0;
            }
            // calculate fps
            auto start = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(start - last_time);
            last_time = start;
            auto fps = 1000.0 / static_cast<double>(duration.count());
            fps_total += fps;
            fps_count += 1;
            fps = fps_total/fps_count;
            // LOG_DEBUG("模型FPS： {}", fps);

            auto start_time = std::chrono::high_resolution_clock::now();
            // 设置时间序列
            inference->set_dt(duration.count() / 1000.0);

            auto frame = getVideoImage();
            // 推理处理
            if (!frame.empty())
            {
                auto rotate_angle = getRotateAngle();
                cv::resize(frame, frame, cv::Size(280, 280), cv::INTER_NEAREST);
                int y = frame.rows / 2;
                int x = frame.cols / 2;
                auto rotate_matrix = cv::getRotationMatrix2D(cv::Point(x, y), rotate_angle, 1);
                cv::warpAffine(frame, frame, rotate_matrix, frame.size(), cv::INTER_NEAREST);
                cv::Mat infer_frame;
                infer_frame = frame.clone();
                auto roi_rect = getRoiRect();
                if (!roi_rect.rect.empty() && roi_rect.is_roi_end)
                {
                    infer_frame = infer_frame(roi_rect.rect);
                }
                inference->inference(infer_frame);
                {
                    std::lock_guard<std::mutex> lock(outputs_mutex);
                    outputs = inference->get_output();
                }
            }
            auto end_time = std::chrono::high_resolution_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
            int delay_ms = max(0, static_cast<int>(1000.0 / get_max_fps() - elapsed));
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        }
    });

    osc_send_thread = std::thread([this] ()
    {
        auto last_time = std::chrono::high_resolution_clock::now();
        double fps_total = 0;
        double fps_count = 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        while (is_running())
        {
         if (fps_total > 1000)
         {
             fps_count = 0;
             fps_total = 0;
         }
         // calculate fps
         auto start = std::chrono::high_resolution_clock::now();
         auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(start - last_time);
         last_time = start;
         auto fps = 1000.0 / static_cast<double>(duration.count());
         fps_total += fps;
         fps_count += 1;
         fps = fps_total/fps_count;
         auto start_time = std::chrono::high_resolution_clock::now();
        // 发送OSC数据
         {
             std::lock_guard<std::mutex> lock(outputs_mutex);
             if (!outputs.empty()) {
                updateCalibrationProgressBars(outputs, inference->getBlendShapeIndexMap());
                osc_manager->sendModelOutput(outputs, blend_shapes);
             }
         }

         auto end_time = std::chrono::high_resolution_clock::now();
         auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
         int delay_ms = max(0, static_cast<int>(1000.0 / 66.0 - elapsed));
         std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        }
    });
}
void PaperFaceTrackerWindow::onCheekPuffLeftOffsetChanged()
{
    bool ok;
    float value = ui.CheekPuffLeftOffset->text().toFloat(&ok);
    if (ok) {
        cheek_puff_left_offset = value;
        updateOffsetsToInference();
    }
}

void PaperFaceTrackerWindow::onCheekPuffRightOffsetChanged()
{
    bool ok;
    float value = ui.CheekPuffRightOffset->text().toFloat(&ok);
    if (ok) {
        cheek_puff_right_offset = value;
        updateOffsetsToInference();
    }
}

void PaperFaceTrackerWindow::onJawOpenOffsetChanged()
{
    bool ok;
    float value = ui.JawOpenOffset->text().toFloat(&ok);
    if (ok) {
        jaw_open_offset = value;
        updateOffsetsToInference();
    }
}

void PaperFaceTrackerWindow::onTongueOutOffsetChanged()
{
    bool ok;
    float value = ui.TongueOutOffset->text().toFloat(&ok);
    if (ok) {
        tongue_out_offset = value;
        updateOffsetsToInference();
    }
}

void PaperFaceTrackerWindow::updateOffsetsToInference()
{
    // 创建一个包含偏置值的映射
    std::unordered_map<std::string, float> offset_map;
    offset_map["cheekPuffLeft"] = cheek_puff_left_offset;
    offset_map["cheekPuffRight"] = cheek_puff_right_offset;
    offset_map["jawOpen"] = jaw_open_offset;
    offset_map["tongueOut"] = tongue_out_offset;
    // 添加新的偏置值
    offset_map["mouthClose"] = mouth_close_offset;
    offset_map["mouthFunnel"] = mouth_funnel_offset;
    offset_map["mouthPucker"] = mouth_pucker_offset;
    offset_map["mouthRollUpper"] = mouth_roll_upper_offset;
    offset_map["mouthRollLower"] = mouth_roll_lower_offset;
    offset_map["mouthShrugUpper"] = mouth_shrug_upper_offset;
    offset_map["mouthShrugLower"] = mouth_shrug_lower_offset;

    // 调用inference的设置偏置值方法
    inference->set_offset_map(offset_map);

    // 添加下面这行代码，同时更新振幅映射
    inference->set_amp_map(getAmpMap());

    // 记录日志，便于调试
    //LOG_INFO("偏置值已更新到推理引擎");
}
void PaperFaceTrackerWindow::onShowSerialDataButtonClicked()
{
    showSerialData = !showSerialData;
    if (showSerialData) {
        LOG_INFO("已开启串口原始数据显示");
        ui.ShowSerialDataButton->setText("停止显示串口数据");
    } else {
        LOG_INFO("已关闭串口原始数据显示");
        ui.ShowSerialDataButton->setText("显示串口数据");
    }
}
void PaperFaceTrackerWindow::setupKalmanFilterControls() {
    // 在校准页面上添加卡尔曼滤波参数控制

    // dt参数
    dtLabel = new QLabel("时间步长(dt):", ui.page_2);
    dtLabel->setGeometry(QRect(510, 10, 120, 20));

    dtLineEdit = new QLineEdit(ui.page_2);
    dtLineEdit->setGeometry(QRect(630, 10, 80, 25));
    dtLineEdit->setText(QString::number(current_dt, 'f', 3));

    // q_factor参数
    qFactorLabel = new QLabel("过程噪声系数(q):", ui.page_2);
    qFactorLabel->setGeometry(QRect(510, 45, 120, 20));

    qFactorLineEdit = new QLineEdit(ui.page_2);
    qFactorLineEdit->setGeometry(QRect(630, 45, 80, 25));
    qFactorLineEdit->setText(QString::number(current_q_factor, 'f', 2));

    // r_factor参数
    rFactorLabel = new QLabel("测量噪声系数(r):", ui.page_2);
    rFactorLabel->setGeometry(QRect(510, 80, 120, 20));

    rFactorLineEdit = new QLineEdit(ui.page_2);
    rFactorLineEdit->setGeometry(QRect(630, 80, 80, 25));
    rFactorLineEdit->setText(QString::number(current_r_factor, 'f', 6));

    // 连接信号和槽
    connect(dtLineEdit, &QLineEdit::editingFinished, this, &PaperFaceTrackerWindow::onDtEditingFinished);
    connect(qFactorLineEdit, &QLineEdit::editingFinished, this, &PaperFaceTrackerWindow::onQFactorEditingFinished);
    connect(rFactorLineEdit, &QLineEdit::editingFinished, this, &PaperFaceTrackerWindow::onRFactorEditingFinished);

    // 添加说明标签
    QLabel* helpLabel = new QLabel(ui.page_2);
    helpLabel->setGeometry(QRect(510, 115, 280, 60));
    helpLabel->setText("调整建议:\n"
                      "增大q值, 减小r值: 更灵敏, 抖动更明显\n"
                      "减小q值, 增大r值: 更平滑, 反应更滞后");
    helpLabel->setWordWrap(true);

    // 设置控件样式
    QString labelStyle = "QLabel { color: white; font-weight: bold; }";
    dtLabel->setStyleSheet(labelStyle);
    qFactorLabel->setStyleSheet(labelStyle);
    rFactorLabel->setStyleSheet(labelStyle);
    helpLabel->setStyleSheet(labelStyle);
}
void PaperFaceTrackerWindow::onDtEditingFinished() {
    bool ok;
    float value = dtLineEdit->text().toFloat(&ok);
    if (ok && value > 0) {
        current_dt = value;
        if (inference) {
            inference->set_dt(current_dt);
            LOG_INFO("卡尔曼滤波参数已更新: dt = {}", current_dt);
        }
    } else {
        // 输入无效，恢复原值
        dtLineEdit->setText(QString::number(current_dt, 'f', 3));
    }
}

void PaperFaceTrackerWindow::onQFactorEditingFinished() {
    bool ok;
    float value = qFactorLineEdit->text().toFloat(&ok);
    if (ok && value > 0) {
        current_q_factor = value;
        if (inference) {
            inference->set_q_factor(current_q_factor);
            LOG_INFO("卡尔曼滤波参数已更新: q_factor = {}", current_q_factor);
        }
    } else {
        // 输入无效，恢复原值
        qFactorLineEdit->setText(QString::number(current_q_factor, 'f', 2));
    }
}

void PaperFaceTrackerWindow::onRFactorEditingFinished() {
    bool ok;
    float value = rFactorLineEdit->text().toFloat(&ok);
    if (ok && value > 0) {
        current_r_factor = value;
        if (inference) {
            inference->set_r_factor(current_r_factor);
            LOG_INFO("卡尔曼滤波参数已更新: r_factor = {}", current_r_factor);
        }
    } else {
        // 输入无效，恢复原值
        rFactorLineEdit->setText(QString::number(current_r_factor, 'f', 6));
    }
}