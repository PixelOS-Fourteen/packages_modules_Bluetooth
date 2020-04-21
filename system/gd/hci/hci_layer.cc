/*
 * Copyright 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "hci/hci_layer.h"

#include "common/bind.h"
#include "common/callback.h"
#include "os/alarm.h"
#include "os/queue.h"
#include "packet/packet_builder.h"

namespace bluetooth {
namespace hci {
using bluetooth::common::Bind;
using bluetooth::common::BindOn;
using bluetooth::common::BindOnce;
using bluetooth::common::Callback;
using bluetooth::common::Closure;
using bluetooth::common::ContextualCallback;
using bluetooth::common::ContextualOnceCallback;
using bluetooth::common::OnceCallback;
using bluetooth::common::OnceClosure;
using bluetooth::hci::CommandCompleteView;
using bluetooth::hci::CommandPacketBuilder;
using bluetooth::hci::CommandStatusView;
using bluetooth::hci::EventPacketView;
using bluetooth::hci::LeMetaEventView;
using bluetooth::os::Handler;
using common::BidiQueue;
using common::BidiQueueEnd;
using hci::OpCode;
using hci::ResetCompleteView;
using os::Alarm;
using os::Handler;

static void fail_if_reset_complete_not_success(CommandCompleteView complete) {
  auto reset_complete = ResetCompleteView::Create(complete);
  ASSERT(reset_complete.IsValid());
  ASSERT(reset_complete.GetStatus() == ErrorCode::SUCCESS);
}

static void on_hci_timeout(OpCode op_code) {
  ASSERT_LOG(false, "Timed out waiting for 0x%02hx (%s)", op_code, OpCodeText(op_code).c_str());
}

class CommandQueueEntry {
 public:
  CommandQueueEntry(std::unique_ptr<CommandPacketBuilder> command_packet,
                    ContextualOnceCallback<void(CommandCompleteView)> on_complete_function)
      : command(std::move(command_packet)), waiting_for_status_(false), on_complete(std::move(on_complete_function)) {}

  CommandQueueEntry(std::unique_ptr<CommandPacketBuilder> command_packet,
                    ContextualOnceCallback<void(CommandStatusView)> on_status_function)
      : command(std::move(command_packet)), waiting_for_status_(true), on_status(std::move(on_status_function)) {}

  std::unique_ptr<CommandPacketBuilder> command;
  bool waiting_for_status_;
  ContextualOnceCallback<void(CommandStatusView)> on_status;
  ContextualOnceCallback<void(CommandCompleteView)> on_complete;
};

template <typename T>
class CommandInterfaceImpl : public CommandInterface<T> {
 public:
  explicit CommandInterfaceImpl(HciLayer& hci) : hci_(hci) {}
  ~CommandInterfaceImpl() override = default;

  void EnqueueCommand(std::unique_ptr<T> command,
                      ContextualOnceCallback<void(CommandCompleteView)> on_complete) override {
    hci_.EnqueueCommand(std::move(command), std::move(on_complete));
  }

  void EnqueueCommand(std::unique_ptr<T> command, ContextualOnceCallback<void(CommandStatusView)> on_status) override {
    hci_.EnqueueCommand(std::move(command), std::move(on_status));
  }
  HciLayer& hci_;
};

struct HciLayer::impl {
  impl(hal::HciHal* hal, HciLayer& module) : hal_(hal), module_(module) {
    hci_timeout_alarm_ = new Alarm(module.GetHandler());
  }

  ~impl() {
    incoming_acl_packet_buffer_.Clear();
    delete hci_timeout_alarm_;
    command_queue_.clear();
  }

  void drop(EventPacketView) {}

  void on_outbound_acl_ready() {
    auto packet = acl_queue_.GetDownEnd()->TryDequeue();
    std::vector<uint8_t> bytes;
    BitInserter bi(bytes);
    packet->Serialize(bi);
    hal_->sendAclData(bytes);
  }

  void on_command_status(EventPacketView event) {
    CommandStatusView status_view = CommandStatusView::Create(event);
    ASSERT(status_view.IsValid());
    command_credits_ = status_view.GetNumHciCommandPackets();
    OpCode op_code = status_view.GetCommandOpCode();
    if (op_code == OpCode::NONE) {
      send_next_command();
      return;
    }
    ASSERT_LOG(!command_queue_.empty(), "Unexpected status event with OpCode 0x%02hx (%s)", op_code,
               OpCodeText(op_code).c_str());
    ASSERT_LOG(waiting_command_ == op_code, "Waiting for 0x%02hx (%s), got 0x%02hx (%s)", waiting_command_,
               OpCodeText(waiting_command_).c_str(), op_code, OpCodeText(op_code).c_str());
    ASSERT_LOG(command_queue_.front().waiting_for_status_,
               "Waiting for command complete 0x%02hx (%s), got command status for 0x%02hx (%s)", waiting_command_,
               OpCodeText(waiting_command_).c_str(), op_code, OpCodeText(op_code).c_str());
    command_queue_.front().on_status.Invoke(std::move(status_view));
    command_queue_.pop_front();
    waiting_command_ = OpCode::NONE;
    hci_timeout_alarm_->Cancel();
    send_next_command();
  }

  void on_command_complete(EventPacketView event) {
    CommandCompleteView complete_view = CommandCompleteView::Create(event);
    ASSERT(complete_view.IsValid());
    command_credits_ = complete_view.GetNumHciCommandPackets();
    OpCode op_code = complete_view.GetCommandOpCode();
    if (op_code == OpCode::NONE) {
      send_next_command();
      return;
    }
    ASSERT_LOG(command_queue_.size() > 0, "Unexpected command complete with OpCode 0x%02hx (%s)", op_code,
               OpCodeText(op_code).c_str());
    ASSERT_LOG(waiting_command_ == op_code, "Waiting for 0x%02hx (%s), got 0x%02hx (%s)", waiting_command_,
               OpCodeText(waiting_command_).c_str(), op_code, OpCodeText(op_code).c_str());
    ASSERT_LOG(!command_queue_.front().waiting_for_status_,
               "Waiting for command status 0x%02hx (%s), got command complete for 0x%02hx (%s)", waiting_command_,
               OpCodeText(waiting_command_).c_str(), op_code, OpCodeText(op_code).c_str());
    command_queue_.front().on_complete.Invoke(std::move(complete_view));
    command_queue_.pop_front();
    waiting_command_ = OpCode::NONE;
    hci_timeout_alarm_->Cancel();
    send_next_command();
  }

  void on_le_meta_event(EventPacketView event) {
    LeMetaEventView meta_event_view = LeMetaEventView::Create(event);
    ASSERT(meta_event_view.IsValid());
    SubeventCode subevent_code = meta_event_view.GetSubeventCode();
    ASSERT_LOG(subevent_handlers_.find(subevent_code) != subevent_handlers_.end(),
               "Unhandled le event of type 0x%02hhx (%s)", subevent_code, SubeventCodeText(subevent_code).c_str());
    subevent_handlers_[subevent_code].Invoke(meta_event_view);
  }

  void on_hci_event(EventPacketView event) {
    EventCode event_code = event.GetEventCode();
    if (event_handlers_.find(event_code) == event_handlers_.end()) {
      LOG_DEBUG("Dropping unregistered event of type 0x%02hhx (%s)", event_code, EventCodeText(event_code).c_str());
      return;
    }
    event_handlers_[event_code].Invoke(event);
  }

  void handle_enqueue_command_with_complete(std::unique_ptr<CommandPacketBuilder> command,
                                            ContextualOnceCallback<void(CommandCompleteView)> on_complete) {
    command_queue_.emplace_back(std::move(command), std::move(on_complete));

    send_next_command();
  }

  void handle_enqueue_command_with_status(std::unique_ptr<CommandPacketBuilder> command,
                                          ContextualOnceCallback<void(CommandStatusView)> on_status) {
    command_queue_.emplace_back(std::move(command), std::move(on_status));

    send_next_command();
  }

  void send_next_command() {
    if (command_credits_ == 0) {
      return;
    }
    if (waiting_command_ != OpCode::NONE) {
      return;
    }
    if (command_queue_.size() == 0) {
      return;
    }
    std::shared_ptr<std::vector<uint8_t>> bytes = std::make_shared<std::vector<uint8_t>>();
    BitInserter bi(*bytes);
    command_queue_.front().command->Serialize(bi);
    hal_->sendHciCommand(*bytes);
    auto cmd_view = CommandPacketView::Create(bytes);
    ASSERT(cmd_view.IsValid());
    OpCode op_code = cmd_view.GetOpCode();
    waiting_command_ = op_code;
    command_credits_ = 0;  // Only allow one outstanding command
    hci_timeout_alarm_->Schedule(BindOnce(&on_hci_timeout, op_code), kHciTimeoutMs);
  }

  void handle_register_event_handler(EventCode event_code, ContextualCallback<void(EventPacketView)> event_handler) {
    ASSERT_LOG(event_handlers_.count(event_code) == 0, "Can not register a second handler for event_code %02hhx (%s)",
               event_code, EventCodeText(event_code).c_str());
    event_handlers_[event_code] = event_handler;
  }

  void handle_unregister_event_handler(EventCode event_code) {
    event_handlers_.erase(event_handlers_.find(event_code));
  }

  void handle_register_le_event_handler(SubeventCode subevent_code,
                                        ContextualCallback<void(LeMetaEventView)> subevent_handler) {
    ASSERT_LOG(subevent_handlers_.count(subevent_code) == 0,
               "Can not register a second handler for subevent_code %02hhx (%s)", subevent_code,
               SubeventCodeText(subevent_code).c_str());
    subevent_handlers_[subevent_code] = subevent_handler;
  }

  void handle_unregister_le_event_handler(SubeventCode subevent_code) {
    subevent_handlers_.erase(subevent_handlers_.find(subevent_code));
  }

  hal::HciHal* hal_;
  HciLayer& module_;

  // Interfaces
  CommandInterfaceImpl<ConnectionManagementCommandBuilder> acl_connection_manager_interface_{module_};
  CommandInterfaceImpl<LeConnectionManagementCommandBuilder> le_acl_connection_manager_interface_{module_};
  CommandInterfaceImpl<SecurityCommandBuilder> security_interface{module_};
  CommandInterfaceImpl<LeSecurityCommandBuilder> le_security_interface{module_};
  CommandInterfaceImpl<LeAdvertisingCommandBuilder> le_advertising_interface{module_};
  CommandInterfaceImpl<LeScanningCommandBuilder> le_scanning_interface{module_};

  // Command Handling
  std::list<CommandQueueEntry> command_queue_;

  std::map<EventCode, ContextualCallback<void(EventPacketView)>> event_handlers_;
  std::map<SubeventCode, ContextualCallback<void(LeMetaEventView)>> subevent_handlers_;
  OpCode waiting_command_{OpCode::NONE};
  uint8_t command_credits_{1};  // Send reset first
  Alarm* hci_timeout_alarm_{nullptr};

  // Acl packets
  BidiQueue<AclPacketView, AclPacketBuilder> acl_queue_{3 /* TODO: Set queue depth */};
  os::EnqueueBuffer<AclPacketView> incoming_acl_packet_buffer_{acl_queue_.GetDownEnd()};
};

