#pragma once
#include <ArduinoJson.h>
#include <stddef.h>

#include "badge_list.h"

// Applies the params of a SYNC_ALLOWLIST command (contract: cmd.schema.json) to the node's badge list:
//   {"version": 7, "entries": [{"uid": "04A1B2C3", "role": "operator"}, ...]}
// All or nothing: if any part is wrong, the list stays as it was. A list older than the one the node already
// has from the station is refused; the same version again is accepted (a resend).
// On refusal, returns false and writes a short reason (at most 64 characters, the CMD_ACK limit).
bool applySyncAllowlist(JsonObjectConst params, BadgeList& list, char* reason, size_t cap);