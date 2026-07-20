#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QLineEdit>
#include <QComboBox>
#include <QCheckBox>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QLabel>
#include <QThread>

namespace wdl { struct WebDavCtx; struct MountOptions; }

class MountWorker : public QObject {
  Q_OBJECT
public:
  explicit MountWorker(wdl::WebDavCtx *cfg, wdl::MountOptions *opt,
                       QObject *parent = nullptr);
  ~MountWorker();

public slots:
  void run();
  void requestStop();

signals:
  void mounted(QString drive);
  void unmounted();
  void logMessage(QString msg);
  void errorOccurred(QString msg);

private:
  wdl::WebDavCtx   *m_cfg;
  wdl::MountOptions *m_opt;
  void             *m_fs   = nullptr;
  volatile bool     m_stop = false;
};

class MainWindow : public QMainWindow {
  Q_OBJECT
public:
  explicit MainWindow(QWidget *parent = nullptr);
  ~MainWindow();

private slots:
  void onMountClicked();
  void onUnmountClicked();
  void appendLog(const QString &msg);
  void onMounted(const QString &drive);
  void onUnmounted();
  void onError(const QString &msg);
  void onDriveTypeTest();

private:
  void setupUi();
  void updateState(bool mounted);
  void refreshDriveLetters();

  QLineEdit      *m_url     = nullptr;
  QLineEdit      *m_user    = nullptr;
  QLineEdit      *m_pass    = nullptr;
  QComboBox      *m_drive   = nullptr;
  QLineEdit      *m_cache   = nullptr;
  QCheckBox      *m_noCache = nullptr;
  QPushButton    *m_mountBtn = nullptr;
  QPushButton    *m_unmountBtn = nullptr;
  QPushButton    *m_testBtn = nullptr;
  QPlainTextEdit *m_log     = nullptr;
  QLabel         *m_status  = nullptr;

  QThread        *m_thread  = nullptr;
  MountWorker    *m_worker  = nullptr;
  bool            m_mounted = false;
};

#endif // MAINWINDOW_H
