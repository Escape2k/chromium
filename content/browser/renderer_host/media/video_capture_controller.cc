// Copyright (c) 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/renderer_host/media/video_capture_controller.h"

#include "base/bind.h"
#include "base/stl_util.h"
#include "content/browser/renderer_host/media/media_stream_manager.h"
#include "content/browser/renderer_host/media/video_capture_manager.h"
#include "content/public/browser/browser_thread.h"
#include "media/base/yuv_convert.h"

using content::BrowserThread;

// The number of DIBs VideoCaptureController allocate.
static const size_t kNoOfDIBS = 3;

struct VideoCaptureController::ControllerClient {
  ControllerClient(
      const VideoCaptureControllerID& id,
      VideoCaptureControllerEventHandler* handler,
      base::ProcessHandle render_process,
      const media::VideoCaptureParams& params)
      : controller_id(id),
        event_handler(handler),
        render_process_handle(render_process),
        parameters(params),
        report_ready_to_delete(false) {
  }

  ~ControllerClient() {}

  // ID used for identifying this object.
  VideoCaptureControllerID controller_id;
  VideoCaptureControllerEventHandler* event_handler;

  // Handle to the render process that will receive the DIBs.
  base::ProcessHandle render_process_handle;
  media::VideoCaptureParams parameters;

  // Buffers used by this client.
  std::list<int> buffers;

  // Record client's status when it has called StopCapture, but haven't
  // returned all buffers.
  bool report_ready_to_delete;
};

struct VideoCaptureController::SharedDIB {
  SharedDIB(base::SharedMemory* ptr)
      : shared_memory(ptr),
        references(0) {
  }

  ~SharedDIB() {}

  // The memory created to be shared with renderer processes.
  scoped_ptr<base::SharedMemory> shared_memory;

  // Number of renderer processes which hold this shared memory.
  // renderer process is represented by VidoeCaptureHost.
  int references;
};

VideoCaptureController::VideoCaptureController(
    media_stream::VideoCaptureManager* video_capture_manager)
    : frame_info_available_(false),
      video_capture_manager_(video_capture_manager),
      device_in_use_(false),
      state_(media::VideoCapture::kStopped) {
  memset(&current_params_, 0, sizeof(current_params_));
}

VideoCaptureController::~VideoCaptureController() {
  // Delete all DIBs.
  STLDeleteContainerPairSecondPointers(owned_dibs_.begin(),
                                       owned_dibs_.end());
  STLDeleteContainerPointers(controller_clients_.begin(),
                             controller_clients_.end());
  STLDeleteContainerPointers(pending_clients_.begin(),
                             pending_clients_.end());
}

void VideoCaptureController::StartCapture(
    const VideoCaptureControllerID& id,
    VideoCaptureControllerEventHandler* event_handler,
    base::ProcessHandle render_process,
    const media::VideoCaptureParams& params) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));
  // Signal error in case device is already in error state.
  if (state_ == media::VideoCapture::kError) {
    event_handler->OnError(id);
    return;
  }

  // Do nothing if this client has called StartCapture before.
  if (FindClient(id, event_handler, controller_clients_) ||
      FindClient(id, event_handler, pending_clients_))
    return;

  ControllerClient* client = new ControllerClient(id, event_handler,
                                                  render_process, params);
  // In case capture has been started, need to check different condtions.
  if (state_ == media::VideoCapture::kStarted) {
    // This client has higher resolution than what is currently requested.
    // Need restart capturing.
    if (params.width > current_params_.width ||
        params.height > current_params_.height) {
      video_capture_manager_->Stop(current_params_.session_id,
          base::Bind(&VideoCaptureController::OnDeviceStopped, this));
      frame_info_available_ = false;
      state_ = media::VideoCapture::kStopping;
      pending_clients_.push_back(client);
      return;
    }

    // This client's resolution is no larger than what's currently requested.
    // When frame_info has been returned by device, send them to client.
    if (frame_info_available_) {
      int buffer_size = (frame_info_.width * frame_info_.height * 3) / 2;
      SendFrameInfoAndBuffers(client, buffer_size);
    }
    controller_clients_.push_back(client);
    return;
  }

  // In case the device is in the middle of stopping, put the client in
  // pending queue.
  if (state_ == media::VideoCapture::kStopping) {
    pending_clients_.push_back(client);
    return;
  }

  // Fresh start.
  controller_clients_.push_back(client);
  current_params_ = params;
  // Order the manager to start the actual capture.
  video_capture_manager_->Start(params, this);
  state_ = media::VideoCapture::kStarted;
}

