#pragma once
#include <stdint.h>

#include "command_handler.h"
#include "node_event.h"
#include "unix_clock.h"

// The safety controller's end of the link to the station, M7. Two ways in, with the same JSON lines:
//   - the node's ESP32-S3 on a UART (D0/D1), which carries them to and from the MQTT broker;
//   - a PC running tools/serial_bridge.py on the USB serial port, to test without an ESP32.
// Lines to the node (docs/link.md):
//   {"topic":"aeroion/n01/cmd","retain":false,"payload":{...}}   a command (aeroion/all/cmd too)
//   {"link":"time","ts":1791747040123}                           Unix time in ms, every 5 s
// The node sends each of its contract messages as one line, {"topic":...,"retain":...,"payload":...}.
// The link is "up" while time lines arrive. When it goes silent for 15 s, one FAULT COMM_LOST event goes
// out: the node keeps working on its own (badges, fog and arcs are decided here); only commands stop.
class NodeLink {
 public:
  enum LineKind {
    kTime,      // a time line: the clock is set
    kCommand,   // a command for this node (or for all): answered with CMD_ACK
    kNotForUs,  // a command for another node: ignored
    kUnknown,   // JSON, but not a line of the link: ignored
    kBadJson,   // not JSON, or too big for the memory: ignored
    kKinds
  };
  typedef void (*PrintFn)(const char* text);

  NodeLink(const char* nodeId, UnixClock& clock, CommandHandler& commands, EventSink& events,
           uint32_t silentMs = 15000);

  // One line received from the link or the USB serial port. nowMs: milliseconds since boot.
  LineKind handleLine(const char* line, uint32_t nowMs);
  void poll(uint32_t nowMs);  // call often: watches for silence

  bool up() const { return up_; }
  bool takeCameUp();  // true once each time the link comes (back) up: time to send info and status again
  uint32_t lines(LineKind kind) const { return kind < kKinds ? lines_[kind] : 0; }
  const char* lastError() const { return lastError_; }  // why the last bad line was not understood

  void printStatus(PrintFn print, uint32_t nowMs);  // for the console command "link"

 private:
  LineKind counted(LineKind kind);

  char cmdTopic_[48];
  UnixClock& clock_;
  CommandHandler& commands_;
  EventSink& events_;
  uint32_t silentMs_;
  bool up_;
  bool cameUp_;
  uint32_t lastTimeMs_;
  uint32_t lines_[kKinds];
  const char* lastError_;
};