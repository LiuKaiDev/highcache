#include "highcache/cache/cache_engine.h"
#include "highcache/common/error.h"
#include "highcache/config/config.h"
#include "highcache/logging/logger.h"
#include "highcache/net/cache_server.h"

#include <pthread.h>
#include <signal.h>

#include <exception>
#include <iostream>
#include <sstream>
#include <string_view>

namespace {

sigset_t block_shutdown_signals() {
  sigset_t signals;
  if (::sigemptyset(&signals) != 0 || ::sigaddset(&signals, SIGINT) != 0 ||
      ::sigaddset(&signals, SIGTERM) != 0) {
    throw highcache::HighCacheError(highcache::ErrorCode::network,
                                    "unable to prepare shutdown signals");
  }
  const int result = ::pthread_sigmask(SIG_BLOCK, &signals, nullptr);
  if (result != 0) {
    throw highcache::HighCacheError(highcache::ErrorCode::network,
                                    "unable to block shutdown signals");
  }
  return signals;
}

void print_usage() { std::cout << "usage: highcache_server [config-file]\n"; }

} // namespace

int main(const int argc, char *argv[]) {
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
      print_usage();
      return 0;
    }
    if (argc > 2) {
      throw highcache::HighCacheError(highcache::ErrorCode::invalid_argument,
                                      "usage: highcache_server [config-file]");
    }

    const auto config =
        argc == 2 ? highcache::Config::from_file(argv[1]) : highcache::Config{};
    highcache::Logger logger(config.log_level());
    const auto shutdown_signals = block_shutdown_signals();
    highcache::CacheEngine engine;
    highcache::CacheServer server(
        engine, {config.host(), config.port(), config.worker_threads(),
                 highcache::ServerOptions{}.pending_output_limit});
    server.start();

    std::ostringstream startup;
    startup << "HighCache listening on " << server.host() << ':'
            << server.port() << " with " << server.worker_count()
            << " workers and " << engine.shard_count() << " cache shards";
    logger.info(startup.str());

    int received_signal = 0;
    const int wait_result = ::sigwait(&shutdown_signals, &received_signal);
    if (wait_result != 0) {
      throw highcache::HighCacheError(highcache::ErrorCode::network,
                                      "unable to wait for shutdown signal");
    }
    server.request_stop();
    server.join();
    logger.info("HighCache stopped");
    return 0;
  } catch (const highcache::HighCacheError &error) {
    std::cerr << "highcache_server: " << highcache::to_string(error.code())
              << ": " << error.what() << '\n';
  } catch (const std::exception &error) {
    std::cerr << "highcache_server: unexpected error: " << error.what() << '\n';
  }

  return 1;
}
