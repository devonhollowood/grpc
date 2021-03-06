/*
 *
 * Copyright 2016 gRPC authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include "test/cpp/end2end/test_service_impl.h"

#include <string>
#include <thread>

#include <grpc/support/log.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/server_context.h>

#include "src/proto/grpc/testing/echo.grpc.pb.h"
#include "test/cpp/util/string_ref_helper.h"

#include <gtest/gtest.h>

using std::chrono::system_clock;

namespace grpc {
namespace testing {
namespace {

// When echo_deadline is requested, deadline seen in the ServerContext is set in
// the response in seconds.
void MaybeEchoDeadline(ServerContext* context, const EchoRequest* request,
                       EchoResponse* response) {
  if (request->has_param() && request->param().echo_deadline()) {
    gpr_timespec deadline = gpr_inf_future(GPR_CLOCK_REALTIME);
    if (context->deadline() != system_clock::time_point::max()) {
      Timepoint2Timespec(context->deadline(), &deadline);
    }
    response->mutable_param()->set_request_deadline(deadline.tv_sec);
  }
}

void CheckServerAuthContext(
    const ServerContext* context,
    const grpc::string& expected_transport_security_type,
    const grpc::string& expected_client_identity) {
  std::shared_ptr<const AuthContext> auth_ctx = context->auth_context();
  std::vector<grpc::string_ref> tst =
      auth_ctx->FindPropertyValues("transport_security_type");
  EXPECT_EQ(1u, tst.size());
  EXPECT_EQ(expected_transport_security_type, ToString(tst[0]));
  if (expected_client_identity.empty()) {
    EXPECT_TRUE(auth_ctx->GetPeerIdentityPropertyName().empty());
    EXPECT_TRUE(auth_ctx->GetPeerIdentity().empty());
    EXPECT_FALSE(auth_ctx->IsPeerAuthenticated());
  } else {
    auto identity = auth_ctx->GetPeerIdentity();
    EXPECT_TRUE(auth_ctx->IsPeerAuthenticated());
    EXPECT_EQ(1u, identity.size());
    EXPECT_EQ(expected_client_identity, identity[0]);
  }
}

// Returns the number of pairs in metadata that exactly match the given
// key-value pair. Returns -1 if the pair wasn't found.
int MetadataMatchCount(
    const std::multimap<grpc::string_ref, grpc::string_ref>& metadata,
    const grpc::string& key, const grpc::string& value) {
  int count = 0;
  for (std::multimap<grpc::string_ref, grpc::string_ref>::const_iterator iter =
           metadata.begin();
       iter != metadata.end(); ++iter) {
    if (ToString(iter->first) == key && ToString(iter->second) == value) {
      count++;
    }
  }
  return count;
}
}  // namespace

namespace {
int GetIntValueFromMetadataHelper(
    const char* key,
    const std::multimap<grpc::string_ref, grpc::string_ref>& metadata,
    int default_value) {
  if (metadata.find(key) != metadata.end()) {
    std::istringstream iss(ToString(metadata.find(key)->second));
    iss >> default_value;
    gpr_log(GPR_INFO, "%s : %d", key, default_value);
  }

  return default_value;
}

int GetIntValueFromMetadata(
    const char* key,
    const std::multimap<grpc::string_ref, grpc::string_ref>& metadata,
    int default_value) {
  return GetIntValueFromMetadataHelper(key, metadata, default_value);
}

void ServerTryCancel(ServerContext* context) {
  EXPECT_FALSE(context->IsCancelled());
  context->TryCancel();
  gpr_log(GPR_INFO, "Server called TryCancel() to cancel the request");
  // Now wait until it's really canceled
  while (!context->IsCancelled()) {
    gpr_sleep_until(gpr_time_add(gpr_now(GPR_CLOCK_REALTIME),
                                 gpr_time_from_micros(1000, GPR_TIMESPAN)));
  }
}

void ServerTryCancelNonblocking(ServerContext* context) {
  EXPECT_FALSE(context->IsCancelled());
  context->TryCancel();
  gpr_log(GPR_INFO, "Server called TryCancel() to cancel the request");
}

void LoopUntilCancelled(Alarm* alarm, ServerContext* context,
                        experimental::ServerCallbackRpcController* controller,
                        int loop_delay_us) {
  if (!context->IsCancelled()) {
    alarm->experimental().Set(
        gpr_time_add(gpr_now(GPR_CLOCK_REALTIME),
                     gpr_time_from_micros(loop_delay_us, GPR_TIMESPAN)),
        [alarm, context, controller, loop_delay_us](bool) {
          LoopUntilCancelled(alarm, context, controller, loop_delay_us);
        });
  } else {
    controller->Finish(Status::CANCELLED);
  }
}
}  // namespace

Status TestServiceImpl::Echo(ServerContext* context, const EchoRequest* request,
                             EchoResponse* response) {
  // A bit of sleep to make sure that short deadline tests fail
  if (request->has_param() && request->param().server_sleep_us() > 0) {
    gpr_sleep_until(
        gpr_time_add(gpr_now(GPR_CLOCK_MONOTONIC),
                     gpr_time_from_micros(request->param().server_sleep_us(),
                                          GPR_TIMESPAN)));
  }

  if (request->has_param() && request->param().server_die()) {
    gpr_log(GPR_ERROR, "The request should not reach application handler.");
    GPR_ASSERT(0);
  }
  if (request->has_param() && request->param().has_expected_error()) {
    const auto& error = request->param().expected_error();
    return Status(static_cast<StatusCode>(error.code()), error.error_message(),
                  error.binary_error_details());
  }
  int server_try_cancel = GetIntValueFromMetadata(
      kServerTryCancelRequest, context->client_metadata(), DO_NOT_CANCEL);
  if (server_try_cancel > DO_NOT_CANCEL) {
    // Since this is a unary RPC, by the time this server handler is called,
    // the 'request' message is already read from the client. So the scenarios
    // in server_try_cancel don't make much sense. Just cancel the RPC as long
    // as server_try_cancel is not DO_NOT_CANCEL
    ServerTryCancel(context);
    return Status::CANCELLED;
  }

  response->set_message(request->message());
  MaybeEchoDeadline(context, request, response);
  if (host_) {
    response->mutable_param()->set_host(*host_);
  }
  if (request->has_param() && request->param().client_cancel_after_us()) {
    {
      std::unique_lock<std::mutex> lock(mu_);
      signal_client_ = true;
    }
    while (!context->IsCancelled()) {
      gpr_sleep_until(gpr_time_add(
          gpr_now(GPR_CLOCK_REALTIME),
          gpr_time_from_micros(request->param().client_cancel_after_us(),
                               GPR_TIMESPAN)));
    }
    return Status::CANCELLED;
  } else if (request->has_param() &&
             request->param().server_cancel_after_us()) {
    gpr_sleep_until(gpr_time_add(
        gpr_now(GPR_CLOCK_REALTIME),
        gpr_time_from_micros(request->param().server_cancel_after_us(),
                             GPR_TIMESPAN)));
    return Status::CANCELLED;
  } else if (!request->has_param() ||
             !request->param().skip_cancelled_check()) {
    EXPECT_FALSE(context->IsCancelled());
  }

  if (request->has_param() && request->param().echo_metadata_initially()) {
    const std::multimap<grpc::string_ref, grpc::string_ref>& client_metadata =
        context->client_metadata();
    for (std::multimap<grpc::string_ref, grpc::string_ref>::const_iterator
             iter = client_metadata.begin();
         iter != client_metadata.end(); ++iter) {
      context->AddInitialMetadata(ToString(iter->first),
                                  ToString(iter->second));
    }
  }

  if (request->has_param() && request->param().echo_metadata()) {
    const std::multimap<grpc::string_ref, grpc::string_ref>& client_metadata =
        context->client_metadata();
    for (std::multimap<grpc::string_ref, grpc::string_ref>::const_iterator
             iter = client_metadata.begin();
         iter != client_metadata.end(); ++iter) {
      context->AddTrailingMetadata(ToString(iter->first),
                                   ToString(iter->second));
    }
    // Terminate rpc with error and debug info in trailer.
    if (request->param().debug_info().stack_entries_size() ||
        !request->param().debug_info().detail().empty()) {
      grpc::string serialized_debug_info =
          request->param().debug_info().SerializeAsString();
      context->AddTrailingMetadata(kDebugInfoTrailerKey, serialized_debug_info);
      return Status::CANCELLED;
    }
  }
  if (request->has_param() &&
      (request->param().expected_client_identity().length() > 0 ||
       request->param().check_auth_context())) {
    CheckServerAuthContext(context,
                           request->param().expected_transport_security_type(),
                           request->param().expected_client_identity());
  }
  if (request->has_param() && request->param().response_message_length() > 0) {
    response->set_message(
        grpc::string(request->param().response_message_length(), '\0'));
  }
  if (request->has_param() && request->param().echo_peer()) {
    response->mutable_param()->set_peer(context->peer());
  }
  return Status::OK;
}

Status TestServiceImpl::CheckClientInitialMetadata(
    ServerContext* context, const SimpleRequest* /*request*/,
    SimpleResponse* /*response*/) {
  EXPECT_EQ(MetadataMatchCount(context->client_metadata(),
                               kCheckClientInitialMetadataKey,
                               kCheckClientInitialMetadataVal),
            1);
  EXPECT_EQ(1u,
            context->client_metadata().count(kCheckClientInitialMetadataKey));
  return Status::OK;
}

