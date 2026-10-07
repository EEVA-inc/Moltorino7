#pragma once

#include "providers/tiktok/TikTokApi.hpp"

namespace chatterino {

ExpectedStr<void> signTikTokAccountRequest(const TikTokSession &session,
                                           TikTokApiRequest &request,
                                           bool provider);
ExpectedStr<void> validateTikTokSigningSession(const TikTokSession &session);
void updateTikTokTicketGuard(TikTokSession &session,
                             const QByteArray &serverData);
}
