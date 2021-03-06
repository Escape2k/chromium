// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WEBKIT_FILEAPI_FILE_SYSTEM_CONTEXT_H_
#define WEBKIT_FILEAPI_FILE_SYSTEM_CONTEXT_H_

#include <string>

#include "base/callback.h"
#include "base/memory/ref_counted.h"
#include "base/memory/scoped_ptr.h"
#include "base/platform_file.h"
#include "base/sequenced_task_runner_helpers.h"
#include "webkit/fileapi/file_system_types.h"
#include "webkit/quota/special_storage_policy.h"

class FilePath;
class GURL;

namespace base {
class MessageLoopProxy;
}

namespace quota {
class QuotaManagerProxy;
}

namespace webkit_blob {
class FileReader;
}

namespace fileapi {

class ExternalFileSystemMountPointProvider;
class FileSystemFileUtil;
class FileSystemMountPointProvider;
class FileSystemOperationInterface;
class FileSystemOptions;
class FileSystemPathManager;
class FileSystemQuotaUtil;
class IsolatedMountPointProvider;
class SandboxMountPointProvider;

struct DefaultContextDeleter;

// This class keeps and provides a file system context for FileSystem API.
// An instance of this class is created and owned by profile.
class FileSystemContext
    : public base::RefCountedThreadSafe<FileSystemContext,
                                        DefaultContextDeleter> {
 public:
  FileSystemContext(
      scoped_refptr<base::MessageLoopProxy> file_message_loop,
      scoped_refptr<base::MessageLoopProxy> io_message_loop,
      scoped_refptr<quota::SpecialStoragePolicy> special_storage_policy,
      quota::QuotaManagerProxy* quota_manager_proxy,
      const FilePath& profile_path,
      const FileSystemOptions& options);

  // This method can be called on any thread.
  bool DeleteDataForOriginOnFileThread(const GURL& origin_url);
  bool DeleteDataForOriginAndTypeOnFileThread(const GURL& origin_url,
                                              FileSystemType type);

  quota::QuotaManagerProxy* quota_manager_proxy() const {
    return quota_manager_proxy_.get();
  }

  // Returns a quota util for a given filesystem type.  This may
  // return NULL if the type does not support the usage tracking or
  // it is not a quota-managed storage.
  FileSystemQuotaUtil* GetQuotaUtil(FileSystemType type) const;

  // Returns the appropriate FileUtil instance for the given |type|.
  // This may return NULL if it is given an invalid or unsupported filesystem
  // type.
  FileSystemFileUtil* GetFileUtil(FileSystemType type) const;

  // Returns the mount point provider instance for the given |type|.
  // This may return NULL if it is given an invalid or unsupported filesystem
  // type.
  FileSystemMountPointProvider* GetMountPointProvider(
      FileSystemType type) const;

  // Returns a FileSystemMountPointProvider instance for sandboxed filesystem
  // types (e.g. TEMPORARY or PERSISTENT).  This is equivalent to calling
  // GetMountPointProvider(kFileSystemType{Temporary, Persistent}).
  SandboxMountPointProvider* sandbox_provider() const;

  // Returns a FileSystemMountPointProvider instance for external filesystem
  // type, which is used only by chromeos for now.  This is equivalent to
  // calling GetMountPointProvider(kFileSystemTypeExternal).
  ExternalFileSystemMountPointProvider* external_provider() const;

  // Used for OpenFileSystem.
  typedef base::Callback<void(base::PlatformFileError result,
                              const std::string& name,
                              const GURL& root)> OpenFileSystemCallback;

  // Opens the filesystem for the given |origin_url| and |type|, and dispatches
  // the DidOpenFileSystem callback of the given |dispatcher|.
  // If |create| is true this may actually set up a filesystem instance
  // (e.g. by creating the root directory or initializing the database
  // entry etc).
  void OpenFileSystem(
      const GURL& origin_url,
      FileSystemType type,
      bool create,
      OpenFileSystemCallback callback);

  // Creates a new FileSystemOperation instance by cracking
  // the given filesystem URL |url| to get an appropriate MountPointProvider
  // and calling the provider's corresponding CreateFileSystemOperation method.
  // The resolved MountPointProvider could perform further specialization
  // depending on the filesystem type pointed by the |url|.
  FileSystemOperationInterface* CreateFileSystemOperation(
      const GURL& url,
      base::MessageLoopProxy* file_proxy);

  // Creates new FileReader instance to read a file pointed by the given
  // filesystem URL |url| starting from |offset|.
  // This method internally cracks the |url|, get an appropriate
  // MountPointProvider for the URL and call the provider's CreateFileReader.
  // The resolved MountPointProvider could perform further specialization
  // depending on the filesystem type pointed by the |url|.
  webkit_blob::FileReader* CreateFileReader(
      const GURL& url,
      int64 offset,
      base::MessageLoopProxy* file_proxy);

 private:
  friend struct DefaultContextDeleter;
  friend class base::DeleteHelper<FileSystemContext>;
  ~FileSystemContext();

  void DeleteOnCorrectThread() const;

  scoped_refptr<base::MessageLoopProxy> file_message_loop_;
  scoped_refptr<base::MessageLoopProxy> io_message_loop_;

  scoped_refptr<quota::QuotaManagerProxy> quota_manager_proxy_;

  // Mount point providers.
  scoped_ptr<SandboxMountPointProvider> sandbox_provider_;
  scoped_ptr<IsolatedMountPointProvider> isolated_provider_;
  scoped_ptr<ExternalFileSystemMountPointProvider> external_provider_;

  DISALLOW_IMPLICIT_CONSTRUCTORS(FileSystemContext);
};

struct DefaultContextDeleter {
  static void Destruct(const FileSystemContext* context) {
    context->DeleteOnCorrectThread();
  }
};

}  // namespace fileapi

#endif  // WEBKIT_FILEAPI_FILE_SYSTEM_CONTEXT_H_
