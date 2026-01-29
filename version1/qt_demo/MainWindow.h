#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QPushButton>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QSpinBox>
#include <QComboBox>
#include <QProgressBar>
#include <QGroupBox>
#include <QTabWidget>

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void onRunStressTest();
    void onRunComparison();
    void onRunMultiThreadTest();
    void clearLog();

private:
    QWidget *centralWidget;
    
    // Configuration Inputs
    QSpinBox *spinAllocCount;
    QSpinBox *spinThreadCount;
    QComboBox *comboBlockSize;
    
    // Display
    QTextEdit *logConsole;
    QProgressBar *progressBar;
    QLabel *statusLabel;
    QLabel *lblTotalMemory; // New label for memory estimation

    void setupUi();
    void log(const QString &msg, const QString &color = "white");
    void checkMemoryUsage(); // New helper check
    
    // Test Helpers
    void runSingleThreadTask(size_t count, size_t size, bool usePool);
    void runMultiThreadTask(int threads, size_t countPerThread, size_t size, bool usePool);
};

#endif // MAINWINDOW_H
