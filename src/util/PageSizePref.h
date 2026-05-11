#pragma once

#include <QSettings>

#include <algorithm>

namespace fc::util {

// User-configurable "messages per batch" knob. The same value drives:
//
//   - MessageListModel::pageSize()           cache read chunk
//   - SyncService::topUpPageSize()           Gmail messages.list maxResults
//
// Shared via a header-only helper so the sync layer can read it without
// taking an ui/common dependency on Preferences. Preferences delegates
// here for the UI-side getters/setters and SettingsDialog renders the
// spinbox; everyone uses QSettings against the same key.
inline constexpr const char* kMessagePageSizeKey     = "sync/messagePageSize";
inline constexpr int          kMessagePageSizeDefault = 50;
inline constexpr int          kMessagePageSizeMin     = 10;
inline constexpr int          kMessagePageSizeMax     = 500;

inline int messagePageSize() {
    QSettings s;
    const int v = s.value(QLatin1String(kMessagePageSizeKey),
                          kMessagePageSizeDefault).toInt();
    return std::clamp(v, kMessagePageSizeMin, kMessagePageSizeMax);
}

inline void setMessagePageSize(int n) {
    QSettings s;
    s.setValue(QLatin1String(kMessagePageSizeKey),
               std::clamp(n, kMessagePageSizeMin, kMessagePageSizeMax));
    s.sync();
}

}  // namespace fc::util