void CallbackTestServiceImpl::Echo(
    ServerContext* context, const EchoRequest* request, EchoResponse* response,
    experimental::ServerCallbackRpcController* controller) {
  CancelState* cancel_state = new CancelState;
  int server_use_cancel_callback =
      GetIntValueFromMetadata(kServerUseCancelCallback,
                              context->client_metadata(), DO_NOT_USE_CALLBACK);
  if (server_use_cancel_callback != DO_NOT_USE_CALLBACK) {
    controller->SetCancelCallback([cancel_state] {
      EXPECT_FALSE(cancel_state->callback_invoked.exchange(
          true, std::memory_order_relaxed));
    });
    if (server_use_cancel_callback == MAYBE_USE_CALLBACK_EARLY_CANCEL) {
      EXPECT_TRUE(context->IsCancelled());
      EXPECT_TRUE(
          cancel_state->callback_invoked.load(std::memory_order_relaxed));
    } else {
      EXPECT_FALSE(context->IsCancelled());
      EXPECT_FALSE(
          cancel_state->callback_invoked.load(std::memory_order_relaxed));
    }
  }
  // A bit of sleep to make sure that short deadline tests fail
  if (request->has_param() && request->param().server_sleep_us() > 0) {
    // Set an alarm for that much time
    alarm_.experimental().Set(
        gpr_time_add(gpr_now(GPR_CLOCK_MONOTONIC),
                     gpr_time_from_micros(request->param().server_sleep_us(),
                                          GPR_TIMESPAN)),
        [this, context, request, response, controller, cancel_state](bool) {
          EchoNonDelayed(context, request, response, controller, cancel_state);
        });
  } else {
    EchoNonDelayed(context, request, response, controller, cancel_state);
  }
}

