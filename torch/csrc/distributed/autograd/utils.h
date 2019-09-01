#pragma once

#include <torch/csrc/distributed/autograd/context/dist_autograd_context.h>
#include <torch/csrc/distributed/rpc/message.h>

namespace torch {
namespace distributed {
namespace autograd {

// This method is used to attach the 'send' autograd function to the autograd
// graph when we use RPC. This method creates a new 'send' autograd function
// and attaches the provided tensors as next_edges to the 'send' function. In
// addition to this, it also registers the send function in the provided
// autograd context. Finally, the RPC message is updated with appropriate
// autograd information for the recipient.
void addSendRpcBackward(
    DistAutogradContext& autograd_context,
    torch::distributed::rpc::Message& message);

// This method is used to attach the 'recv' autograd function to the autograd
// graph when we use RPC. This method creates a new 'recv' autograd function
// and attaches the provided tensors as inputs to the 'recv' function. It
// creates a new autograd context if needed and registers the 'recv' function
// with this context.
//
// Returns a pointer to the autograd context created (nullptr in case of no
// autograd information was needed.)
DistAutogradContext* addRecvRpcBackward(
    torch::distributed::rpc::Message& message);

} // namespace autograd
} // namespace distributed
} // namespace torch
