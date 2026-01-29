#include "MainWindow.h"
#include "MemoryPool.h"
#include <QApplication>
// #include <QThread> // Removed unused include to avoid warning
#include <QScrollBar>
#include <chrono>
#include <vector>
#include <thread>
#include <random>
#include <iomanip>
#include <sstream>

using namespace my_memorypool;

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setupUi();
    setWindowTitle("High-Performance Memory Pool Benchmark");
    resize(900, 700);
}

MainWindow::~MainWindow()
{
}

void MainWindow::setupUi()
{
    centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);

    QVBoxLayout *mainLayout = new QVBoxLayout(centralWidget);
    mainLayout->setSpacing(10);
    mainLayout->setContentsMargins(20, 20, 20, 20);

    // --- Header ---
    QLabel *header = new QLabel("Memory Pool Analyzer");
    QFont headerFont("Arial", 18, QFont::Bold);
    header->setFont(headerFont);
    header->setAlignment(Qt::AlignCenter);
    mainLayout->addWidget(header);

    // --- Configuration Panel ---
    QGroupBox *configGroup = new QGroupBox("Benchmark Configuration");
    QHBoxLayout *configLayout = new QHBoxLayout(configGroup);

    QLabel *lblCount = new QLabel("Allocations:");
    spinAllocCount = new QSpinBox();
    spinAllocCount->setRange(1000, 10000000);
    spinAllocCount->setSingleStep(10000);
    spinAllocCount->setValue(100000);
    spinAllocCount->setSuffix(" ops");

    QLabel *lblThreads = new QLabel("Threads:");
    spinThreadCount = new QSpinBox();
    spinThreadCount->setRange(1, 32);
    spinThreadCount->setValue(1);

    QLabel *lblSize = new QLabel("Block Size:");
    comboBlockSize = new QComboBox();
    comboBlockSize->addItem("Small (64 B)", 64);
    comboBlockSize->addItem("Medium (4 KB)", 4096);
    comboBlockSize->addItem("Large (256 KB)", 256 * 1024);
    // comboBlockSize->addItem("Random (16B - 8KB)", 0); // Special case

    configLayout->addWidget(lblCount);
    configLayout->addWidget(spinAllocCount);
    configLayout->addSpacing(20);
    configLayout->addWidget(lblThreads);
    configLayout->addWidget(spinThreadCount);
    configLayout->addSpacing(20);
    configLayout->addWidget(lblSize);
    configLayout->addWidget(comboBlockSize);
    
    // Memory Usage Warning
    lblTotalMemory = new QLabel("Est. Mem: 6.1 MB");
    lblTotalMemory->setStyleSheet("color: gray; font-size: 10px;");
    configLayout->addSpacing(10);
    configLayout->addWidget(lblTotalMemory);
    
    configLayout->addStretch();

    mainLayout->addWidget(configGroup);

    // Signals for safety check
    connect(spinAllocCount, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::checkMemoryUsage);
    connect(spinThreadCount, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::checkMemoryUsage);
    connect(comboBlockSize, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::checkMemoryUsage);

    // Initial check
    checkMemoryUsage();

    // --- Action Buttons ---
    QHBoxLayout *btnLayout = new QHBoxLayout();
    
    QPushButton *btnStress = new QPushButton("Run Custom Benchmark");
    btnStress->setMinimumHeight(40);
    btnStress->setStyleSheet("font-weight: bold; background-color: #007ACC; color: white; border-radius: 5px;");
    connect(btnStress, &QPushButton::clicked, this, &MainWindow::onRunStressTest);

    QPushButton *btnCompare = new QPushButton("Compare vs malloc/free");
    btnCompare->setMinimumHeight(40);
    btnCompare->setStyleSheet("font-weight: bold; background-color: #2D9C5E; color: white; border-radius: 5px;");
    connect(btnCompare, &QPushButton::clicked, this, &MainWindow::onRunComparison);

    QPushButton *btnClear = new QPushButton("Clear Log");
    btnClear->setMinimumHeight(40);
    connect(btnClear, &QPushButton::clicked, this, &MainWindow::clearLog);

    btnLayout->addWidget(btnStress);
    btnLayout->addWidget(btnCompare);
    btnLayout->addWidget(btnClear);

    mainLayout->addLayout(btnLayout);

    // --- Progress Bar ---
    progressBar = new QProgressBar();
    progressBar->setRange(0, 100);
    progressBar->setValue(0);
    progressBar->setTextVisible(false);
    mainLayout->addWidget(progressBar);

    // --- Log Console ---
    QGroupBox *logGroup = new QGroupBox("Execution Log");
    QVBoxLayout *logLayout = new QVBoxLayout(logGroup);
    logConsole = new QTextEdit();
    logConsole->setReadOnly(true);
    logConsole->setStyleSheet("background-color: #1E1E1E; color: #D4D4D4; font-family: Consolas, Monospace; font-size: 11pt;");
    logLayout->addWidget(logConsole);
    mainLayout->addWidget(logGroup);

    // --- Status Bar ---
    statusLabel = new QLabel("System Ready");
    mainLayout->addWidget(statusLabel);
}

