#include "shmipc/platform/memory.hpp"

#include <gtest/gtest.h>
#include <sys/mman.h>

#include <cerrno>
#include <cstring>

namespace shmipc {
namespace {

class MappedPages : public ::testing::Test {
 protected:
  void SetUp() override {
    size_ = 4 * page_size();
    p_ = mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT_NE(p_, MAP_FAILED);
  }
  void TearDown() override { munmap(p_, size_); }

  void* p_ = nullptr;
  std::size_t size_ = 0;
};

TEST(Memory, PageSizeIsPowerOfTwo) {
  const std::size_t ps = page_size();
  ASSERT_GE(ps, 4096u);
  EXPECT_EQ(ps & (ps - 1), 0u);
}

TEST_F(MappedPages, PrefaultPreservesContents) {
  std::memset(p_, 0x5a, size_);
  prefault(p_, size_, /*write=*/true);
  prefault(p_, size_, /*write=*/false);
  const auto* bytes = static_cast<const unsigned char*>(p_);
  for (std::size_t i = 0; i < size_; ++i) ASSERT_EQ(bytes[i], 0x5a) << i;
}

TEST_F(MappedPages, LockAndUnlock) {
  if (!lock_memory(p_, size_)) {
    GTEST_SKIP() << "mlock failed (RLIMIT_MEMLOCK?): " << std::strerror(errno);
  }
  EXPECT_TRUE(unlock_memory(p_, size_));
}

TEST_F(MappedPages, HugePageAdviceIsOnlyAHint) {
  (void)advise_huge_pages(p_, size_);  // Must not crash or change contents.
  static_cast<unsigned char*>(p_)[0] = 1;
  EXPECT_EQ(static_cast<unsigned char*>(p_)[0], 1);
}

}  // namespace
}  // namespace shmipc
