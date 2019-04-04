/*
 *
 * Copyright 2019 The gRPC Authors
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

#include "src/core/lib/iomgr/port.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <random>
#include <thread>

#include <grpc/grpc.h>
#include <grpc/support/alloc.h>
#include <grpc/support/atm.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>
#include <grpc/support/time.h>
#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/health_check_service_interface.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <gtest/gtest.h>

#include "src/core/lib/backoff/backoff.h"
#include "src/core/lib/gpr/env.h"

#include "src/proto/grpc/testing/echo.grpc.pb.h"
#include "test/core/util/port.h"
#include "test/core/util/test_config.h"
#include "test/cpp/end2end/test_service_impl.h"

#ifdef GRPC_CFSTREAM
using grpc::ClientAsyncResponseReader;
using grpc::testing::EchoRequest;
using grpc::testing::EchoResponse;
using grpc::testing::RequestParams;
using std::chrono::system_clock;

namespace grpc {
namespace testing {
namespace {

class CFStreamTest : public ::testing::Test {
 protected:
  CFStreamTest()
      : server_host_("grpctest"),
        interface_("lo0"),
        ipv4_address_("10.0.0.1"),
        kRequestMessage_("🖖") {}

  void DNSUp() {
    std::ostringstream cmd;
    // Add DNS entry for server_host_ in /etc/hosts
    cmd << "echo '" << ipv4_address_ << "      " << server_host_
        << "  ' | sudo tee -a /etc/hosts";
    std::system(cmd.str().c_str());
  }

  void DNSDown() {
    std::ostringstream cmd;
    // Remove DNS entry for server_host_ in /etc/hosts
    cmd << "sudo sed -i '.bak' '/" << server_host_ << "/d' /etc/hosts";
    std::system(cmd.str().c_str());
  }

  void InterfaceUp() {
    std::ostringstream cmd;
    cmd << "sudo /sbin/ifconfig " << interface_ << " alias " << ipv4_address_;
    std::system(cmd.str().c_str());
  }

  void InterfaceDown() {
    std::ostringstream cmd;
    cmd << "sudo /sbin/ifconfig " << interface_ << " -alias " << ipv4_address_;
    std::system(cmd.str().c_str());
  }

  void NetworkUp() {
    gpr_log(GPR_DEBUG, "Bringing network up");
    InterfaceUp();
    DNSUp();
  }

  void NetworkDown() {
    gpr_log(GPR_DEBUG, "Bringing network down");
    InterfaceDown();
    DNSDown();
  }

  void SetUp() override {
    NetworkUp();
    grpc_init();
    StartServer();
  }

  void TearDown() override {
    NetworkDown();
    StopServer();
    grpc_shutdown();
  }

  void StartServer() {
    port_ = grpc_pick_unused_port_or_die();
    server_.reset(new ServerData(port_));
    server_->Start(server_host_);
  }
  void StopServer() { server_->Shutdown(); }

  std::unique_ptr<grpc::testing::EchoTestService::Stub> BuildStub(
      const std::shared_ptr<Channel>& channel) {
    return grpc::testing::EchoTestService::NewStub(channel);
  }

  std::shared_ptr<Channel> BuildChannel() {
    std::ostringstream server_address;
    server_address << server_host_ << ":" << port_;
    return CreateCustomChannel(
        server_address.str(), InsecureChannelCredentials(), ChannelArguments());
  }

  void SendRpc(
      const std::unique_ptr<grpc::testing::EchoTestService::Stub>& stub,
      bool expect_success = false) {
    auto response = std::unique_ptr<EchoResponse>(new EchoResponse());
    EchoRequest request;
    request.set_message(kRequestMessage_);
    ClientContext context;
    Status status = stub->Echo(&context, request, response.get());
    if (status.ok()) {
      gpr_log(GPR_DEBUG, "RPC returned %s\n", response->message().c_str());
    } else {
      gpr_log(GPR_DEBUG, "RPC failed: %s", status.error_message().c_str());
    }
    if (expect_success) {
      EXPECT_TRUE(status.ok());
    }
  }
  void SendAsyncRpc(
      const std::unique_ptr<grpc::testing::EchoTestService::Stub>& stub,
      RequestParams param = RequestParams()) {
    EchoRequest request;
    auto msg = std::to_string(ctr.load());
    request.set_message(msg);
    ctr++;
    *request.mutable_param() = std::move(param);
    AsyncClientCall* call = new AsyncClientCall;

    call->response_reader =
        stub->PrepareAsyncEcho(&call->context, request, &cq_);

    call->response_reader->StartCall();
    gpr_log(GPR_DEBUG, "Sending request: %s", msg.c_str());
    call->response_reader->Finish(&call->reply, &call->status, (void*)call);
  }

  void ShutdownCQ() { cq_.Shutdown(); }

  bool CQNext(void** tag, bool* ok) { return cq_.Next(tag, ok); }

  bool WaitForChannelNotReady(Channel* channel, int timeout_seconds = 5) {
    const gpr_timespec deadline =
        grpc_timeout_seconds_to_deadline(timeout_seconds);
    grpc_connectivity_state state;
    while ((state = channel->GetState(false /* try_to_connect */)) ==
           GRPC_CHANNEL_READY) {
      if (!channel->WaitForStateChange(state, deadline)) return false;
    }
    return true;
  }

  bool WaitForChannelReady(Channel* channel, int timeout_seconds = 10) {
    const gpr_timespec deadline =
        grpc_timeout_seconds_to_deadline(timeout_seconds);
    grpc_connectivity_state state;
    while ((state = channel->GetState(true /* try_to_connect */)) !=
           GRPC_CHANNEL_READY) {
      if (!channel->WaitForStateChange(state, deadline)) return false;
    }
    return true;
  }

  struct AsyncClientCall {
    EchoResponse reply;
    ClientContext context;
    Status status;
    std::unique_ptr<ClientAsyncResponseReader<EchoResponse>> response_reader;
  };

 private:
  struct ServerData {
    int port_;
    std::unique_ptr<Server> server_;
    TestServiceImpl service_;
    std::unique_ptr<std::thread> thread_;
    bool server_ready_ = false;

    explicit ServerData(int port) { port_ = port; }

    void Start(const grpc::string& server_host) {
      gpr_log(GPR_INFO, "starting server on port %d", port_);
      std::mutex mu;
      std::unique_lock<std::mutex> lock(mu);
      std::condition_variable cond;
      thread_.reset(new std::thread(
          std::bind(&ServerData::Serve, this, server_host, &mu, &cond)));
      cond.wait(lock, [this] { return server_ready_; });
      server_ready_ = false;
      gpr_log(GPR_INFO, "server startup complete");
    }

    void Serve(const grpc::string& server_host, std::mutex* mu,
               std::condition_variable* cond) {
      std::ostringstream server_address;
      server_address << server_host << ":" << port_;
      ServerBuilder builder;
      builder.AddListeningPort(server_address.str(),
                               InsecureServerCredentials());
      builder.RegisterService(&service_);
      server_ = builder.BuildAndStart();
      std::lock_guard<std::mutex> lock(*mu);
      server_ready_ = true;
      cond->notify_one();
    }

    void Shutdown(bool join = true) {
      server_->Shutdown(grpc_timeout_milliseconds_to_deadline(0));
      if (join) thread_->join();
    }
  };

  CompletionQueue cq_;
  const grpc::string server_host_;
  const grpc::string interface_;
  const grpc::string ipv4_address_;
  std::unique_ptr<ServerData> server_;
  int port_;
  const grpc::string kRequestMessage_;
  std::atomic_int ctr{0};
};

