// The RSSI of a packet, remembered from the moment it was received until the moment somebody
// wants it (docs/ARCHITECTURE.md section 7, "RSSI is noted in logRx").
//
// Why this exists at all. A packet's SNR is the packet's own - MeshCore stores it in the Packet
// when the frame arrives (Dispatcher::checkRecv, `pkt->_snr = _radio->getLastSNR() * 4.0f`,
// src/Dispatcher.cpp:206 at the pin 81dbae59) - but its **RSSI** is not: the radio keeps one
// "last RSSI" and it has moved on by the time the packet is handled. A flood packet is held back
// on purpose, for a delay the score decides and up to MAX_RX_DELAY_MILLIS (32 s) of it
// (src/Dispatcher.cpp:243-253: `_mgr->queueInbound(pkt, futureMillis(_delay))`), so between
// reception and onRecvPacket the radio may have heard several other frames.
//
// The one moment the number still belongs to the packet is `logRx`, which checkRecv calls
// immediately after parsing it (src/Dispatcher.cpp:238) and before the flood branch queues it.
// So the number is noted there, against the packet's address, and taken back when an advert or a
// reply is made of that packet - which is exactly what AgentMesh::noteRssi / takeRssi do in
// twinoak/meshmaster-agent-firmware at 51faafb427cba41b0de9983736b6621bc155f829, with the
// comment that says why.
//
// The table is as long as the packet pool, so that a note cannot be pushed out by frames that
// arrived after it while its own packet still waits: this build's pool is
// `new StaticPoolPacketManager(32)` (examples/simple_repeater/MyMesh.cpp:864 at the pin), and
// src/roles/repeater/app/TwinOakMesh.cpp static_asserts kRssiNotes against that number's name.
//
// A pooled packet is reused, so an address is not unique over time: note() overwrites the note of
// an address it already holds (that address is a new frame now) and take() removes the note it
// hands out. Both are a linear scan of 32 entries with no allocation.
//
// Host-portable: no Arduino, no MeshCore - the packet is a `const void*` here, which is all this
// file needs of it. test/test_meshclient drives it with addresses of its own.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace twinoak {
namespace meshclient {

// The packet pool of this build (src/roles/repeater/app/TwinOakMesh.cpp holds it against upstream's number).
constexpr uint8_t kRssiNotes = 32;

class RssiNotes {
 public:
  RssiNotes() {
    for (uint8_t i = 0; i < kRssiNotes; i++) {
      pkt_[i] = nullptr;
      rssi_[i] = 0;
    }
  }

  // The radio's last RSSI, while it is still this packet's. An address already noted is
  // overwritten: the pool handed that slot out again and this is the new frame's number.
  void note(const void* packet, int16_t rssi) {
    if (packet == nullptr) return;
    for (uint8_t i = 0; i < kRssiNotes; i++) {
      if (pkt_[i] == packet) {
        rssi_[i] = rssi;
        return;
      }
    }
    pkt_[next_] = packet;
    rssi_[next_] = rssi;
    next_ = uint8_t((next_ + 1) % kRssiNotes);
  }

  // The note for this packet, once. False when there is none - a packet that arrived before this
  // table had room, or one whose note has already been taken - and `rssi` is then untouched.
  bool take(const void* packet, int16_t& rssi) {
    if (packet == nullptr) return false;
    for (uint8_t i = 0; i < kRssiNotes; i++) {
      if (pkt_[i] != packet) continue;
      rssi = rssi_[i];
      pkt_[i] = nullptr;
      return true;
    }
    return false;
  }

  // How many notes are held. Nothing of the firmware reads it; a test does.
  uint8_t held() const {
    uint8_t n = 0;
    for (uint8_t i = 0; i < kRssiNotes; i++) {
      if (pkt_[i] != nullptr) n++;
    }
    return n;
  }

 private:
  const void* pkt_[kRssiNotes];
  int16_t rssi_[kRssiNotes];
  uint8_t next_ = 0;
};

}  // namespace meshclient
}  // namespace twinoak
