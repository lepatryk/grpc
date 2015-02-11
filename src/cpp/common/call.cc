/*
 *
 * Copyright 2014, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <grpc/support/alloc.h>
#include <grpc++/impl/call.h>
#include <grpc++/client_context.h>
#include <grpc++/channel_interface.h>

#include "src/cpp/proto/proto_utils.h"

namespace grpc {

void CallOpBuffer::Reset(void* next_return_tag) {
  return_tag_ = next_return_tag;

  send_initial_metadata_ = false;
  initial_metadata_count_ = 0;
  gpr_free(initial_metadata_);

  recv_initial_metadata_ = nullptr;
  gpr_free(recv_initial_metadata_arr_.metadata);
  recv_initial_metadata_arr_ = {0, 0, nullptr};

  send_message_ = nullptr;
  if (send_message_buf_) {
    grpc_byte_buffer_destroy(send_message_buf_);
    send_message_buf_ = nullptr;
  }

  recv_message_ = nullptr;
  if (recv_message_buf_) {
    grpc_byte_buffer_destroy(recv_message_buf_);
    recv_message_buf_ = nullptr;
  }

  client_send_close_ = false;

  recv_trailing_metadata_ = nullptr;
  recv_status_ = nullptr;
  gpr_free(recv_trailing_metadata_arr_.metadata);
  recv_trailing_metadata_arr_ = {0, 0, nullptr};

  status_code_ = GRPC_STATUS_OK;
  gpr_free(status_details_);
  status_details_ = nullptr;
  status_details_capacity_ = 0;

  send_status_ = nullptr;
  trailing_metadata_count_ = 0;
  trailing_metadata_ = nullptr;
}

namespace {
// TODO(yangg) if the map is changed before we send, the pointers will be a
// mess. Make sure it does not happen.
grpc_metadata* FillMetadataArray(
    std::multimap<grpc::string, grpc::string>* metadata) {
  if (metadata->empty()) { return nullptr; }
  grpc_metadata* metadata_array = (grpc_metadata*)gpr_malloc(
      metadata->size()* sizeof(grpc_metadata));
  size_t i = 0;
  for (auto iter = metadata->cbegin();
       iter != metadata->cend();
       ++iter, ++i) {
    metadata_array[i].key = iter->first.c_str();
    metadata_array[i].value = iter->second.c_str();
    metadata_array[i].value_length = iter->second.size();
  }
  return metadata_array;
}

void FillMetadataMap(grpc_metadata_array* arr,
                     std::multimap<grpc::string, grpc::string>* metadata) {
  for (size_t i = 0; i < arr->count; i++) {
    // TODO(yangg) handle duplicates?
    metadata->insert(std::pair<grpc::string, grpc::string>(
        arr->metadata[i].key, {arr->metadata[i].value, arr->metadata[i].value_length}));
  }
  grpc_metadata_array_destroy(arr);
  grpc_metadata_array_init(arr);
}
}  // namespace

void CallOpBuffer::AddSendInitialMetadata(
    std::multimap<grpc::string, grpc::string>* metadata) {
  send_initial_metadata_ = true;
  initial_metadata_count_ = metadata->size();
  initial_metadata_ = FillMetadataArray(metadata);
}

void CallOpBuffer::AddSendInitialMetadata(ClientContext *ctx) {
  AddSendInitialMetadata(&ctx->metadata_);
}

void CallOpBuffer::AddSendMessage(const google::protobuf::Message& message) {
  send_message_ = &message;
}

void CallOpBuffer::AddRecvMessage(google::protobuf::Message *message) {
  recv_message_ = message;
}

void CallOpBuffer::AddClientSendClose() {
  client_send_close_ = true;
}

void CallOpBuffer::AddClientRecvStatus(
    std::multimap<grpc::string, grpc::string>* metadata, Status *status) {
  recv_trailing_metadata_ = metadata;
  recv_status_ = status;
}

void CallOpBuffer::AddServerSendStatus(
    std::multimap<grpc::string, grpc::string>* metadata, const Status& status) {
  trailing_metadata_count_ = metadata->size();
  trailing_metadata_ = FillMetadataArray(metadata);
  send_status_ = &status;
}

void CallOpBuffer::FillOps(grpc_op *ops, size_t *nops) {
  *nops = 0;
  if (send_initial_metadata_) {
    ops[*nops].op = GRPC_OP_SEND_INITIAL_METADATA;
    ops[*nops].data.send_initial_metadata.count = initial_metadata_count_;
    ops[*nops].data.send_initial_metadata.metadata = initial_metadata_;
    (*nops)++;
  }
  if (recv_initial_metadata_) {
    ops[*nops].op = GRPC_OP_RECV_INITIAL_METADATA;
    ops[*nops].data.recv_initial_metadata = &recv_initial_metadata_arr_;
    (*nops)++;
  }
  if (send_message_) {
    bool success = SerializeProto(*send_message_, &send_message_buf_);
    if (!success) {
      // TODO handle parse failure
    }
    ops[*nops].op = GRPC_OP_SEND_MESSAGE;
    ops[*nops].data.send_message = send_message_buf_;
    (*nops)++;
  }
  if (recv_message_) {
    ops[*nops].op = GRPC_OP_RECV_MESSAGE;
    ops[*nops].data.recv_message = &recv_message_buf_;
    (*nops)++;
  }
  if (client_send_close_) {
    ops[*nops].op = GRPC_OP_SEND_CLOSE_FROM_CLIENT;
    (*nops)++;
  }
  if (recv_status_) {
    ops[*nops].op = GRPC_OP_RECV_STATUS_ON_CLIENT;
    ops[*nops].data.recv_status_on_client.trailing_metadata =
        &recv_trailing_metadata_arr_;
    ops[*nops].data.recv_status_on_client.status = &status_code_;
    ops[*nops].data.recv_status_on_client.status_details = &status_details_;
    ops[*nops].data.recv_status_on_client.status_details_capacity =
        &status_details_capacity_;
    (*nops)++;
  }
  if (send_status_) {
    ops[*nops].op = GRPC_OP_SEND_STATUS_FROM_SERVER;
    ops[*nops].data.send_status_from_server.trailing_metadata_count =
        trailing_metadata_count_;
    ops[*nops].data.send_status_from_server.trailing_metadata =
        trailing_metadata_;
    ops[*nops].data.send_status_from_server.status =
        static_cast<grpc_status_code>(send_status_->code());
    ops[*nops].data.send_status_from_server.status_details =
        send_status_->details().c_str();
    (*nops)++;
  }
}

void CallOpBuffer::FinalizeResult(void **tag, bool *status) {
  // Release send buffers.
  if (send_message_buf_) {
    grpc_byte_buffer_destroy(send_message_buf_);
    send_message_buf_ = nullptr;
  }
  if (initial_metadata_) {
    gpr_free(initial_metadata_);
    initial_metadata_ = nullptr;
  }
  if (trailing_metadata_count_) {
    gpr_free(trailing_metadata_);
    trailing_metadata_ = nullptr;
  }
  // Set user-facing tag.
  *tag = return_tag_;
  // Process received initial metadata
  if (recv_initial_metadata_) {
    FillMetadataMap(&recv_initial_metadata_arr_, recv_initial_metadata_);
  }
  // Parse received message if any.
  if (recv_message_ && recv_message_buf_) {
    *status = DeserializeProto(recv_message_buf_, recv_message_);
    grpc_byte_buffer_destroy(recv_message_buf_);
    recv_message_buf_ = nullptr;
  }
  // Parse received status.
  if (recv_status_) {
    FillMetadataMap(&recv_trailing_metadata_arr_, recv_trailing_metadata_);
    *recv_status_ = Status(
        static_cast<StatusCode>(status_code_),
        status_details_ ?  grpc::string(status_details_, status_details_capacity_)
                        :  grpc::string());
  }
}

void CCallDeleter::operator()(grpc_call* c) {
  grpc_call_destroy(c);
}

Call::Call(grpc_call* call, CallHook *call_hook, CompletionQueue* cq)
    : call_hook_(call_hook), cq_(cq), call_(call) {}

void Call::PerformOps(CallOpBuffer* buffer) {
  call_hook_->PerformOpsOnCall(buffer, this);
}

}  // namespace grpc
