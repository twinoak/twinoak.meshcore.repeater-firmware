#include "LinkMeshClient.h"

namespace twinoak {
namespace meshclient {

uint8_t LinkMeshClient::beginJob(const link::JobBegin& m, uint32_t now_ms) {
  // The codec has already refused a path 6.1 does not allow (decodeJobBegin), so the job's own
  // check of the same bounds cannot fail here; it is kept because this file is not the only
  // caller of Job::begin, and a refusal is "a field is not a legal value" either way.
  const bool ok = job_.begin(m.pubkey, m.path.mode, m.path.hash_size, m.path.hop_count,
                             m.path.hashes.ptr, m.path.hashes.len, now_ms);
  return ok ? link::kOk : link::kErrMalformed;
}

uint8_t LinkMeshClient::setPath(const link::Path& p) {
  // "ERR_STATE with no job" (6.1), judged before the path is: a Walter that forgot to begin a
  // job learns that and not that its path was wrong.
  if (!job_.begun()) return link::kErrState;
  const bool ok = job_.setPath(p.mode, p.hash_size, p.hop_count, p.hashes.ptr, p.hashes.len);
  return ok ? link::kOk : link::kErrMalformed;
}

uint8_t LinkMeshClient::getPath(link::Path& out) {
  // 6.1 says "the path the job holds now" and nothing about a node with no job; ERR_STATE is
  // the same answer PATH_SET gives (tasks/phase-2/reports/01.md, decisions).
  if (!job_.begun()) return link::kErrState;
  out.mode = job_.pathMode();
  out.hash_size = job_.pathHashSize();
  out.hop_count = job_.pathHopCount();
  // A view into the job's own storage, valid until the path changes - which is after this
  // response has been built.
  out.hashes = link::bytesOf(job_.pathBytes(), job_.pathByteCount());
  return link::kOk;
}

}  // namespace meshclient
}  // namespace twinoak
