#pragma once
#include <stddef.h>
#include <stdint.h>

#include "aeroion_contract.h"

// The badges this node accepts, and the role of each. The station owns the list and sends all of it
// (SYNC_ALLOWLIST); the node keeps its own copy, so badges keep working while the Pi or the radio is down.
// A UID that is not on the list has the role "unknown", which the safety core always denies.
// Fixed size, no dynamic memory.
class BadgeList {
 public:
  static const int kMaxBadges = 64;  // the contract's limit for one SYNC_ALLOWLIST

  BadgeList();

  bool set(const char* uid, aeroion_role_t role);  // add or change one badge; false if the UID is invalid
                                                   // (see isContractUid) or the list is full
  bool remove(const char* uid);
  void clear();

  aeroion_role_t roleOf(const char* uid) const;  // AEROION_ROLE_UNKNOWN if not on the list
  int count() const { return count_; }
  const char* uidAt(int index) const;
  aeroion_role_t roleAt(int index) const;

  // Version of the last list received from the station. Before the first one, the list comes from the
  // node's settings and fromStation() is false.
  bool fromStation() const { return fromStation_; }
  uint32_t version() const { return version_; }
  void setVersion(uint32_t version);

 private:
  struct Entry {
    char uid[21];
    aeroion_role_t role;
  };
  int find(const char* uid) const;

  Entry entries_[kMaxBadges];
  int count_;
  bool fromStation_;
  uint32_t version_;
};

// Role from its contract name ("operator", "technician", ...). False if there is no such role.
bool badgeRoleFromName(const char* name, aeroion_role_t* role);