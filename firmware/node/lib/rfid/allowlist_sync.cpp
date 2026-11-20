#include "allowlist_sync.h"

#include <stdio.h>

#include "node_event.h"  // isContractUid()

namespace {
bool refuse(char* reason, size_t cap, const char* text) {
  NodeEvent::copyText(reason, cap, text);
  return false;
}
}  // namespace

bool applySyncAllowlist(JsonObjectConst params, BadgeList& list, char* reason, size_t cap) {
  char text[65];
  JsonVariantConst version = params["version"];
  JsonArrayConst entries = params["entries"];
  if (!version.is<uint32_t>()) return refuse(reason, cap, "version must be a whole number from 0");
  if (entries.isNull()) return refuse(reason, cap, "entries missing");
  if (entries.size() > static_cast<size_t>(BadgeList::kMaxBadges)) return refuse(reason, cap, "more than 64 entries");
  uint32_t newVersion = version.as<uint32_t>();
  if (list.fromStation() && newVersion < list.version()) {
    snprintf(text, sizeof text, "older list: version %lu, node has %lu", static_cast<unsigned long>(newVersion),
             static_cast<unsigned long>(list.version()));
    return refuse(reason, cap, text);
  }

  // Check every entry before changing anything.
  int index = 0;
  for (JsonObjectConst entry : entries) {
    const char* uid = entry["uid"];
    const char* roleName = entry["role"];
    aeroion_role_t role;
    if (!isContractUid(uid)) {
      snprintf(text, sizeof text, "entry %d: uid must be 8 to 20 upper-case hex characters", index);
      return refuse(reason, cap, text);
    }
    if (!badgeRoleFromName(roleName, &role)) {
      snprintf(text, sizeof text, "entry %d: unknown role", index);
      return refuse(reason, cap, text);
    }
    index++;
  }

  list.clear();
  for (JsonObjectConst entry : entries) {
    const char* uid = entry["uid"];
    const char* roleName = entry["role"];
    aeroion_role_t role = AEROION_ROLE_UNKNOWN;
    badgeRoleFromName(roleName, &role);
    list.set(uid, role);
  }
  list.setVersion(newVersion);
  if (cap > 0) reason[0] = '\0';
  return true;
}