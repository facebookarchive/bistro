/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#pragma once

#include <gflags/gflags.h>
#include <memory>
#include <thrift/lib/cpp/async/TAsyncSocket.h>
#include <thrift/lib/cpp2/async/HeaderClientChannel.h>
#include <thrift/lib/cpp2/protocol/DebugProtocol.h>

#include "bistro/bistro/if/gen-cpp2/common_types.h"

DECLARE_int32(thrift_connect_timeout_ms);
DECLARE_int32(thrift_send_timeout_ms);
DECLARE_int32(thrift_receive_timeout_ms);

namespace facebook { namespace bistro {

/**
 * Makes a Thrift asynchronous client for a ServiceAddress and port.
 *
 * Add proxy support here, if desired.
 */
template<typename ThriftClient>
std::shared_ptr<ThriftClient> getAsyncClientForAddress(
  apache::thrift::async::TEventBase* event_base,
  const cpp2::ServiceAddress& addr,
  int connect_timeout_ms = 0,
  int send_timeout_ms = 0,
  int receive_timeout_ms = 0
) {
  if (connect_timeout_ms == 0) {
    connect_timeout_ms = FLAGS_thrift_connect_timeout_ms;
  }
  if (send_timeout_ms == 0) {
    send_timeout_ms = FLAGS_thrift_send_timeout_ms;
  }
  if (receive_timeout_ms == 0) {
    receive_timeout_ms = FLAGS_thrift_receive_timeout_ms;
  }
  using namespace apache::thrift::async;
  using namespace apache::thrift;
  auto socket = TAsyncSocket::newSocket(
    event_base,
    addr.ip_or_host,
    addr.port,
    connect_timeout_ms
  );
  auto channel = HeaderClientChannel::newChannel(socket);
  // This also sets the socket send timeout, but we overwrite it below.
  channel->setTimeout(receive_timeout_ms);
  socket->setSendTimeout(send_timeout_ms);
  return std::make_shared<ThriftClient>(std::move(channel));
}

/**
 * Stores results from fanOutRequestToServices() until the last request ends
 * (as per refcount).  Then, invokes finish_fn, and self-destructs.
 */
template<
  typename ResultType,
  typename SendFunc,
  typename RecvFunc
>
struct FanOutRequestToServicesResults {

  typedef std::function<void(
    std::exception_ptr ex, const std::string& service_id
  )> ExceptFunc;
  typedef std::function<void(
    std::vector<std::unique_ptr<ResultType>>&& results,
    apache::thrift::async::TEventBase* event_base
  )> FinishFunc;

  int numReferences_;
  apache::thrift::async::TEventBase* eventBase_;
  // Store all functions here to guarantee their lifetimes
  SendFunc sendFn_;
  RecvFunc recvFn_;
  ExceptFunc exceptFn_;
  FinishFunc finishFn_;
  std::vector<std::unique_ptr<ResultType>> results_;

  // Starts with one reference. Call removeReference() to self-destruct.
  FanOutRequestToServicesResults(
      apache::thrift::async::TEventBase* event_base,
      SendFunc&& send_fn,
      RecvFunc&& recv_fn,
      ExceptFunc&& except_fn,
      FinishFunc&& finish_fn)
    : numReferences_(1),
      eventBase_(event_base),
      sendFn_(std::move(send_fn)),
      recvFn_(std::move(recv_fn)),
      exceptFn_(std::move(except_fn)),
      finishFn_(std::move(finish_fn)) {}

  void addReference() {
    CHECK(eventBase_->isInEventBaseThread());
    numReferences_ += 1;
  }