// All functions here are running on the HAL thread
struct HciLayer::hal_callbacks : public hal::HciHalCallbacks {
  hal_callbacks(HciLayer& module) : module_(module) {}

  void hciEventReceived(hal::HciPacket event_bytes) override {
    auto packet = packet::PacketView<packet::kLittleEndian>(std::make_shared<std::vector<uint8_t>>(event_bytes));
    EventPacketView event = EventPacketView::Create(packet);
    ASSERT(event.IsValid());
    module_.CallOn(module_.impl_, &impl::on_hci_event, std::move(event));
  }

  void aclDataReceived(hal::HciPacket data_bytes) override {
    auto packet =
        packet::PacketView<packet::kLittleEndian>(std::make_shared<std::vector<uint8_t>>(std::move(data_bytes)));
    AclPacketView acl = AclPacketView::Create(packet);
    module_.impl_->incoming_acl_packet_buffer_.Enqueue(std::make_unique<AclPacketView>(acl), module_.GetHandler());
  }

  void scoDataReceived(hal::HciPacket data_bytes) override {
    // Not implemented yet
  }

  HciLayer& module_;
};

HciLayer::HciLayer() : impl_(nullptr), hal_callbacks_(nullptr) {}

HciLayer::~HciLayer() {
}

void HciLayer::EnqueueCommand(std::unique_ptr<CommandPacketBuilder> command,
                              common::ContextualOnceCallback<void(CommandCompleteView)> on_complete) {
  CallOn(impl_, &impl::handle_enqueue_command_with_complete, std::move(command), std::move(on_complete));
}