void CallbackTestServiceImpl::CheckClientInitialMetadata(
    ServerContext* context, const SimpleRequest* /*request*/,
    SimpleResponse* /*response*/,
    experimental::ServerCallbackRpcController* controller) {
  EXPECT_EQ(MetadataMatchCount(context->client_metadata(),
                               kCheckClientInitialMetadataKey,
                               kCheckClientInitialMetadataVal),
            1);
  EXPECT_EQ(1u,
            context->client_metadata().count(kCheckClientInitialMetadataKey));
  controller->Finish(Status::OK);
}

void CallbackTestServiceImpl::EchoNonDelayed(
    ServerContext* context, const EchoRequest* request, EchoResponse* response,
    experimental::ServerCallbackRpcController* controller,
    CancelState* cancel_state) {
  int server_use_cancel_callback =
      GetIntValueFromMetadata(kServerUseCancelCallback,
                              context->client_metadata(), DO_NOT_USE_CALLBACK);

  // Safe to clear cancel callback even if it wasn't set
  controller->ClearCancelCallback();
  if (server_use_cancel_callback == MAYBE_USE_CALLBACK_EARLY_CANCEL ||
      server_use_cancel_callback == MAYBE_USE_CALLBACK_LATE_CANCEL) {
    EXPECT_TRUE(context->IsCancelled());
    EXPECT_TRUE(cancel_state->callback_invoked.load(std::memory_order_relaxed));
    delete cancel_state;
    controller->Finish(Status::CANCELLED);
    return;
  }

  EXPECT_FALSE(cancel_state->callback_invoked.load(std::memory_order_relaxed));
  delete cancel_state;

  if (request->has_param() && request->param().server_die()) {
    gpr_log(GPR_ERROR, "The request should not reach application handler.");
    GPR_ASSERT(0);
  }
  if (request->has_param() && request->param().has_expected_error()) {
    const auto& error = request->param().expected_error();
    controller->Finish(Status(static_cast<StatusCode>(error.code()),
                              error.error_message(),
                              error.binary_error_details()));
    return;
  }
  int server_try_cancel = GetIntValueFromMetadata(
      kServerTryCancelRequest, context->client_metadata(), DO_NOT_CANCEL);
  if (server_try_cancel > DO_NOT_CANCEL) {
    // Since this is a unary RPC, by the time this server handler is called,
    // the 'request' message is already read from the client. So the scenarios
    // in server_try_cancel don't make much sense. Just cancel the RPC as long
    // as server_try_cancel is not DO_NOT_CANCEL
    EXPECT_FALSE(context->IsCancelled());
    context->TryCancel();
    gpr_log(GPR_INFO, "Server called TryCancel() to cancel the request");

    if (server_use_cancel_callback == DO_NOT_USE_CALLBACK) {
      // Now wait until it's really canceled
      LoopUntilCancelled(&alarm_, context, controller, 1000);
    }
    return;
  }

  gpr_log(GPR_DEBUG, "Request message was %s", request->message().c_str());
  response->set_message(request->message());
  MaybeEchoDeadline(context, request, response);
  if (host_) {
    response->mutable_param()->set_host(*host_);
  }
  if (request->has_param() && request->param().client_cancel_after_us()) {
    {
      std::unique_lock<std::mutex> lock(mu_);
      signal_client_ = true;
    }
    if (server_use_cancel_callback == DO_NOT_USE_CALLBACK) {
      // Now wait until it's really canceled
      LoopUntilCancelled(&alarm_, context, controller,
                         request->param().client_cancel_after_us());
    }
    return;
  } else if (request->has_param() &&
             request->param().server_cancel_after_us()) {
    alarm_.experimental().Set(
        gpr_time_add(
            gpr_now(GPR_CLOCK_REALTIME),
            gpr_time_from_micros(request->param().server_cancel_after_us(),
                                 GPR_TIMESPAN)),
        [controller](bool) { controller->Finish(Status::CANCELLED); });
    return;
  } else if (!request->has_param() ||
             !request->param().skip_cancelled_check()) {
    EXPECT_FALSE(context->IsCancelled());
  }

  if (request->has_param() && request->param().echo_metadata_initially()) {
    const std::multimap<grpc::string_ref, grpc::string_ref>& client_metadata =
        context->client_metadata();
    for (std::multimap<grpc::string_ref, grpc::string_ref>::const_iterator
             iter = client_metadata.begin();
         iter != client_metadata.end(); ++iter) {
      context->AddInitialMetadata(ToString(iter->first),
                                  ToString(iter->second));
    }
    controller->SendInitialMetadata([](bool ok) { EXPECT_TRUE(ok); });
  }

  if (request->has_param() && request->param().echo_metadata()) {
    const std::multimap<grpc::string_ref, grpc::string_ref>& client_metadata =
        context->client_metadata();
    for (std::multimap<grpc::string_ref, grpc::string_ref>::const_iterator
             iter = client_metadata.begin();
         iter != client_metadata.end(); ++iter) {
      context->AddTrailingMetadata(ToString(iter->first),
                                   ToString(iter->second));
    }
    // Terminate rpc with error and debug info in trailer.
    if (request->param().debug_info().stack_entries_size() ||
        !request->param().debug_info().detail().empty()) {
      grpc::string serialized_debug_info =
          request->param().debug_info().SerializeAsString();
      context->AddTrailingMetadata(kDebugInfoTrailerKey, serialized_debug_info);
      controller->Finish(Status::CANCELLED);
      return;
    }
  }
  if (request->has_param() &&
      (request->param().expected_client_identity().length() > 0 ||
       request->param().check_auth_context())) {
    CheckServerAuthContext(context,
                           request->param().expected_transport_security_type(),
                           request->param().expected_client_identity());
  }
  if (request->has_param() && request->param().response_message_length() > 0) {
    response->set_message(
        grpc::string(request->param().response_message_length(), '\0'));
  }
  if (request->has_param() && request->param().echo_peer()) {
    response->mutable_param()->set_peer(context->peer());
  }
  controller->Finish(Status::OK);
}

