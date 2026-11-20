#include "bench_console.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

const char* kTestOperatorUid = "0A0B0C0D";
const char* kTestTechnicianUid = "0E0E0E0E";

const char* kHelp =
    "# badges:   op | tech | badge <UID> <role>   (or press SW2 = operator, SW3 = technician)\r\n"
    "# station:  lock [reason] | prearm <eta_s>\r\n"
    "# sensors:  fog <0..1> | person on|off | lid closed|open | current <uA> | risk <0..1>|none\r\n"
    "#           add 'lost' instead of a value to simulate a dead sensor, e.g. person lost\r\n"
    "# show:     state | inputs\r\n";

bool roleFromName(const char* name, aeroion_role_t* role) {
  for (int i = 0; i < AEROION_ROLE__COUNT; ++i) {
    if (strcmp(name, AEROION_ROLE_NAMES[i]) == 0) {
      *role = static_cast<aeroion_role_t>(i);
      return true;
    }
  }
  return false;
}

// Integers only: the C library of small chips often cannot print floats.
int hundredths(float value) { return static_cast<int>(value * 100.0f + (value >= 0.0f ? 0.5f : -0.5f)); }

}  // namespace

BenchConsole::BenchConsole(SafetyCore& safety, BenchInputs& inputs, PrintFn print)
    : safety_(safety), inputs_(inputs), print_(print), length_(0) {
  line_[0] = '\0';
}

void BenchConsole::feed(int c, uint32_t nowMs) {
  if (c == '\n' || c == '\r') {
    if (length_ > 0) {
      line_[length_] = '\0';
      run(line_, nowMs);
      length_ = 0;
    }
  } else if (c >= ' ' && length_ < sizeof line_ - 1) {
    line_[length_++] = static_cast<char>(c);
  }
}

void BenchConsole::tapOperator() { tap(kTestOperatorUid, AEROION_ROLE_OPERATOR); }

void BenchConsole::tapTechnician() { tap(kTestTechnicianUid, AEROION_ROLE_TECHNICIAN); }

void BenchConsole::run(char* line, uint32_t nowMs) {
  char* command = strtok(line, " ");
  char* arg1 = strtok(nullptr, " ");
  char* rest = strtok(nullptr, "");  // everything after the first argument
  while (rest != nullptr && *rest == ' ') rest++;
  if (command == nullptr) return;
  char text[160];

  if (strcmp(command, "op") == 0) {
    tapOperator();
  } else if (strcmp(command, "tech") == 0) {
    tapTechnician();
  } else if (strcmp(command, "badge") == 0) {
    aeroion_role_t role;
    if (arg1 == nullptr || rest == nullptr) {
      print_("# usage: badge <UID> <role>\r\n");
      return;
    }
    for (char* p = arg1; *p; ++p) *p = static_cast<char>(toupper(*p));
    if (!isContractUid(arg1)) {
      print_("# UID must be 8 to 20 hex characters, e.g. 04A1B2C3\r\n");
    } else if (!roleFromName(rest, &role)) {
      print_("# role must be operator, technician, admin, household or unknown\r\n");
    } else {
      tap(arg1, role);
    }
  } else if (strcmp(command, "lock") == 0) {
    char reason[48] = "bench console";
    if (arg1 != nullptr) snprintf(reason, sizeof reason, "%s%s%s", arg1, rest ? " " : "", rest ? rest : "");
    SafetyReply r = safety_.lock(reason);
    snprintf(text, sizeof text, "# LOCK %s%s\r\n", r.accepted ? "accepted" : "rejected: ", r.reason);
    print_(text);
  } else if (strcmp(command, "prearm") == 0) {
    float etaS = arg1 ? static_cast<float>(atof(arg1)) : 0.0f;
    SafetyReply r = safety_.prearm(etaS, nowMs);
    snprintf(text, sizeof text, "# PREARM %s%s\r\n", r.accepted ? "accepted" : "rejected: ", r.reason);
    print_(text);
  } else if (strcmp(command, "state") == 0) {
    printState();
  } else if (strcmp(command, "inputs") == 0) {
    printInputs();
  } else if (arg1 != nullptr && (strcmp(command, "fog") == 0 || strcmp(command, "person") == 0 ||
                                 strcmp(command, "lid") == 0 || strcmp(command, "current") == 0 ||
                                 strcmp(command, "risk") == 0)) {
    setSensor(command, arg1);
  } else {
    print_(kHelp);
  }
}

void BenchConsole::setSensor(const char* name, const char* value) {
  bool lost = strcmp(value, "lost") == 0;
  float number = static_cast<float>(atof(value));
  if (strcmp(name, "fog") == 0) {
    if (lost) inputs_.fogIdx.clear(); else inputs_.fogIdx = number < 0.0f ? 0.0f : (number > 1.0f ? 1.0f : number);
  } else if (strcmp(name, "person") == 0) {
    if (lost) inputs_.personNear.clear(); else inputs_.personNear = strcmp(value, "on") == 0;
  } else if (strcmp(name, "lid") == 0) {
    if (lost) inputs_.lidClosed.clear(); else inputs_.lidClosed = strcmp(value, "open") != 0;
  } else if (strcmp(name, "current") == 0) {
    if (lost) inputs_.currentUa.clear(); else inputs_.currentUa = number < 0.0f ? 0.0f : number;
  } else if (strcmp(name, "risk") == 0) {
    if (lost || strcmp(value, "none") == 0) inputs_.arcRisk.clear(); else inputs_.arcRisk = number;
  }
  printInputs();
}

void BenchConsole::tap(const char* uid, aeroion_role_t role) {
  char text[96];
  aeroion_badge_result_t result = safety_.badge(uid, role);
  snprintf(text, sizeof text, "# badge %s (%s): %s\r\n", uid, aeroion_role_name(role),
           aeroion_badge_result_name(result));
  print_(text);
  printState();
}

void BenchConsole::printState() {
  char text[192];
  const char* fault = safety_.blockingFault();
  snprintf(text, sizeof text, "# state %s (%s)%s%s\r\n", aeroion_state_name(safety_.state()), safety_.lastReason(),
           fault ? ", cannot arm: " : "", fault ? fault : "");
  print_(text);
}

void BenchConsole::printInputs() {
  char fog[16] = "lost", current[16] = "lost", risk[16] = "none";
  if (inputs_.fogIdx.has()) snprintf(fog, sizeof fog, "%d%%", hundredths(inputs_.fogIdx.get()));
  if (inputs_.currentUa.has()) snprintf(current, sizeof current, "%d uA", hundredths(inputs_.currentUa.get()) / 100);
  if (inputs_.arcRisk.has()) snprintf(risk, sizeof risk, "%d%%", hundredths(inputs_.arcRisk.get()));
  const char* person = !inputs_.personNear.has() ? "lost" : (inputs_.personNear.get() ? "near" : "none");
  const char* lid = !inputs_.lidClosed.has() ? "lost" : (inputs_.lidClosed.get() ? "closed" : "open");
  char text[160];
  snprintf(text, sizeof text, "# inputs: fog %s, person %s, lid %s, current %s, arc risk %s\r\n", fog, person, lid,
           current, risk);
  print_(text);
}