void VideoCaptureController::StopCapture(
    const VideoCaptureControllerID& id,
    VideoCaptureControllerEventHandler* event_handler,
    bool force_buffer_return) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));

  ControllerClient* client = FindClient(id, event_handler, pending_clients_);
  // If the client is still in pending queue, just remove it.
  if (client) {
    client->event_handler->OnReadyToDelete(client->controller_id);
    pending_clients_.remove(client);
    return;
  }

  client = FindClient(id, event_handler, controller_clients_);
  DCHECK(client) << "Client should have called StartCapture()";

  if (force_buffer_return) {
    // The client requests to return buffers which means it can't return
    // buffers normally. After buffers are returned, client is free to
    // delete itself. No need to call OnReadyToDelete.

    // Return all buffers held by the clients.
    for (std::list<int>::iterator buffer_it = client->buffers.begin();
         buffer_it != client->buffers.end(); ++buffer_it) {
      int buffer_id = *buffer_it;
      DIBMap::iterator dib_it = owned_dibs_.find(buffer_id);
      if (dib_it == owned_dibs_.end())
        continue;

      {
        base::AutoLock lock(lock_);
        DCHECK_GT(dib_it->second->references, 0)
            << "The buffer is not used by renderer.";
        dib_it->second->references -= 1;
      }
    }
    client->buffers.clear();
  } else {
    // Normal way to stop capture.
    if (!client->buffers.empty()) {
      // There are still some buffers held by the client.
      client->report_ready_to_delete = true;
      return;
    }
    // No buffer is held by the client. Ready to delete.
    client->event_handler->OnReadyToDelete(client->controller_id);
  }

  delete client;
  controller_clients_.remove(client);

  // No more clients. Stop device.
  if (controller_clients_.empty()) {
    video_capture_manager_->Stop(current_params_.session_id,
        base::Bind(&VideoCaptureController::OnDeviceStopped, this));
    frame_info_available_ = false;
    state_ = media::VideoCapture::kStopping;
  }
}

void VideoCaptureController::ReturnBuffer(
    const VideoCaptureControllerID& id,
    VideoCaptureControllerEventHandler* event_handler,
    int buffer_id) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));

  ControllerClient* client = FindClient(id, event_handler,
                                        controller_clients_);
  DIBMap::iterator dib_it = owned_dibs_.find(buffer_id);
  // If this buffer is not held by this client, or this client doesn't exist
  // in controller, do nothing.
  if (!client || dib_it == owned_dibs_.end())
    return;

  client->buffers.remove(buffer_id);
  // If this client has called StopCapture and doesn't hold any buffer after
  // after this return, ready to delete this client.
  if (client->report_ready_to_delete && client->buffers.empty()) {
    client->event_handler->OnReadyToDelete(client->controller_id);
    delete client;
    controller_clients_.remove(client);
  }
  {
    base::AutoLock lock(lock_);
    DCHECK_GT(dib_it->second->references, 0)
        << "The buffer is not used by renderer.";
    dib_it->second->references -= 1;
    if (dib_it->second->references > 0)
      return;
  }

  // When all buffers have been returned by clients and device has been
  // called to stop, check if restart is needed. This could happen when
  // some clients call StopCapture before returning all buffers.
  if (!ClientHasDIB() && state_ == media::VideoCapture::kStopping) {
    PostStopping();
  }
}

