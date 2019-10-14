/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

namespace facebook { namespace bistro {

inline void wrapOrCloseFd(folly::File* file, int fd) {
  if (file) {
    *file = folly::File(fd, /*owns_fd=*/ true);
  } else {
    folly::checkUnixError(::close(fd));
  }
}

inline void makePipe(folly::File* read_pipe, folly::File* write_pipe) {
  int pipe_fds[2];
  folly::checkUnixError(pipe(pipe_fds), "pipe");
  wrapOrCloseFd(write_pipe, pipe_fds[1]);
  wrapOrCloseFd(read_pipe, pipe_fds[0]);
}

}}
