#pragma once

#include "harness/options.hpp"

namespace shmipc::bench {

// Commands implemented outside spsc_bench.cpp (which owns main()).
int run_waitcurve(const Options& opt);
int run_mpsc(const Options& opt);

}  // namespace shmipc::bench