///////////////////////////////////////////////////////////////////////////////
// Implements VideoCaptureDevice::EventHandler.
// OnIncomingCapturedFrame is called the thread running the capture device.
// I.e.- DirectShow thread on windows and v4l2_thread on Linux.
void VideoCaptureController::OnIncomingCapturedFrame(const uint8* data,
                                                     int length,
                                                     base::Time timestamp) {
  int buffer_id = 0;
  base::SharedMemory* dib = NULL;
  {
    base::AutoLock lock(lock_);
    for (DIBMap::iterator dib_it = owned_dibs_.begin();
         dib_it != owned_dibs_.end(); dib_it++) {
      if (dib_it->second->references == 0) {
        buffer_id = dib_it->first;
        // Use special value "-1" in order to not be treated as buffer at
        // renderer side.
        dib_it->second->references = -1;
        dib = dib_it->second->shared_memory.get();
        break;
      }
    }
  }

  if (!dib) {
    return;
  }

  uint8* target = static_cast<uint8*>(dib->memory());
  CHECK(dib->created_size() >= static_cast<size_t> (frame_info_.width *
                                                    frame_info_.height * 3) /
                                                    2);
  uint8* yplane = target;
  uint8* uplane = target + frame_info_.width * frame_info_.height;
  uint8* vplane = uplane + (frame_info_.width * frame_info_.height) / 4;

  // Do color conversion from the camera format to I420.
  switch (frame_info_.color) {
    case media::VideoCaptureDevice::kColorUnknown:  // Color format not set.
      break;
    case media::VideoCaptureDevice::kI420: {
      memcpy(target, data, (frame_info_.width * frame_info_.height * 3) / 2);
      break;
    }
    case media::VideoCaptureDevice::kYUY2: {
      media::ConvertYUY2ToYUV(data, yplane, uplane, vplane, frame_info_.width,
                              frame_info_.height);
      break;
    }
    case media::VideoCaptureDevice::kRGB24: {
      int ystride = frame_info_.width;
      int uvstride = frame_info_.width / 2;
#if defined(OS_WIN)  // RGB on Windows start at the bottom line.
      int rgb_stride = -3 * frame_info_.width;
      const uint8* rgb_src = data + 3 * frame_info_.width *
          (frame_info_.height -1);
#else
      int rgb_stride = 3 * frame_info_.width;
      const uint8* rgb_src = data;
#endif
      media::ConvertRGB24ToYUV(rgb_src, yplane, uplane, vplane,
                               frame_info_.width, frame_info_.height,
                               rgb_stride, ystride, uvstride);
      break;
    }
    case media::VideoCaptureDevice::kARGB: {
      media::ConvertRGB32ToYUV(data, yplane, uplane, vplane, frame_info_.width,
                               frame_info_.height, frame_info_.width * 4,
                               frame_info_.width, frame_info_.width / 2);
      break;
    }
    default:
      NOTREACHED();
  }

  BrowserThread::PostTask(BrowserThread::IO,
      FROM_HERE,
      base::Bind(&VideoCaptureController::DoIncomingCapturedFrameOnIOThread,
                 this, buffer_id, timestamp));
}

void VideoCaptureController::OnError() {
  video_capture_manager_->Error(current_params_.session_id);
  BrowserThread::PostTask(BrowserThread::IO,
      FROM_HERE,
      base::Bind(&VideoCaptureController::DoErrorOnIOThread, this));
}

void VideoCaptureController::OnFrameInfo(
    const media::VideoCaptureDevice::Capability& info) {
  BrowserThread::PostTask(BrowserThread::IO,
      FROM_HERE,
      base::Bind(&VideoCaptureController::DoFrameInfoOnIOThread,
                 this, info));
}

void VideoCaptureController::DoIncomingCapturedFrameOnIOThread(
    int buffer_id, base::Time timestamp) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));

  int count = 0;
  if (state_ == media::VideoCapture::kStarted) {
    for (ControllerClients::iterator client_it = controller_clients_.begin();
         client_it != controller_clients_.end(); client_it++) {
      if ((*client_it)->report_ready_to_delete)
        continue;

      (*client_it)->event_handler->OnBufferReady((*client_it)->controller_id,
                                                 buffer_id, timestamp);
      (*client_it)->buffers.push_back(buffer_id);
      count++;
    }
  }

  base::AutoLock lock(lock_);
  DCHECK_EQ(owned_dibs_[buffer_id]->references, -1);
  owned_dibs_[buffer_id]->references = count;
}

void VideoCaptureController::DoErrorOnIOThread() {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));
  state_ = media::VideoCapture::kError;
  ControllerClients::iterator client_it;
  for (client_it = controller_clients_.begin();
       client_it != controller_clients_.end(); client_it++) {
    (*client_it)->event_handler->OnError((*client_it)->controller_id);
  }
  for (client_it = pending_clients_.begin();
       client_it != pending_clients_.end(); client_it++) {
    (*client_it)->event_handler->OnError((*client_it)->controller_id);
  }
}

void VideoCaptureController::DoFrameInfoOnIOThread(
    const media::VideoCaptureDevice::Capability info) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));
  DCHECK(owned_dibs_.empty())
      << "Device is restarted without releasing shared memory.";

  bool frames_created = true;
  const size_t needed_size = (info.width * info.height * 3) / 2;
  {
    base::AutoLock lock(lock_);
    for (size_t i = 1; i <= kNoOfDIBS; ++i) {
      base::SharedMemory* shared_memory = new base::SharedMemory();
      if (!shared_memory->CreateAndMapAnonymous(needed_size)) {
        frames_created = false;
        break;
      }
      SharedDIB* dib = new SharedDIB(shared_memory);
      owned_dibs_.insert(std::make_pair(i, dib));
    }
  }
  // Check whether all DIBs were created successfully.
  if (!frames_created) {
    state_ = media::VideoCapture::kError;
    for (ControllerClients::iterator client_it = controller_clients_.begin();
         client_it != controller_clients_.end(); client_it++) {
      (*client_it)->event_handler->OnError((*client_it)->controller_id);
    }
    return;
  }
  frame_info_= info;
  frame_info_available_ = true;

  for (ControllerClients::iterator client_it = controller_clients_.begin();
       client_it != controller_clients_.end(); client_it++) {
    SendFrameInfoAndBuffers((*client_it), static_cast<int>(needed_size));
  }
}

