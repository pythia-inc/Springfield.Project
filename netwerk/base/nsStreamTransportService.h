/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsStreamTransportService_h__
#define nsStreamTransportService_h__

#include "nsIStreamTransportService.h"
#include "nsIEventTarget.h"
#include "nsIObserver.h"
#include "nsCOMPtr.h"
#include "nsTArray.h"
#include "nsThreadUtils.h"
#include "mozilla/Attributes.h"
#include "mozilla/DataMutex.h"
#include "mozilla/Mutex.h"

class nsIThreadPool;

namespace mozilla {
namespace net {

class nsStreamTransportService final : public nsIStreamTransportService,
                                       public nsIEventTarget,
                                       public nsIObserver {
 public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSISTREAMTRANSPORTSERVICE
  NS_DECL_NSIEVENTTARGET_FULL
  NS_DECL_NSIOBSERVER

  nsresult Init();

  nsStreamTransportService();

 private:
  ~nsStreamTransportService();

  nsCOMPtr<nsIThreadPool> mPool MOZ_GUARDED_BY(mShutdownLock);

  mozilla::Mutex mShutdownLock{"nsStreamTransportService.mShutdownLock"};
  bool mIsShutdown MOZ_GUARDED_BY(mShutdownLock){false};
};

}  // namespace net
}  // namespace mozilla
#endif