void MainWindow::log(const QString &msg, const QString &color)
{
    logConsole->append(QString("<span style='color:%1;'>%2</span>").arg(color, msg));
    logConsole->verticalScrollBar()->setValue(logConsole->verticalScrollBar()->maximum());
}

void MainWindow::checkMemoryUsage()
{
    long long count = spinAllocCount->value();
    long long threads = spinThreadCount->value();
    long long size = comboBlockSize->currentData().toLongLong();
    if (size == 0) size = 64; 

    long long totalBytes = count * threads * size;
    double mb = totalBytes / (1024.0 * 1024.0);
    double gb = mb / 1024.0;

    QString text;
    QString color = "gray";

    if (gb >= 1.0) {
        text = QString("Est. Mem: %1 GB").arg(gb, 0, 'f', 2);
        if (gb > 2.0) color = "red"; // Warn over 2GB
        else color = "#E6A23C"; // Warning orange over 1GB
    } else {
        text = QString("Est. Mem: %1 MB").arg(mb, 0, 'f', 1);
        color = "#67C23A"; // Green
    }

    lblTotalMemory->setText(text);
    lblTotalMemory->setStyleSheet(QString("font-weight:bold; color: %1;").arg(color));
}

void MainWindow::clearLog()
{
    logConsole->clear();
}

void MainWindow::onRunStressTest()
{
    checkMemoryUsage(); // Refresh label state
    if (lblTotalMemory->styleSheet().contains("red")) {
        log("<b>WARNING:</b> Benchmark skipped to prevent system freeze (Est. Memory > 2GB). Please reduce Allocations or Threads.", "red");
        return;
    }

    int count = spinAllocCount->value();
    int threads = spinThreadCount->value();
    size_t size = comboBlockSize->currentData().toULongLong();
    
    // Check for random size logic if I added it back, currently using fixed
    if (size == 0) size = 64; 

    log("==========================================", "#569CD6");
    log(QString("<b>Starting Benchmark:</b> %1 threads, %2 ops/thread, %3 bytes/block")
            .arg(threads).arg(count).arg(size), "#569CD6");

    statusLabel->setText("Running Benchmark...");
    progressBar->setRange(0, 0); // Indeterminate
    QApplication::processEvents();

    auto start = std::chrono::high_resolution_clock::now();

    if (threads == 1) {
        runSingleThreadTask(count, size, true);
    } else {
        runMultiThreadTask(threads, count, size, true);
    }

    auto end = std::chrono::high_resolution_clock::now();
    double elapsed_ms = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1000.0;
    
    double totalOps = (double)count * threads;
    double opsPerSec = totalOps / (elapsed_ms / 1000.0);

    progressBar->setRange(0, 100);
    progressBar->setValue(100);
    statusLabel->setText("Done.");

    log(QString("Total Time: <b>%1 ms</b>").arg(elapsed_ms, 0, 'f', 2), "#CE9178");
    checkMemoryUsage();
    if (lblTotalMemory->styleSheet().contains("red")) {
        log("<b>WARNING:</b> Comparison skipped to prevent system freeze (Est. Memory > 2GB). Please reduce Allocations or Threads.", "red");
        return;
    }

    log(QString("Throughout: <b>%1 ops/sec</b>").arg((long long)opsPerSec), "#B5CEA8");
    log("==========================================\n", "#569CD6");
}