void HciLayer::EnqueueCommand(std::unique_ptr<CommandPacketBuilder> command,
                              common::ContextualOnceCallback<void(CommandStatusView)> on_status) {
  CallOn(impl_, &impl::handle_enqueue_command_with_status, std::move(command), std::move(on_status));
}

common::BidiQueueEnd<AclPacketBuilder, AclPacketView>* HciLayer::GetAclQueueEnd() {
  return impl_->acl_queue_.GetUpEnd();
}

void HciLayer::RegisterEventHandler(EventCode event_code, ContextualCallback<void(EventPacketView)> event_handler) {
  CallOn(impl_, &impl::handle_register_event_handler, event_code, event_handler);
}

void HciLayer::UnregisterEventHandler(EventCode event_code) {
  CallOn(impl_, &impl::handle_unregister_event_handler, event_code);
}

void HciLayer::RegisterLeEventHandler(SubeventCode subevent_code,
                                      ContextualCallback<void(LeMetaEventView)> event_handler) {
  CallOn(impl_, &impl::handle_register_le_event_handler, subevent_code, event_handler);
}

void HciLayer::UnregisterLeEventHandler(SubeventCode subevent_code) {
  CallOn(impl_, &impl::handle_unregister_le_event_handler, subevent_code);
}

AclConnectionInterface* HciLayer::GetAclConnectionInterface(
    ContextualCallback<void(EventPacketView)> event_handler,
    ContextualCallback<void(uint16_t, ErrorCode)> on_disconnect) {
  for (const auto event : AclConnectionEvents) {
    RegisterEventHandler(event, event_handler);
  }
  return &impl_->acl_connection_manager_interface_;
}

