/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/processes/SubprocessOutputWithTimeout.h"
#include "bistro/bistro/processes/AsyncReadPipe.h"
#include <folly/io/async/EventBaseManager.h>

namespace facebook { namespace bistro {

folly::ProcessReturnCode subprocessOutputWithTimeout(
    const std::vector<std::string>& cmd,
    std::vector<std::string>* stdOutLines,
    std::vector<std::string>* stdErrLines,
    uint32_t timeoutMs,
    uint32_t pollMs) {
  folly::ProcessReturnCode res;
  // Set subprocess options, read stdout, stderr and kill on timeout
  auto opts =
    folly::Subprocess::Options().pipeStdout().
                       pipeStderr().
                       parentDeathSignal(SIGKILL).
                       processGroupLeader();

  try {
    // Create Subprocess object
    folly::Subprocess proc(cmd, opts);

    // We can not use a local thread event base, it can be part of the other
    // even_base driven system like IOThreadPool
    folly::EventBase localEvb;
    auto evb = &localEvb;

    // set read callback for pipes
    std::vector<folly::Future<folly::Unit>> pipe_futures;
    for (auto&& p : proc.takeOwnershipOfPipes()) {
      const int childFd = p.childFd;
      pipe_futures.emplace_back(asyncReadPipe(
        evb,
        std::move(p.pipe),
        readPipeLinesCallback([childFd, stdOutLines, stdErrLines](
          AsyncReadPipe*, folly::StringPiece s) {
          if (!s.empty()) {
            switch (childFd) {
              case STDOUT_FILENO:
                if (stdOutLines) {
                  stdOutLines->emplace_back(s.data(), s.size());
                }
                break;
              case STDERR_FILENO:
                if (stdErrLines) {
                  stdErrLines->emplace_back(s.data(), s.size());
                }
                break;
              default:
                LOG(FATAL) << "Unexpected pipe descriptor: " << childFd;
            }
          }
          return true;  // Keep reading
        },
        2048)  // Limit line length
      )->pipeClosed());
    }

    // Take into account timeout and calculate how many runtime callback
    // invocations should happen before timeout
    int32_t num_polls =
      std::ceil(double(std::max(1U, timeoutMs)) / double(std::max(1U, pollMs)));

    collectAllSemiFuture(
        // subprocess
        asyncSubprocess(
            evb,
            std::move(proc),
            [evb, &num_polls](folly::Subprocess& p) {
              // track timeout
              if (--num_polls <= 0) {
                // kill process
                LOG(WARNING) << "Subprocess timed out, killing it...";
                if (kill(-p.pid(), SIGKILL) == -1) {
                  PLOG(ERROR) << "Failed to signal the process group"
                              << ", trying to signal the process.";
                  // The previous kill should never fail, but a fallback seems
                  // ok.
                  p.kill();
                }
              }
            },
            pollMs)
            .thenValue([&res](folly::ProcessReturnCode && rc) noexcept {
              res = rc;
            }),
        // all pipes
        collectAllSemiFuture(pipe_futures)
            .toUnsafeFuture()
            .thenValue([](
                std::vector<folly::Try<folly::Unit>> && allClosed) noexcept {
              for (auto& pipeClosed : allClosed) {
                try {
                  pipeClosed.throwIfFailed();
                } catch (const std::exception& e) {
                  LOG(ERROR) << "task_pipe_error, message: " << e.what();
                }
              }
            }))
        .toUnsafeFuture()
        .thenValue([evb](
            std::tuple<
                folly::Try<folly::Unit>,
                folly::Try<folly::Unit>> &&) noexcept {
          // all is done
          evb->terminateLoopSoon();
        });
    // completion engine pumping
    evb->loopForever();
  } catch (const std::exception& x) {
    LOG(ERROR) << "Cannot start subprocess, cmd: " << folly::join(' ', cmd)
               << ", err: " << x.what();
  }

  return res;
}

}}  // namespace facebook::bistro
