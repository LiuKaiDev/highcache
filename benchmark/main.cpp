#include "benchmark/benchmark.h"

#include <exception>
#include <iostream>
#include <string_view>
#include <vector>

int main(const int argc, char *argv[]) {
  try {
    std::vector<std::string_view> arguments;
    arguments.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
    for (int index = 1; index < argc; ++index) {
      arguments.emplace_back(argv[index]);
    }

    const auto options = highcache::benchmark::parse_options(arguments);
    if (options.help) {
      highcache::benchmark::print_usage(std::cout);
      return 0;
    }

    const auto result = highcache::benchmark::run_benchmark(options);
    highcache::benchmark::print_result(std::cout, options, result);
    return result.failed == 0 ? 0 : 2;
  } catch (const std::exception &error) {
    std::cerr << "highcache_benchmark: " << error.what() << '\n';
    return 1;
  }
}
