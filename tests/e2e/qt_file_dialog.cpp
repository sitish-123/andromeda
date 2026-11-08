// Test client for portal_toolkits.sh: a plain Qt 6 app that opens QFileDialog::getOpenFileName. With
// QT_QPA_PLATFORMTHEME=xdgdesktopportal the dialog goes through the FileChooser portal. Prints the chosen path,
// or exits 1 when the dialog was cancelled.
#include <QApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QFileDialog>
#include <cstdio>

int main(int argc, char* argv[]) {
  QApplication app(argc, argv);
  // The portal theme reads the FileChooser version with an async call and falls back to Qt's own dialog until the
  // reply is in. A blocking call on the same connection returns after that reply, which events then deliver.
  auto get = QDBusMessage::createMethodCall("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                                            "org.freedesktop.DBus.Properties", "Get");
  get << "org.freedesktop.portal.FileChooser" << "version";
  QDBusConnection::sessionBus().call(get);
  QCoreApplication::processEvents();
  const QString path = QFileDialog::getOpenFileName(nullptr, "Open", argc > 1 ? argv[1] : QString());
  if (path.isEmpty()) {
    return 1;
  }
  std::printf("%s\n", path.toUtf8().constData());
  return 0;
}
