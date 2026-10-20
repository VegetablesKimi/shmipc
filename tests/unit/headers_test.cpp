// Every public header in one translation unit: catches ODR clashes between
// headers that are never included together elsewhere. (broadcast.hpp and
// mpsc.hpp once both defined detail::unpack_header; the first program to
// use both channels, the apps test, failed to compile. LESSONS.md #9.)
// The include list is generated from include/ at configure time.
#include <gtest/gtest.h>

#include "all_public_headers.hpp"

TEST(Headers, AllPublicHeadersCompileTogether) {
  const auto w = shmipc::record::pack(24, 7, 3);
  const auto h = shmipc::record::unpack(w);
  EXPECT_EQ(h.length, 24);
  EXPECT_EQ(h.type, 7);
  EXPECT_EQ(h.flags, 3);
}
