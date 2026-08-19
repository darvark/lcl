/*
 * Start the Qt frontend application.
 *
 * @param argc Command-line argument count.
 * @param argv Command-line argument vector.
 * @return Qt application exit code.
 */
int main(int argc, char **argv) {
  int qt_argc = 1;
  int print_features = 0;
  for (int i = 1; i < argc; i++) {
    if (std::strcmp(argv[i], "--debug") == 0) {
      app_debug_enabled = 1;
      continue;
    }

    if (std::strcmp(argv[i], "--features") == 0) {
      print_features = 1;
      continue;
    }

    argv[qt_argc++] = argv[i];
  }
  argv[qt_argc] = nullptr;

  if (print_features) {
    std::fprintf(stdout, "LOGGER_FEATURES_BEGIN\n");
    std::fprintf(stdout, "CONTEST_CONFIG_UI=1\n");
    std::fprintf(stdout, "CONTEST_CONFIG_BUTTON=1\n");
    std::fprintf(stdout, "CONTEST_CONFIG_TOOLBAR=1\n");
    std::fprintf(stdout, "CONTEST_CONFIG_MENU=1\n");
    std::fprintf(stdout, "CONTEST_CONFIG_SHORTCUT_CTRL_F8=1\n");
    std::fprintf(stdout, "CONTEST_CONFIG_SHORTCUT_CTRL_SHIFT_C=0\n");
    std::fprintf(stdout, "CONTEST_CONFIG_SHORTCUT_ALT_C=0\n");
    std::fprintf(stdout, "CONTEST_CONFIG_SHORTCUT_CTRL_ALT_C=0\n");
    std::fprintf(stdout, "BUILD_STAMP=%s %s\n", __DATE__, __TIME__);
    std::fprintf(stdout, "LOGGER_FEATURES_END\n");
    std::fflush(stdout);
    return 0;
  }

  QApplication app(qt_argc, argv);

  if (app_debug_enabled) {
    std::fprintf(stderr, "[debug] logger starting with --debug\n");
  }

  if (app_controller_init() != 0) {
    if (app_debug_enabled) {
      std::fprintf(stderr, "[debug] app_controller_init returned failure\n");
    }
    QMessageBox::warning(nullptr, "Logger",
                         "DXCluster startup failed. The app will continue without it.");
  }

  cat_init();

  LoggerQtWindow window;
  window.show();

  const int rc = app.exec();

  cat_disconnect_cw_keyer();
  cat_shutdown();
  app_controller_shutdown();
  return rc;
}