LeAclConnectionInterface* HciLayer::GetLeAclConnectionInterface(
    ContextualCallback<void(LeMetaEventView)> event_handler,
    ContextualCallback<void(uint16_t, ErrorCode)> on_disconnect) {
  for (const auto event : LeConnectionManagementEvents) {
    RegisterLeEventHandler(event, event_handler);
  }
  return &impl_->le_acl_connection_manager_interface_;
}

SecurityInterface* HciLayer::GetSecurityInterface(ContextualCallback<void(EventPacketView)> event_handler) {
  for (const auto event : SecurityEvents) {
    RegisterEventHandler(event, event_handler);
  }
  return &impl_->security_interface;
}

LeSecurityInterface* HciLayer::GetLeSecurityInterface(ContextualCallback<void(LeMetaEventView)> event_handler) {
  for (const auto subevent : LeSecurityEvents) {
    RegisterLeEventHandler(subevent, event_handler);
  }
  return &impl_->le_security_interface;
}

LeAdvertisingInterface* HciLayer::GetLeAdvertisingInterface(ContextualCallback<void(LeMetaEventView)> event_handler) {
  for (const auto subevent : LeAdvertisingEvents) {
    RegisterLeEventHandler(subevent, event_handler);
  }
  return &impl_->le_advertising_interface;
}

LeScanningInterface* HciLayer::GetLeScanningInterface(ContextualCallback<void(LeMetaEventView)> event_handler) {
  for (const auto subevent : LeScanningEvents) {
    RegisterLeEventHandler(subevent, event_handler);
  }
  return &impl_->le_scanning_interface;
}

const ModuleFactory HciLayer::Factory = ModuleFactory([]() { return new HciLayer(); });

void HciLayer::ListDependencies(ModuleList* list) {
  list->add<hal::HciHal>();
}

void HciLayer::Start() {
  auto hal = GetDependency<hal::HciHal>();
  impl_ = new impl(hal, *this);
  hal_callbacks_ = new hal_callbacks(*this);

  Handler* handler = GetHandler();
  impl_->acl_queue_.GetDownEnd()->RegisterDequeue(handler, BindOn(impl_, &impl::on_outbound_acl_ready));
  RegisterEventHandler(EventCode::COMMAND_COMPLETE, handler->BindOn(impl_, &impl::on_command_complete));
  RegisterEventHandler(EventCode::COMMAND_STATUS, handler->BindOn(impl_, &impl::on_command_status));
  RegisterEventHandler(EventCode::LE_META_EVENT, handler->BindOn(impl_, &impl::on_le_meta_event));
  // TODO find the right place
  auto drop_packet = handler->BindOn(impl_, &impl::drop);
  RegisterEventHandler(EventCode::PAGE_SCAN_REPETITION_MODE_CHANGE, drop_packet);
  RegisterEventHandler(EventCode::MAX_SLOTS_CHANGE, drop_packet);
  RegisterEventHandler(EventCode::VENDOR_SPECIFIC, drop_packet);

  EnqueueCommand(ResetBuilder::Create(), handler->BindOnce(&fail_if_reset_complete_not_success));
  hal->registerIncomingPacketCallback(hal_callbacks_);
}

void HciLayer::Stop() {
  auto hal = GetDependency<hal::HciHal>();
  hal->unregisterIncomingPacketCallback();
  delete hal_callbacks_;

  impl_->acl_queue_.GetDownEnd()->UnregisterDequeue();
  delete impl_;
}

}  // namespace hci
}  // namespace bluetooth