// Unimplemented is left unimplemented to test the returned error.

Status TestServiceImpl::RequestStream(ServerContext* context,
                                      ServerReader<EchoRequest>* reader,
                                      EchoResponse* response) {
  // If 'server_try_cancel' is set in the metadata, the RPC is cancelled by
  // the server by calling ServerContext::TryCancel() depending on the value:
  //   CANCEL_BEFORE_PROCESSING: The RPC is cancelled before the server reads
  //   any message from the client
  //   CANCEL_DURING_PROCESSING: The RPC is cancelled while the server is
  //   reading messages from the client
  //   CANCEL_AFTER_PROCESSING: The RPC is cancelled after the server reads
  //   all the messages from the client
  int server_try_cancel = GetIntValueFromMetadata(
      kServerTryCancelRequest, context->client_metadata(), DO_NOT_CANCEL);

  EchoRequest request;
  response->set_message("");

  if (server_try_cancel == CANCEL_BEFORE_PROCESSING) {
    ServerTryCancel(context);
    return Status::CANCELLED;
  }

  std::thread* server_try_cancel_thd = nullptr;
  if (server_try_cancel == CANCEL_DURING_PROCESSING) {
    server_try_cancel_thd =
        new std::thread([context] { ServerTryCancel(context); });
  }

  int num_msgs_read = 0;
  while (reader->Read(&request)) {
    response->mutable_message()->append(request.message());
  }
  gpr_log(GPR_INFO, "Read: %d messages", num_msgs_read);

  if (server_try_cancel_thd != nullptr) {
    server_try_cancel_thd->join();
    delete server_try_cancel_thd;
    return Status::CANCELLED;
  }

  if (server_try_cancel == CANCEL_AFTER_PROCESSING) {
    ServerTryCancel(context);
    return Status::CANCELLED;
  }

  return Status::OK;
}

