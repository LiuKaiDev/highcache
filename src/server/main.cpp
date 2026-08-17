#include "highcache/common/error.h"
#include "highcache/config/config.h"
#include "highcache/logging/logger.h"

#include <exception>
#include <iostream>

int main(const int argc, char *argv[]) {
  try {
    if (argc > 2) {
      throw highcache::HighCacheError(highcache::ErrorCode::invalid_argument,
                                      "usage: highcache_server [config-file]");
    }

    const auto config =
        argc == 2 ? highcache::Config::from_file(argv[1]) : highcache::Config{};
    highcache::Logger logger(config.log_level());
    logger.info("HighCache initialized");
    return 0;
  } catch (const highcache::HighCacheError &error) {
    std::cerr << "highcache_server: " << highcache::to_string(error.code())
              << ": " << error.what() << '\n';
  } catch (const std::exception &error) {
    std::cerr << "highcache_server: unexpected error: " << error.what() << '\n';
  }

  return 1;
}
