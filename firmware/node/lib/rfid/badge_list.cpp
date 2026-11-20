#include "badge_list.h"

#include <string.h>

#include "node_event.h"  // isContractUid()

BadgeList::BadgeList() : count_(0), fromStation_(false), version_(0) {}

int BadgeList::find(const char* uid) const {
  if (uid == nullptr) return -1;
  for (int i = 0; i < count_; ++i) {
    if (strcmp(entries_[i].uid, uid) == 0) return i;
  }
  return -1;
}

bool BadgeList::set(const char* uid, aeroion_role_t role) {
  if (!isContractUid(uid)) return false;
  int index = find(uid);
  if (index < 0) {
    if (count_ >= kMaxBadges) return false;
    index = count_++;
    NodeEvent::copyText(entries_[index].uid, sizeof entries_[index].uid, uid);
  }
  entries_[index].role = role;
  return true;
}

bool BadgeList::remove(const char* uid) {
  int index = find(uid);
  if (index < 0) return false;
  entries_[index] = entries_[--count_];  // order does not matter
  return true;
}

void BadgeList::clear() { count_ = 0; }

aeroion_role_t BadgeList::roleOf(const char* uid) const {
  int index = find(uid);
  return index < 0 ? AEROION_ROLE_UNKNOWN : entries_[index].role;
}

const char* BadgeList::uidAt(int index) const { return (index >= 0 && index < count_) ? entries_[index].uid : ""; }

aeroion_role_t BadgeList::roleAt(int index) const {
  return (index >= 0 && index < count_) ? entries_[index].role : AEROION_ROLE_UNKNOWN;
}

void BadgeList::setVersion(uint32_t version) {
  version_ = version;
  fromStation_ = true;
}

bool badgeRoleFromName(const char* name, aeroion_role_t* role) {
  if (name == nullptr) return false;
  for (int i = 0; i < AEROION_ROLE__COUNT; ++i) {
    if (strcmp(name, AEROION_ROLE_NAMES[i]) == 0) {
      *role = static_cast<aeroion_role_t>(i);
      return true;
    }
  }
  return false;
}