#include "mainwindow.h"
#include <QApplication>

int main(int argc, char *argv[]) {
  QApplication app(argc, argv);
  app.setApplicationName("webdav2localdisk");
  app.setApplicationVersion("0.1.0");
  app.setOrganizationName("BlueFang");

  MainWindow w;
  w.show();
  return app.exec();
}