void VideoCaptureController::SendFrameInfoAndBuffers(
    ControllerClient* client, int buffer_size) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));
  DCHECK(frame_info_available_);
  client->event_handler->OnFrameInfo(client->controller_id,
                                     frame_info_.width, frame_info_.height,
                                     frame_info_.frame_rate);
  for (DIBMap::iterator dib_it = owned_dibs_.begin();
       dib_it != owned_dibs_.end(); dib_it++) {
    base::SharedMemory* shared_memory = dib_it->second->shared_memory.get();
    int index = dib_it->first;
    base::SharedMemoryHandle remote_handle;
    shared_memory->ShareToProcess(client->render_process_handle,
                                  &remote_handle);
    client->event_handler->OnBufferCreated(client->controller_id,
                                           remote_handle,
                                           buffer_size,
                                           index);
  }
}

// This function is called when all buffers have been returned to controller,
// or when device is stopped. It decides whether the device needs to be
// restarted.
void VideoCaptureController::PostStopping() {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));
  DCHECK_EQ(state_, media::VideoCapture::kStopping);

  // When clients still have some buffers, or device has not been stopped yet,
  // do nothing.
  if (ClientHasDIB() || device_in_use_)
    return;

  // It's safe to free all DIB's on IO thread since device won't send
  // buffer over.
  STLDeleteValues(&owned_dibs_);

  // No more client. Therefore the controller is stopped.
  if (controller_clients_.empty() && pending_clients_.empty()) {
    state_ = media::VideoCapture::kStopped;
    return;
  }

  // Restart the device.
  current_params_.width = 0;
  current_params_.height = 0;
  ControllerClients::iterator client_it;
  for (client_it = controller_clients_.begin();
       client_it != controller_clients_.end(); client_it++) {
    if (current_params_.width < (*client_it)->parameters.width)
      current_params_.width = (*client_it)->parameters.width;
    if (current_params_.height < (*client_it)->parameters.height)
      current_params_.height = (*client_it)->parameters.height;
  }
  for (client_it = pending_clients_.begin();
       client_it != pending_clients_.end(); ) {
    if (current_params_.width < (*client_it)->parameters.width)
      current_params_.width = (*client_it)->parameters.width;
    if (current_params_.height < (*client_it)->parameters.height)
      current_params_.height = (*client_it)->parameters.height;
    controller_clients_.push_back((*client_it));
    pending_clients_.erase(client_it++);
  }
  // Request the manager to start the actual capture.
  video_capture_manager_->Start(current_params_, this);
  state_ = media::VideoCapture::kStarted;
}

bool VideoCaptureController::ClientHasDIB() {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));
  base::AutoLock lock(lock_);
  for (DIBMap::iterator dib_it = owned_dibs_.begin();
       dib_it != owned_dibs_.end(); dib_it++) {
    if (dib_it->second->references > 0)
      return true;
  }
  return false;
}

VideoCaptureController::ControllerClient*
VideoCaptureController::FindClient(
    const VideoCaptureControllerID& id,
    VideoCaptureControllerEventHandler* handler,
    const ControllerClients& clients) {
  for (ControllerClients::const_iterator client_it = clients.begin();
       client_it != clients.end(); client_it++) {
    if ((*client_it)->controller_id == id &&
        (*client_it)->event_handler == handler) {
      return *client_it;
    }
  }
  return NULL;
}

///////////////////////////////////////////////////////////////////////////////
// Called by VideoCaptureManager when a device have been stopped.
void VideoCaptureController::OnDeviceStopped() {
  BrowserThread::PostTask(BrowserThread::IO,
      FROM_HERE,
      base::Bind(&VideoCaptureController::DoDeviceStoppedOnIOThread, this));
}

void VideoCaptureController::DoDeviceStoppedOnIOThread() {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));
  device_in_use_ = false;
  if (state_ == media::VideoCapture::kStopping) {
    PostStopping();
  }
}

