#include "shmipc/platform/process.hpp"

#include <gtest/gtest.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <thread>

namespace shmipc {
namespace {

TEST(Process, SelfIsAlive) {
  const ProcessId self = self_process();
  EXPECT_EQ(self.pid, getpid());
  EXPECT_NE(self.start_time, 0u);
  EXPECT_TRUE(process_alive(self));
}

TEST(Process, ReusedPidWithDifferentStartTimeIsDead) {
  ProcessId impostor = self_process();
  impostor.start_time += 1;  // Same PID, different incarnation.
  EXPECT_FALSE(process_alive(impostor));
}

TEST(Process, NonexistentPidIsDead) {
  EXPECT_FALSE(process_start_time(-1).has_value());
  EXPECT_FALSE(process_alive(ProcessId{0, 0}));
}

TEST(Process, ZombieIsDead) {
  const pid_t pid = fork();
  if (pid == 0) {
    pause();
    _exit(0);
  }
  ASSERT_GT(pid, 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  const auto start = process_start_time(pid);
  ASSERT_TRUE(start.has_value());
  const ProcessId child{pid, *start};
  EXPECT_TRUE(process_alive(child));

  kill(pid, SIGKILL);
  // Not reaped yet: the child is a zombie. kill(pid, 0) would still succeed here.
  for (int i = 0; i < 200 && process_alive(child); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_FALSE(process_alive(child));
  EXPECT_EQ(kill(pid, 0), 0) << "expected an unreaped zombie";

  waitpid(pid, nullptr, 0);
  EXPECT_FALSE(process_alive(child));
}

}  // namespace
}  // namespace shmipc
