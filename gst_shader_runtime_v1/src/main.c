#include "app.h"
#include "config.h"
#include "log.h"

int main(int argc, char **argv) {
  AppConfig cfg;
  if (!parse_args(argc, argv, &cfg)) {
    LOG_ERR("configuration failed");
    return 1;
  }
  if (!app_setup(&cfg)) {
    cleanup_config(&cfg);
    return 2;
  }
  app_loop();
  app_teardown();
  cleanup_config(&cfg);
  return 0;
}
