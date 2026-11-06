# Phase 3.8 — follow-ups

Items for a later task of this phase are tagged with it. The review of 01–02 goes below them when it
is written.

- [ ] **[Thomas] Status, neighbours and owner-info of the unit's own repeater, through its Walter.**
  6 November: the Walter's *local door* (its `components/agent/include/walter_agent/link_mesh.h`)
  serves a job for the unit's own repeater without the air. The login is answered locally, a CLI line
  is the link's `CLI`, telemetry is `LPP_GET`. Status, neighbours, owner-info, discovery and trace have
  no request on the link, so they are *Failed*: the unit cannot read its own repeater's status at all,
  and meshmaster.dk's scheduled `status` and `neighbours` of that repeater fail through its uplink.
  Two ways, one decision:
  - a new link request (1.2) that answers what MeshCore's `REQ_TYPE_GET_STATUS` and the neighbours
    request would, as bytes;
  - the CLI's own stats lines (`stats-core`, `stats-radio`, `stats-packets` at the pin) through `CLI`,
    with the Walter shaping them into what agent-core expects.

  The first is exact and is a spec change. The second needs none, but it parses text. meshmaster.dk's
  pile 4.25 task 169 routes around it meanwhile.
