/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "GMPProcessParent.h"
#include "GMPUtils.h"
#include "nsIFile.h"
#include "nsIRunnable.h"

#include "base/string_util.h"
#include "base/process_util.h"

#include <string>

using std::vector;
using std::string;

using mozilla::gmp::GMPProcessParent;
using mozilla::ipc::GeckoChildProcessHost;
using base::ProcessArchitecture;

namespace mozilla {

extern LogModule* GetGMPLog();
#define GMP_LOG(msg, ...) MOZ_LOG(GetGMPLog(), mozilla::LogLevel::Debug, (msg, ##__VA_ARGS__))

namespace gmp {

GMPProcessParent::GMPProcessParent(const std::string& aGMPPath)
: GeckoChildProcessHost(GeckoProcessType_GMPlugin),
  mGMPPath(aGMPPath)
{
  MOZ_COUNT_CTOR(GMPProcessParent);
}

GMPProcessParent::~GMPProcessParent()
{
  MOZ_COUNT_DTOR(GMPProcessParent);
}

bool
GMPProcessParent::Launch(int32_t aTimeoutMs)
{
  nsCOMPtr<nsIFile> path;
  if (!GetEMEVoucherPath(getter_AddRefs(path))) {
    NS_WARNING("GMPProcessParent can't get EME voucher path!");
    return false;
  }
  nsAutoCString voucherPath;
  path->GetNativePath(voucherPath);

  vector<string> args;

  args.push_back(mGMPPath);

  args.push_back(string(voucherPath.BeginReading(), voucherPath.EndReading()));

  return SyncLaunch(args, aTimeoutMs, base::GetCurrentProcessArchitecture());
}

void
GMPProcessParent::Delete(nsCOMPtr<nsIRunnable> aCallback)
{
  mDeletedCallback = aCallback;
  XRE_GetIOMessageLoop()->PostTask(NewNonOwningRunnableMethod(this, &GMPProcessParent::DoDelete));
}

void
GMPProcessParent::DoDelete()
{
  MOZ_ASSERT(MessageLoop::current() == XRE_GetIOMessageLoop());
  Join();

  if (mDeletedCallback) {
    mDeletedCallback->Run();
  }

  delete this;
}

} // namespace gmp
} // namespace mozilla