  void removeReference() {
    CHECK(eventBase_->isInEventBaseThread());
    CHECK(numReferences_ > 0) << "Should already have been deleted";
    numReferences_ -= 1;
    if (numReferences_ == 0) {
      finishFn_(std::move(results_), eventBase_);
      delete this;
    }
  }
};

/**
 * Sends the same Thrift request to each of a collection of services,
 * aggregates their results in a vector, and invokes finish_fn once the
 * fan-out is complete. Sample usage:
 *
 *   fanOutRequestToServices<cpp2::MyThriftAsyncClient, cpp2::MyReturnType>(
 *     apache::thrift::async::TEventBaseManager::get()->getEventBase(),
 *     &cpp2::MyThriftAsyncClient::callback_myMethod,
 *     &cpp2::MyThriftAsyncClient::recv_myMethod,
 *     [](exception_ptr ex, const string& service) {
 *       try {
 *         std::rethrow_exception(ex);  // ex is guaranteed to be non-null
 *       } catch(const std::exception& e) {
 *         LOG(ERROR) << "myMethod failed on " << service << ": " << e.what();
 *       }
 *     },
 *     [](
 *       vector<unique_ptr<cpp2::MyReturnType>>&& results,
 *       apache::thrift::async::TEventBase* evb
 *     ) {
 *       // Do whatever you want with the results
 *       evb->terminateLoopSoon();
 *     },
 *     your_services,
 *     my_method_arg1,
 *     my_method_arg2
 *   );
 *   // Execute scheduled work, and wait for finish_fn
 *   apache::thrift::async::TEventBaseManager::get()
 *     ->getEventBase()->loopForever();
 *
 * NOTE(facebook): This is a minimal, low-performance clone of SRAggregator2
 * with no connection-pooling.
 */
template<
  typename ThriftClient,
  typename ResultType,
  typename SendFunc,
  typename RecvFunc,
  typename... ARGS
>
void fanOutRequestToServices(
  apache::thrift::async::TEventBase* event_base,
  SendFunc&& send_fn,
  RecvFunc&& recv_fn,
  typename FanOutRequestToServicesResults<
    ResultType, SendFunc, RecvFunc
  >::ExceptFunc&& except_fn,
  typename FanOutRequestToServicesResults<
    ResultType, SendFunc, RecvFunc
  >::FinishFunc&& finish_fn,
  const std::vector<cpp2::ServiceAddress>& services,
  ARGS&&... args
) {
  using namespace apache::thrift;
  FanOutRequestToServicesResults<ResultType, SendFunc, RecvFunc>* state
    = new FanOutRequestToServicesResults<ResultType, SendFunc, RecvFunc>(
      event_base,
      std::move(send_fn),
      std::move(recv_fn),
      std::move(except_fn),
      std::move(finish_fn)
    );
  // Remove the initial reference once this goes out of scope (i.e. all
  // fanout requests are sent.
  SCOPE_EXIT {
    event_base->runInEventBaseThread([state](){ state->removeReference(); });
  };

  for (const auto& addr : services) {
    // GCC bug 47226 breaks forwarding 'args' into lambdas, but bind works.
    auto bound_send_fn = std::bind(
      state->sendFn_,
      std::placeholders::_1,
      std::placeholders::_2,
      std::forward<ARGS>(args)...
    );
    // The lambdas below all execute after their scopes die, so copy captures.
    event_base->runInEventBaseThread([state, bound_send_fn, addr]() {
      state->addReference();
      const std::string service_id = debugString(addr);
      try {
        auto client =
          getAsyncClientForAddress<ThriftClient>(state->eventBase_, addr);
        bound_send_fn(
          client.get(),
          std::unique_ptr<RequestCallback>(new FunctionReplyCallback([
            state, service_id, client  // Capture the client to keep it alive
          ](ClientReceiveState&& recv_state) {
            try {
              auto result = folly::make_unique<ResultType>();
              state->recvFn_(*result, recv_state);
              state->results_.emplace_back(std::move(result));
            } catch (...) {
              state->exceptFn_(std::current_exception(), service_id);
            }
            state->removeReference();
          }))
        );
      } catch (...) {
        state->exceptFn_(std::current_exception(), service_id);
        state->removeReference();
      }
    });
  }
}

}}