// Return 'kNumResponseStreamMsgs' messages.
// TODO(yangg) make it generic by adding a parameter into EchoRequest
Status TestServiceImpl::ResponseStream(ServerContext* context,
                                       const EchoRequest* request,
                                       ServerWriter<EchoResponse>* writer) {
  // If server_try_cancel is set in the metadata, the RPC is cancelled by the
  // server by calling ServerContext::TryCancel() depending on the value:
  //   CANCEL_BEFORE_PROCESSING: The RPC is cancelled before the server writes
  //   any messages to the client
  //   CANCEL_DURING_PROCESSING: The RPC is cancelled while the server is
  //   writing messages to the client
  //   CANCEL_AFTER_PROCESSING: The RPC is cancelled after the server writes
  //   all the messages to the client
  int server_try_cancel = GetIntValueFromMetadata(
      kServerTryCancelRequest, context->client_metadata(), DO_NOT_CANCEL);

  int server_coalescing_api = GetIntValueFromMetadata(
      kServerUseCoalescingApi, context->client_metadata(), 0);

  int server_responses_to_send = GetIntValueFromMetadata(
      kServerResponseStreamsToSend, context->client_metadata(),
      kServerDefaultResponseStreamsToSend);

  if (server_try_cancel == CANCEL_BEFORE_PROCESSING) {
    ServerTryCancel(context);
    return Status::CANCELLED;
  }

  EchoResponse response;
  std::thread* server_try_cancel_thd = nullptr;
  if (server_try_cancel == CANCEL_DURING_PROCESSING) {
    server_try_cancel_thd =
        new std::thread([context] { ServerTryCancel(context); });
  }

  for (int i = 0; i < server_responses_to_send; i++) {
    response.set_message(request->message() + grpc::to_string(i));
    if (i == server_responses_to_send - 1 && server_coalescing_api != 0) {
      writer->WriteLast(response, WriteOptions());
    } else {
      writer->Write(response);
    }
  }

  if (server_try_cancel_thd != nullptr) {
    server_try_cancel_thd->join();
    delete server_try_cancel_thd;
    return Status::CANCELLED;
  }

  if (server_try_cancel == CANCEL_AFTER_PROCESSING) {
    ServerTryCancel(context);
    return Status::CANCELLED;
  }

  return Status::OK;
}

