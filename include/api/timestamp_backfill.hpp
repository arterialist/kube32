#pragma once

// Objects created before the wall clock is set (SNTP has not answered yet)
// carry no metadata.creationTimestamp, and Events no firstTimestamp or
// lastTimestamp: the API server omits times it does not know rather than
// writing 1970. Once the clock is valid this stamps them with the boot time,
// the earliest moment they can have been created: they all came into being
// between boot and the first clock sync, a few seconds at most.

#include "storage/store.hpp"

#include <cstddef>

namespace kube32::api {

// Returns the number of objects updated, 0 while the clock is not set.
size_t backfill_timestamps(storage::Store& store);

} // namespace kube32::api
