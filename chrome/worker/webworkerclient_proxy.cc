// Copyright (c) 2009 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/worker/webworkerclient_proxy.h"

#include "base/command_line.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/common/webmessageportchannel_impl.h"
#include "chrome/common/worker_messages.h"
#include "chrome/renderer/webworker_proxy.h"
#include "chrome/worker/webworker_stub_base.h"
#include "chrome/worker/worker_thread.h"
#include "ipc/ipc_logging.h"
#include "webkit/api/public/WebString.h"
#include "webkit/api/public/WebURL.h"
#include "webkit/api/public/WebWorker.h"

using WebKit::WebMessagePortChannel;
using WebKit::WebMessagePortChannelArray;
using WebKit::WebString;
using WebKit::WebWorker;
using WebKit::WebWorkerClient;

// How long to wait for worker to finish after it's been told to terminate.
#define kMaxTimeForRunawayWorkerMs 3000

WebWorkerClientProxy::WebWorkerClientProxy(int route_id,
                                           WebWorkerStubBase* stub)
    : route_id_(route_id),
      stub_(stub),
      ALLOW_THIS_IN_INITIALIZER_LIST(kill_process_factory_(this)) {
}

WebWorkerClientProxy::~WebWorkerClientProxy() {
}

void WebWorkerClientProxy::postMessageToWorkerObject(
    const WebString& message,
    const WebMessagePortChannelArray& channels) {
  std::vector<int> message_port_ids(channels.size());
  std::vector<int> routing_ids(channels.size());
  for (size_t i = 0; i < channels.size(); ++i) {
    WebMessagePortChannelImpl* webchannel =
        static_cast<WebMessagePortChannelImpl*>(channels[i]);
    message_port_ids[i] = webchannel->message_port_id();
    webchannel->QueueMessages();
    DCHECK(message_port_ids[i] != MSG_ROUTING_NONE);
    routing_ids[i] = MSG_ROUTING_NONE;
  }

  Send(new WorkerMsg_PostMessage(
      route_id_, message, message_port_ids, routing_ids));
}

void WebWorkerClientProxy::postExceptionToWorkerObject(
    const WebString& error_message,
    int line_number,
    const WebString& source_url) {
  Send(new WorkerHostMsg_PostExceptionToWorkerObject(
      route_id_, error_message, line_number, source_url));
}

void WebWorkerClientProxy::postConsoleMessageToWorkerObject(
    int destination,
    int source,
    int type,
    int level,
    const WebString& message,
    int line_number,
    const WebString& source_url) {
  WorkerHostMsg_PostConsoleMessageToWorkerObject_Params params;
  params.destination_identifier = destination;
  params.source_identifier = source;
  params.message_type = type;
  params.message_level = level;
  params.message = message;
  params.line_number = line_number;
  params.source_url = source_url;
  Send(new WorkerHostMsg_PostConsoleMessageToWorkerObject(route_id_, params));
}

void WebWorkerClientProxy::confirmMessageFromWorkerObject(
    bool has_pending_activity) {
  Send(new WorkerHostMsg_ConfirmMessageFromWorkerObject(
      route_id_, has_pending_activity));
}

void WebWorkerClientProxy::reportPendingActivity(bool has_pending_activity) {
  Send(new WorkerHostMsg_ReportPendingActivity(
      route_id_, has_pending_activity));
}

void WebWorkerClientProxy::workerContextClosed() {
  // TODO(atwilson): Notify WorkerProcessHost that the worker context is closing
  // (needed for shared workers so we don't allow new connections).
}

void WebWorkerClientProxy::workerContextDestroyed() {
  Send(new WorkerHostMsg_WorkerContextDestroyed(route_id_));
  // Tell the stub that the worker has shutdown - frees this object.
  if (stub_)
    stub_->Shutdown();
}

WebKit::WebWorker* WebWorkerClientProxy::createWorker(
    WebKit::WebWorkerClient* client) {
  return new WebWorkerProxy(client, WorkerThread::current(), 0);
}

bool WebWorkerClientProxy::Send(IPC::Message* message) {
  return WorkerThread::current()->Send(message);
}

void WebWorkerClientProxy::EnsureWorkerContextTerminates() {
  // Avoid a worker doing a while(1) from never exiting.
  if (CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kWebWorkerShareProcesses)) {
    // Can't kill the process since there could be workers from other
    // renderer process.
    NOTIMPLEMENTED();
    return;
  }

  // This shuts down the process cleanly from the perspective of the browser
  // process, and avoids the crashed worker infobar from appearing to the new
  // page.
  MessageLoop::current()->PostDelayedTask(FROM_HERE,
      kill_process_factory_.NewRunnableMethod(
          &WebWorkerClientProxy::workerContextDestroyed),
          kMaxTimeForRunawayWorkerMs);
}