Status TestServiceImpl::BidiStream(
    ServerContext* context,
    ServerReaderWriter<EchoResponse, EchoRequest>* stream) {
  // If server_try_cancel is set in the metadata, the RPC is cancelled by the
  // server by calling ServerContext::TryCancel() depending on the value:
  //   CANCEL_BEFORE_PROCESSING: The RPC is cancelled before the server reads/
  //   writes any messages from/to the client
  //   CANCEL_DURING_PROCESSING: The RPC is cancelled while the server is
  //   reading/writing messages from/to the client
  //   CANCEL_AFTER_PROCESSING: The RPC is cancelled after the server
  //   reads/writes all messages from/to the client
  int server_try_cancel = GetIntValueFromMetadata(
      kServerTryCancelRequest, context->client_metadata(), DO_NOT_CANCEL);

  EchoRequest request;
  EchoResponse response;

  if (server_try_cancel == CANCEL_BEFORE_PROCESSING) {
    ServerTryCancel(context);
    return Status::CANCELLED;
  }

  std::thread* server_try_cancel_thd = nullptr;
  if (server_try_cancel == CANCEL_DURING_PROCESSING) {
    server_try_cancel_thd =
        new std::thread([context] { ServerTryCancel(context); });
  }

  // kServerFinishAfterNReads suggests after how many reads, the server should
  // write the last message and send status (coalesced using WriteLast)
  int server_write_last = GetIntValueFromMetadata(
      kServerFinishAfterNReads, context->client_metadata(), 0);

  int read_counts = 0;
  while (stream->Read(&request)) {
    read_counts++;
    gpr_log(GPR_INFO, "recv msg %s", request.message().c_str());
    response.set_message(request.message());
    if (read_counts == server_write_last) {
      stream->WriteLast(response, WriteOptions());
    } else {
      stream->Write(response);
    }
  }

  if (server_try_cancel_thd != nullptr) {
    server_try_cancel_thd->join();
    delete server_try_cancel_thd;
    return Status::CANCELLED;
  }

  if (server_try_cancel == CANCEL_AFTER_PROCESSING) {
    ServerTryCancel(context);
    return Status::CANCELLED;
  }

  return Status::OK;
}

experimental::ServerReadReactor<EchoRequest, EchoResponse>*
CallbackTestServiceImpl::RequestStream() {
  class Reactor : public ::grpc::experimental::ServerReadReactor<EchoRequest,
                                                                 EchoResponse> {
   public:
    Reactor() {}
    void OnStarted(ServerContext* context, EchoResponse* response) override {
      // Assign ctx_ and response_ as late as possible to increase likelihood of
      // catching any races

      // If 'server_try_cancel' is set in the metadata, the RPC is cancelled by
      // the server by calling ServerContext::TryCancel() depending on the
      // value:
      //   CANCEL_BEFORE_PROCESSING: The RPC is cancelled before the server
      //   reads any message from the client CANCEL_DURING_PROCESSING: The RPC
      //   is cancelled while the server is reading messages from the client
      //   CANCEL_AFTER_PROCESSING: The RPC is cancelled after the server reads
      //   all the messages from the client
      server_try_cancel_ = GetIntValueFromMetadata(
          kServerTryCancelRequest, context->client_metadata(), DO_NOT_CANCEL);

      response->set_message("");

      if (server_try_cancel_ == CANCEL_BEFORE_PROCESSING) {
        ServerTryCancelNonblocking(context);
        ctx_ = context;
      } else {
        if (server_try_cancel_ == CANCEL_DURING_PROCESSING) {
          context->TryCancel();
          // Don't wait for it here
        }
        ctx_ = context;
        response_ = response;
        StartRead(&request_);
      }

      on_started_done_ = true;
    }
    void OnDone() override { delete this; }
    void OnCancel() override {
      EXPECT_TRUE(on_started_done_);
      EXPECT_TRUE(ctx_->IsCancelled());
      FinishOnce(Status::CANCELLED);
    }
    void OnReadDone(bool ok) override {
      if (ok) {
        response_->mutable_message()->append(request_.message());
        num_msgs_read_++;
        StartRead(&request_);
      } else {
        gpr_log(GPR_INFO, "Read: %d messages", num_msgs_read_);

        if (server_try_cancel_ == CANCEL_DURING_PROCESSING) {
          // Let OnCancel recover this
          return;
        }
        if (server_try_cancel_ == CANCEL_AFTER_PROCESSING) {
          ServerTryCancelNonblocking(ctx_);
          return;
        }
        FinishOnce(Status::OK);
      }
    }

   private:
    void FinishOnce(const Status& s) {
      std::lock_guard<std::mutex> l(finish_mu_);
      if (!finished_) {
        Finish(s);
        finished_ = true;
      }
    }

    ServerContext* ctx_;
    EchoResponse* response_;
    EchoRequest request_;
    int num_msgs_read_{0};
    int server_try_cancel_;
    std::mutex finish_mu_;
    bool finished_{false};
    bool on_started_done_{false};
  };

  return new Reactor;
}