// gRPC should automatically detech network flaps (without enabling keepalives)
//  when CFStream is enabled
TEST_F(CFStreamTest, NetworkTransition) {
  auto channel = BuildChannel();
  auto stub = BuildStub(channel);
  // Channel should be in READY state after we send an RPC
  SendRpc(stub, /*expect_success=*/true);
  EXPECT_EQ(channel->GetState(false), GRPC_CHANNEL_READY);

  std::atomic_bool shutdown{false};
  std::thread sender = std::thread([this, &stub, &shutdown]() {
    while (true) {
      if (shutdown.load()) {
        return;
      }
      SendRpc(stub);
      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
  });

  // bring down network
  NetworkDown();

  // network going down should be detected by cfstream
  EXPECT_TRUE(WaitForChannelNotReady(channel.get()));

  // bring network interface back up
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  NetworkUp();

  // channel should reconnect
  EXPECT_TRUE(WaitForChannelReady(channel.get()));
  EXPECT_EQ(channel->GetState(false), GRPC_CHANNEL_READY);
  shutdown.store(true);
  sender.join();
}

// Network flaps while RPCs are in flight
TEST_F(CFStreamTest, NetworkFlapRpcsInFlight) {
  auto channel = BuildChannel();
  auto stub = BuildStub(channel);
  std::atomic_int rpcs_sent{0};

  // Channel should be in READY state after we send some RPCs
  for (int i = 0; i < 10; ++i) {
    SendAsyncRpc(stub);
    ++rpcs_sent;
  }
  EXPECT_TRUE(WaitForChannelReady(channel.get()));

  // Bring down the network
  NetworkDown();

  std::thread thd = std::thread([this, &rpcs_sent]() {
    void* got_tag;
    bool ok = false;
    bool network_down = true;
    int total_completions = 0;

    while (CQNext(&got_tag, &ok)) {
      ++total_completions;
      GPR_ASSERT(ok);
      AsyncClientCall* call = static_cast<AsyncClientCall*>(got_tag);
      if (call->status.ok()) {
        gpr_log(GPR_DEBUG, "RPC response: %s", call->reply.message().c_str());
      } else {
        gpr_log(GPR_DEBUG, "RPC failed with error: %s",
                call->status.error_message().c_str());
        // Bring network up when RPCs start failing
        if (network_down) {
          NetworkUp();
          network_down = false;
        }
      }
      delete call;
    }
    EXPECT_EQ(total_completions, rpcs_sent);
  });

  for (int i = 0; i < 100; ++i) {
    SendAsyncRpc(stub);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ++rpcs_sent;
  }

  ShutdownCQ();

  thd.join();
}

// Send a bunch of RPCs, some of which are expected to fail.
// We should get back a response for all RPCs
TEST_F(CFStreamTest, ConcurrentRpc) {
  auto channel = BuildChannel();
  auto stub = BuildStub(channel);
  std::atomic_int rpcs_sent{0};
  std::thread thd = std::thread([this, &rpcs_sent]() {
    void* got_tag;
    bool ok = false;
    bool network_down = true;
    int total_completions = 0;

    while (CQNext(&got_tag, &ok)) {
      ++total_completions;
      GPR_ASSERT(ok);
      AsyncClientCall* call = static_cast<AsyncClientCall*>(got_tag);
      if (call->status.ok()) {
        gpr_log(GPR_DEBUG, "RPC response: %s", call->reply.message().c_str());
      } else {
        gpr_log(GPR_DEBUG, "RPC failed: %s",
                call->status.error_message().c_str());
        // Bring network up when RPCs start failing
        if (network_down) {
          NetworkUp();
          network_down = false;
        }
      }
      delete call;
    }
    EXPECT_EQ(total_completions, rpcs_sent);
  });

  for (int i = 0; i < 10; ++i) {
    if (i % 3 == 0) {
      RequestParams param;
      ErrorStatus* error = param.mutable_expected_error();
      error->set_code(StatusCode::INTERNAL);
      error->set_error_message("internal error");
      SendAsyncRpc(stub, param);
    } else if (i % 5 == 0) {
      RequestParams param;
      param.set_echo_metadata(true);
      DebugInfo* info = param.mutable_debug_info();
      info->add_stack_entries("stack_entry1");
      info->add_stack_entries("stack_entry2");
      info->set_detail("detailed debug info");
      SendAsyncRpc(stub, param);
    } else {
      SendAsyncRpc(stub);
    }
    ++rpcs_sent;
  }

  ShutdownCQ();

  thd.join();
}

}  // namespace
}  // namespace testing
}  // namespace grpc
#endif  // GRPC_CFSTREAM

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  grpc_test_init(argc, argv);
  gpr_setenv("grpc_cfstream", "1");
  const auto result = RUN_ALL_TESTS();
  return result;
}
