#include "mainwindow.h"
#include "../src/fs.h"

#include <QApplication>
#include <QComboBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QThread>
#include <QVBoxLayout>
#include <QCheckBox>
#include <QFileDialog>

#include <windows.h>
#include <cstring>
#include <thread>

/* =================================================================== */
/* MountWorker — runs FsDriverMount in its own thread                  */
/* =================================================================== */

MountWorker::MountWorker(wdl::WebDavCtx *cfg, wdl::MountOptions *opt,
                         QObject *parent)
    : QObject(parent), m_cfg(cfg), m_opt(opt) {}

MountWorker::~MountWorker() { delete m_cfg; delete m_opt; }

void MountWorker::run() {
  void *fs = nullptr;
  bool ok = wdl::FsDriverMount(*m_opt, m_cfg, &fs);
  if (!ok) {
    emit errorOccurred(QString::fromWCharArray(L"Mount failed. "
        "Make sure WinFsp is installed and drive %1 is free.")
        .arg(QString::fromStdWString(m_opt->mount_point)));
    emit unmounted();
    return;
  }
  m_fs = fs;
  emit mounted(QString::fromStdWString(m_opt->mount_point));

  /* spin until requestStop(). WinFsp dispatcher has its own threads;
   * we just poll for the stop signal. */
  while (!m_stop) {
    QThread::msleep(200);
  }

  wdl::FsDriverUnmount(fs);
  m_fs = nullptr;
  emit unmounted();
}

void MountWorker::requestStop() { m_stop = true; }

/* =================================================================== */
/* MainWindow                                                           */
/* =================================================================== */

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
  setWindowTitle("webdav2localdisk");
  setMinimumSize(540, 460);
  setupUi();
}

MainWindow::~MainWindow() {
  if (m_thread && m_thread->isRunning()) {
    if (m_worker) m_worker->requestStop();
    m_thread->quit();
    m_thread->wait(3000);
  }
}

void MainWindow::setupUi() {
  auto *cw = new QWidget(this);
  setCentralWidget(cw);
  auto *top = new QVBoxLayout(cw);

  /* --- Connection group -------------------------------------------- */
  auto *grp = new QGroupBox("WebDAV Connection", cw);
  auto *gl  = new QVBoxLayout(grp);

  auto *urlRow = new QHBoxLayout;
  urlRow->addWidget(new QLabel("URL:"));
  m_url = new QLineEdit; m_url->setPlaceholderText("https://dav.example.com/dav");
  urlRow->addWidget(m_url);
  gl->addLayout(urlRow);

  auto *authRow = new QHBoxLayout;
  authRow->addWidget(new QLabel("User:"));
  m_user = new QLineEdit;
  authRow->addWidget(m_user);
  authRow->addWidget(new QLabel("Pass:"));
  m_pass = new QLineEdit; m_pass->setEchoMode(QLineEdit::Password);
  authRow->addWidget(m_pass);
  gl->addLayout(authRow);

  auto *optRow = new QHBoxLayout;
  optRow->addWidget(new QLabel("Drive:"));
  m_drive = new QComboBox;
  /* populated in refreshDriveLetters() */
  optRow->addWidget(m_drive);
  optRow->addWidget(new QLabel("Cache:"));
  m_cache = new QLineEdit;
  m_cache->setPlaceholderText("%LOCALAPPDATA%\\webdav2localdisk");
  optRow->addWidget(m_cache);
  m_noCache = new QCheckBox("no cache");
  optRow->addWidget(m_noCache);
  gl->addLayout(optRow);

  /* buttons */
  auto *btnRow = new QHBoxLayout;
  m_mountBtn = new QPushButton("Mount");
  m_unmountBtn = new QPushButton("Unmount");
  m_unmountBtn->setEnabled(false);
  m_testBtn = new QPushButton("Test DRIVE_FIXED");
  m_testBtn->setEnabled(false);
  btnRow->addWidget(m_mountBtn);
  btnRow->addWidget(m_unmountBtn);
  btnRow->addWidget(m_testBtn);
  btnRow->addStretch();
  gl->addLayout(btnRow);

  m_status = new QLabel("Not connected");
  gl->addWidget(m_status);

  top->addWidget(grp);

  /* --- Log area ---------------------------------------------------- */
  m_log = new QPlainTextEdit;
  m_log->setReadOnly(true);
  m_log->setMaximumBlockCount(2000);
  top->addWidget(m_log);

  /* connections */
  connect(m_mountBtn, &QPushButton::clicked,
          this, &MainWindow::onMountClicked);
  connect(m_unmountBtn, &QPushButton::clicked,
          this, &MainWindow::onUnmountClicked);
  connect(m_testBtn, &QPushButton::clicked,
          this, &MainWindow::onDriveTypeTest);

  refreshDriveLetters();
}

