#include "bench_console.h"

#include <Arduino.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

namespace {

const char* kTestOperatorUid = "0A0B0C0D";
const char* kTestTechnicianUid = "0E0E0E0E";

const char* kHelp =
    "# commands:\n"
    "#   op                   tap the test operator badge (arm / disarm / reset)\n"
    "#   tech                 tap the test technician badge (lock-out / release)\n"
    "#   badge <UID> <role>   tap any badge; role: operator technician admin household unknown\n"
    "#   lock [reason]        station LOCK command\n"
    "#   prearm <eta_s>       station PREARM command\n"
    "#   state                show the state and what blocks arming";

bool roleFromName(const char* name, aeroion_role_t* role) {
  for (int i = 0; i < AEROION_ROLE__COUNT; ++i) {
    if (strcmp(name, AEROION_ROLE_NAMES[i]) == 0) {
      *role = static_cast<aeroion_role_t>(i);
      return true;
    }
  }
  return false;
}

}  // namespace

BenchConsole::BenchConsole(SafetyCore& safety) : safety_(safety), length_(0) { line_[0] = '\0'; }

void BenchConsole::poll(uint32_t nowMs) {
  while (Serial.available() > 0) {
    char c = static_cast<char>(Serial.read());
    if (c == '\n' || c == '\r') {
      if (length_ > 0) {
        line_[length_] = '\0';
        run(line_, nowMs);
        length_ = 0;
      }
    } else if (length_ < sizeof line_ - 1) {
      line_[length_++] = c;
    }
  }
}

void BenchConsole::run(char* line, uint32_t nowMs) {
  if (line[0] == '{') return;  // JSON lines from tools/serial_bridge.py (time, station commands): not typed commands
  char* command = strtok(line, " ");
  char* arg1 = strtok(nullptr, " ");
  char* rest = strtok(nullptr, "");  // everything after the first argument
  while (rest != nullptr && *rest == ' ') rest++;
  if (command == nullptr) return;

  if (strcmp(command, "op") == 0) {
    tap(kTestOperatorUid, AEROION_ROLE_OPERATOR);
  } else if (strcmp(command, "tech") == 0) {
    tap(kTestTechnicianUid, AEROION_ROLE_TECHNICIAN);
  } else if (strcmp(command, "badge") == 0) {
    aeroion_role_t role;
    if (arg1 == nullptr || rest == nullptr) {
      Serial.println("# usage: badge <UID> <role>");
      return;
    }
    for (char* p = arg1; *p; ++p) *p = static_cast<char>(toupper(*p));
    if (!isContractUid(arg1)) {
      Serial.println("# UID must be 8 to 20 hex characters, e.g. 04A1B2C3");
    } else if (!roleFromName(rest, &role)) {
      Serial.println("# role must be operator, technician, admin, household or unknown");
    } else {
      tap(arg1, role);
    }
  } else if (strcmp(command, "lock") == 0) {
    char reason[48] = "bench console";
    if (arg1 != nullptr) snprintf(reason, sizeof reason, "%s%s%s", arg1, rest ? " " : "", rest ? rest : "");
    SafetyReply r = safety_.lock(reason);
    Serial.printf("# LOCK %s%s\n", r.accepted ? "accepted" : "rejected: ", r.reason);
  } else if (strcmp(command, "prearm") == 0) {
    float etaS = arg1 ? static_cast<float>(atof(arg1)) : 0.0f;
    SafetyReply r = safety_.prearm(etaS, nowMs);
    Serial.printf("# PREARM %s%s\n", r.accepted ? "accepted" : "rejected: ", r.reason);
  } else if (strcmp(command, "state") == 0) {
    printState();
  } else {
    Serial.println(kHelp);
  }
}

void BenchConsole::tap(const char* uid, aeroion_role_t role) {
  aeroion_badge_result_t result = safety_.badge(uid, role);
  Serial.printf("# badge %s (%s): %s\n", uid, aeroion_role_name(role), aeroion_badge_result_name(result));
  printState();
}

void BenchConsole::printState() {
  const char* fault = safety_.blockingFault();
  Serial.printf("# state %s (%s)%s%s\n", aeroion_state_name(safety_.state()), safety_.lastReason(),
                fault ? ", cannot arm: " : "", fault ? fault : "");
}