// Return 'kNumResponseStreamMsgs' messages.
// TODO(yangg) make it generic by adding a parameter into EchoRequest
experimental::ServerWriteReactor<EchoRequest, EchoResponse>*
CallbackTestServiceImpl::ResponseStream() {
  class Reactor
      : public ::grpc::experimental::ServerWriteReactor<EchoRequest,
                                                        EchoResponse> {
   public:
    Reactor() {}
    void OnStarted(ServerContext* context,
                   const EchoRequest* request) override {
      // Assign ctx_ and request_ as late as possible to increase likelihood of
      // catching any races

      // If 'server_try_cancel' is set in the metadata, the RPC is cancelled by
      // the server by calling ServerContext::TryCancel() depending on the
      // value:
      //   CANCEL_BEFORE_PROCESSING: The RPC is cancelled before the server
      //   reads any message from the client CANCEL_DURING_PROCESSING: The RPC
      //   is cancelled while the server is reading messages from the client
      //   CANCEL_AFTER_PROCESSING: The RPC is cancelled after the server reads
      //   all the messages from the client
      server_try_cancel_ = GetIntValueFromMetadata(
          kServerTryCancelRequest, context->client_metadata(), DO_NOT_CANCEL);
      server_coalescing_api_ = GetIntValueFromMetadata(
          kServerUseCoalescingApi, context->client_metadata(), 0);
      server_responses_to_send_ = GetIntValueFromMetadata(
          kServerResponseStreamsToSend, context->client_metadata(),
          kServerDefaultResponseStreamsToSend);
      if (server_try_cancel_ == CANCEL_BEFORE_PROCESSING) {
        ServerTryCancelNonblocking(context);
        ctx_ = context;
      } else {
        if (server_try_cancel_ == CANCEL_DURING_PROCESSING) {
          context->TryCancel();
        }
        ctx_ = context;
        request_ = request;
        if (num_msgs_sent_ < server_responses_to_send_) {
          NextWrite();
        }
      }
      on_started_done_ = true;
    }
    void OnDone() override { delete this; }
    void OnCancel() override {
      EXPECT_TRUE(on_started_done_);
      EXPECT_TRUE(ctx_->IsCancelled());
      FinishOnce(Status::CANCELLED);
    }
    void OnWriteDone(bool /*ok*/) override {
      if (num_msgs_sent_ < server_responses_to_send_) {
        NextWrite();
      } else if (server_coalescing_api_ != 0) {
        // We would have already done Finish just after the WriteLast
      } else if (server_try_cancel_ == CANCEL_DURING_PROCESSING) {
        // Let OnCancel recover this
      } else if (server_try_cancel_ == CANCEL_AFTER_PROCESSING) {
        ServerTryCancelNonblocking(ctx_);
      } else {
        FinishOnce(Status::OK);
      }
    }

   private:
    void FinishOnce(const Status& s) {
      std::lock_guard<std::mutex> l(finish_mu_);
      if (!finished_) {
        Finish(s);
        finished_ = true;
      }
    }

    void NextWrite() {
      response_.set_message(request_->message() +
                            grpc::to_string(num_msgs_sent_));
      if (num_msgs_sent_ == server_responses_to_send_ - 1 &&
          server_coalescing_api_ != 0) {
        num_msgs_sent_++;
        StartWriteLast(&response_, WriteOptions());
        // If we use WriteLast, we shouldn't wait before attempting Finish
        FinishOnce(Status::OK);
      } else {
        num_msgs_sent_++;
        StartWrite(&response_);
      }
    }
    ServerContext* ctx_;
    const EchoRequest* request_;
    EchoResponse response_;
    int num_msgs_sent_{0};
    int server_try_cancel_;
    int server_coalescing_api_;
    int server_responses_to_send_;
    std::mutex finish_mu_;
    bool finished_{false};
    bool on_started_done_{false};
  };
  return new Reactor;
}

