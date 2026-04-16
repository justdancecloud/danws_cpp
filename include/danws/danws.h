#pragma once

/// Main include for the danws C++ client library.
/// DanProtocol v3.5 — Real-Time State Synchronization.

#include "protocol/types.h"
#include "protocol/error.h"
#include "protocol/codec.h"
#include "protocol/serializer.h"
#include "protocol/stream_parser.h"
#include "state/key_registry.h"
#include "connection/heartbeat_manager.h"
#include "connection/reconnect_engine.h"
#include "connection/bulk_queue.h"
#include "api/topic_client_handle.h"
#include "api/client.h"