void MainWindow::onRunComparison()
{
    int count = spinAllocCount->value();
    // Force single thread for comparison simplicity to show allocation overhead purely
    // Or allow multithread to show lock contention difference
    int threads = spinThreadCount->value(); 
    size_t size = comboBlockSize->currentData().toULongLong();

    log("================ COMPARING ================", "#C586C0");
    log(QString("Configuration: %1 threads, %2 ops, %3 bytes").arg(threads).arg(count).arg(size), "#DCDCAA");
    
    // 1. Run Malloc
    log("Running <b>malloc/free</b> ...", "gray");
    QApplication::processEvents();
    auto startMalloc = std::chrono::high_resolution_clock::now();
    if (threads == 1) runSingleThreadTask(count, size, false);
    else runMultiThreadTask(threads, count, size, false);
    auto endMalloc = std::chrono::high_resolution_clock::now();
    double timeMalloc = std::chrono::duration_cast<std::chrono::microseconds>(endMalloc - startMalloc).count() / 1000.0;
    log(QString("Malloc Time: %1 ms").arg(timeMalloc, 0, 'f', 2));

    // 2. Run MemoryPool
    log("Running <b>MemoryPool</b> ...", "gray");
    QApplication::processEvents();
    auto startPool = std::chrono::high_resolution_clock::now();
    if (threads == 1) runSingleThreadTask(count, size, true);
    else runMultiThreadTask(threads, count, size, true);
    auto endPool = std::chrono::high_resolution_clock::now();
    double timePool = std::chrono::duration_cast<std::chrono::microseconds>(endPool - startPool).count() / 1000.0;
    log(QString("Pool Time:   %1 ms").arg(timePool, 0, 'f', 2));

    // Result
    double ratio = timeMalloc / timePool;
    QString color = ratio > 1.0 ? "#4EC9B0" : "#F44747"; // Green if faster
    log(QString("<b>Speedup: %1x</b>").arg(ratio, 0, 'f', 2), color);
    
    if (timePool > timeMalloc) {
         log("Note: For very large allocations, system malloc might be faster or equal.", "gray");
    }
    log("==========================================\n", "#C586C0");
}

void MainWindow::onRunMultiThreadTest()
{
    // Mapped to onRunStressTest logic
}

void MainWindow::runSingleThreadTask(size_t count, size_t size, bool usePool)
{
    std::vector<void*> ptrs;
    ptrs.reserve(count);

    if (usePool) {
        for (size_t i = 0; i < count; ++i) {
            ptrs.push_back(MemoryPool::allocate(size));
        }
        for (void* p : ptrs) {
            MemoryPool::deallocate(p, size);
        }
    } else {
        for (size_t i = 0; i < count; ++i) {
            ptrs.push_back(std::malloc(size));
        }
        for (void* p : ptrs) {
            std::free(p);
        }
    }
}

void MainWindow::runMultiThreadTask(int threads, size_t countPerThread, size_t size, bool usePool)
{
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (int i = 0; i < threads; ++i) {
        workers.emplace_back([=]() {
            this->runSingleThreadTask(countPerThread, size, usePool);
        });
    }

    for (auto &t : workers) {
        t.join();
    }
}