experimental::ServerBidiReactor<EchoRequest, EchoResponse>*
CallbackTestServiceImpl::BidiStream() {
  class Reactor : public ::grpc::experimental::ServerBidiReactor<EchoRequest,
                                                                 EchoResponse> {
   public:
    Reactor() {}
    void OnStarted(ServerContext* context) override {
      // Assign ctx_ as late as possible to increase likelihood of catching any
      // races

      // If 'server_try_cancel' is set in the metadata, the RPC is cancelled by
      // the server by calling ServerContext::TryCancel() depending on the
      // value:
      //   CANCEL_BEFORE_PROCESSING: The RPC is cancelled before the server
      //   reads any message from the client CANCEL_DURING_PROCESSING: The RPC
      //   is cancelled while the server is reading messages from the client
      //   CANCEL_AFTER_PROCESSING: The RPC is cancelled after the server reads
      //   all the messages from the client
      server_try_cancel_ = GetIntValueFromMetadata(
          kServerTryCancelRequest, context->client_metadata(), DO_NOT_CANCEL);
      server_write_last_ = GetIntValueFromMetadata(
          kServerFinishAfterNReads, context->client_metadata(), 0);
      if (server_try_cancel_ == CANCEL_BEFORE_PROCESSING) {
        ServerTryCancelNonblocking(context);
        ctx_ = context;
      } else {
        if (server_try_cancel_ == CANCEL_DURING_PROCESSING) {
          context->TryCancel();
        }
        ctx_ = context;
        StartRead(&request_);
      }
      on_started_done_ = true;
    }
    void OnDone() override { delete this; }
    void OnCancel() override {
      EXPECT_TRUE(on_started_done_);
      EXPECT_TRUE(ctx_->IsCancelled());
      FinishOnce(Status::CANCELLED);
    }
    void OnReadDone(bool ok) override {
      if (ok) {
        num_msgs_read_++;
        gpr_log(GPR_INFO, "recv msg %s", request_.message().c_str());
        response_.set_message(request_.message());
        if (num_msgs_read_ == server_write_last_) {
          StartWriteLast(&response_, WriteOptions());
          // If we use WriteLast, we shouldn't wait before attempting Finish
        } else {
          StartWrite(&response_);
          return;
        }
      }

      if (server_try_cancel_ == CANCEL_DURING_PROCESSING) {
        // Let OnCancel handle this
      } else if (server_try_cancel_ == CANCEL_AFTER_PROCESSING) {
        ServerTryCancelNonblocking(ctx_);
      } else {
        FinishOnce(Status::OK);
      }
    }
    void OnWriteDone(bool /*ok*/) override {
      std::lock_guard<std::mutex> l(finish_mu_);
      if (!finished_) {
        StartRead(&request_);
      }
    }

   private:
    void FinishOnce(const Status& s) {
      std::lock_guard<std::mutex> l(finish_mu_);
      if (!finished_) {
        Finish(s);
        finished_ = true;
      }
    }

    ServerContext* ctx_;
    EchoRequest request_;
    EchoResponse response_;
    int num_msgs_read_{0};
    int server_try_cancel_;
    int server_write_last_;
    std::mutex finish_mu_;
    bool finished_{false};
    bool on_started_done_{false};
  };

  return new Reactor;
}

}  // namespace testing
}  // namespace grpc
