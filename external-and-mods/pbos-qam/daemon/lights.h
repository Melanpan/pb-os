// The stick light engine: pbos-qamd --lights, its own unit
// (pbos-utils-lights), started by the tools section.
#pragma once

namespace pq {

// Runs until SIGTERM; 0 also when this device has no stick lights to drive.
int run_lights();

}  // namespace pq