void MainWindow::refreshDriveLetters() {
  m_drive->clear();
  DWORD bits = ::GetLogicalDrives();
  wchar_t d[3] = L" :";
  DWORD mask = 1;
  /* List free drive letters (DRIVE_NO_ROOT_DIR == 1).
   * We DO include those that exist (DRIVE_FIXED==3) but mark them as [used]. */
  for (wchar_t L = 'C'; L <= 'Z'; ++L, mask <<= 1) {
    bool exists = (bits & mask) != 0;
    d[0] = L;
    UINT type = exists ? ::GetDriveTypeW(d) : 1;
    if (exists) {
      m_drive->addItem(QString("%1 [%2]").arg(L).arg(type == DRIVE_FIXED ? "used" : "remote"), QString(L));
    } else {
      m_drive->addItem(QString("%1 (free)").arg(L), QString(L));
    }
  }
}

void MainWindow::onMountClicked() {
  QStringList drive = m_drive->currentData().toString();
  if (drive.isEmpty()) return;
  QChar letter = drive[0];
  std::wstring mp(1, letter.unicode()); mp += L":";

  /* pre-flight */
  DWORD bits = ::GetLogicalDrives();
  if (bits & (1 << (letter.unicode() - 'A'))) {
    QMessageBox::warning(this, "Drive in use",
        QString("Drive %1: is already in use. Pick another.").arg(letter));
    return;
  }

  auto *cfg = new wdl::WebDavCtx;
  cfg->server_url = m_url->text().toStdWString();
  cfg->username   = m_user->text().toStdWString();
  cfg->password   = m_pass->text().toStdWString();
  cfg->cache_dir  = m_noCache->isChecked() ? ""
                    : m_cache->text().toStdString();

  auto *opt = new wdl::MountOptions;
  opt->mount_point = mp;

  m_thread = new QThread(this);
  m_worker = new MountWorker(cfg, opt);
  m_worker->moveToThread(m_thread);

  connect(m_thread, &QThread::started,  m_worker, &MountWorker::run);
  connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);

  connect(m_worker, &MountWorker::mounted,
          this, &MainWindow::onMounted);
  connect(m_worker, &MountWorker::unmounted,
          this, &MainWindow::onUnmounted);
  connect(m_worker, &MountWorker::errorOccurred,
          this, &MainWindow::onError);
  connect(m_worker, &MountWorker::logMessage,
          this, &MainWindow::appendLog);

  m_log->clear();
  appendLog(QString("Mounting %1 → %2 ...")
            .arg(QString::fromStdWString(cfg->server_url))
            .arg(QString::fromStdWString(mp)));
  updateState(false);
  m_thread->start();
}

void MainWindow::onUnmountClicked() {
  if (m_worker) m_worker->requestStop();
}

void MainWindow::onMounted(const QString &drive) {
  m_mounted = true;
  updateState(true);
  m_status->setText(QString("Mounted on %1 (DRIVE_FIXED)").arg(drive));
  appendLog(QString("[OK] %1 mounted — GetDriveType returns DRIVE_FIXED.").arg(drive));
}

void MainWindow::onUnmounted() {
  m_mounted = false;
  if (m_thread) { m_thread->quit(); m_thread->wait(3000); m_thread = nullptr; }
  m_worker = nullptr;
  updateState(false);
  m_status->setText("Not connected");
  appendLog("[OK] unmounted.");
  refreshDriveLetters();
}

void MainWindow::onError(const QString &msg) {
  appendLog(QString("[ERROR] ") + msg);
  updateState(false);
  m_status->setText("Error");
}

void MainWindow::onDriveTypeTest() {
  if (!m_mounted) return;
  QChar letter = m_drive->currentData().toString()[0];
  std::wstring root(1, letter.unicode()); root += L":\\";
  UINT dt = ::GetDriveTypeW(root.c_str());

  static const wchar_t *names[] = {
    L"unknown(0)", L"NO_ROOT_DIR", L"REMOVABLE", L"FIXED",
    L"REMOTE", L"CDROM", L"RAMDISK"
  };
  const wchar_t *name = (dt <= 6) ? names[dt] : L"???";
  QString result = QString("GetDriveTypeW(%1\\) == %2 (%3)")
      .arg(letter).arg(dt).arg(QString::fromWCharArray(name));
  appendLog(result);

  QString verdict = (dt == DRIVE_FIXED)
      ? QString("✅  PASS — DRIVE_FIXED! The trick works.")
      : QString("❌  FAIL — expected DRIVE_FIXED(3), got %1").arg(dt);
  QMessageBox::information(this, "DriveType Test", verdict);
}

void MainWindow::appendLog(const QString &msg) {
  m_log->appendPlainText(msg);
}

void MainWindow::updateState(bool mounted) {
  m_mountBtn->setEnabled(!mounted);
  m_unmountBtn->setEnabled(mounted);
  m_testBtn->setEnabled(mounted);
  m_url->setEnabled(!mounted);
  m_user->setEnabled(!mounted);
  m_pass->setEnabled(!mounted);
  m_drive->setEnabled(!mounted);
}
