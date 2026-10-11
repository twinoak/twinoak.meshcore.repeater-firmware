#include "TwinOakMesh.h"

#include "../clock/BoardClock.h"

// Every body calls its base and, in the four that can put this node's clock on the air, tells the
// clock keeper so first - before the base call, so that there is no window in which upstream has
// replied and the keeper does not know (TwinOakMesh.h says which four and why). What the
// overrides will do beyond that is phase 2's.

int TwinOakMesh::searchPeersByHash(const uint8_t* hash) {
  return MyMesh::searchPeersByHash(hash);
}

void TwinOakMesh::getPeerSharedSecret(uint8_t* dest_secret, int peer_idx) {
  MyMesh::getPeerSharedSecret(dest_secret, peer_idx);
}

void TwinOakMesh::onPeerDataRecv(mesh::Packet* packet, uint8_t type, int sender_idx,
                                 const uint8_t* secret, uint8_t* data, size_t len) {
  twinoak::clock::noteTimestampEmitted();
  MyMesh::onPeerDataRecv(packet, type, sender_idx, secret, data, len);
}

bool TwinOakMesh::onPeerPathRecv(mesh::Packet* packet, int sender_idx, const uint8_t* secret,
                                 uint8_t* path, uint8_t path_len, uint8_t extra_type,
                                 uint8_t* extra, uint8_t extra_len) {
  twinoak::clock::noteTimestampEmitted();
  return MyMesh::onPeerPathRecv(packet, sender_idx, secret, path, path_len, extra_type, extra,
                                extra_len);
}

void TwinOakMesh::onAnonDataRecv(mesh::Packet* packet, const uint8_t* secret,
                                 const mesh::Identity& sender, uint8_t* data, size_t len) {
  twinoak::clock::noteTimestampEmitted();
  MyMesh::onAnonDataRecv(packet, secret, sender, data, len);
}

void TwinOakMesh::onTraceRecv(mesh::Packet* packet, uint32_t tag, uint32_t auth_code,
                              uint8_t flags, const uint8_t* path_snrs,
                              const uint8_t* path_hashes, uint8_t path_len) {
  MyMesh::onTraceRecv(packet, tag, auth_code, flags, path_snrs, path_hashes, path_len);
}

void TwinOakMesh::onAdvertRecv(mesh::Packet* packet, const mesh::Identity& id,
                               uint32_t timestamp, const uint8_t* app_data,
                               size_t app_data_len) {
  MyMesh::onAdvertRecv(packet, id, timestamp, app_data, app_data_len);
}

void TwinOakMesh::logRx(mesh::Packet* pkt, int len, float score) {
  MyMesh::logRx(pkt, len, score);
}

void TwinOakMesh::logTx(mesh::Packet* pkt, int len) {
  MyMesh::logTx(pkt, len);
}

void TwinOakMesh::sendSelfAdvertisement(int delay_millis, bool flood) {
  twinoak::clock::noteTimestampEmitted();
  MyMesh::sendSelfAdvertisement(delay_millis, flood);
}
