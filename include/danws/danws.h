#pragma once

/// Main include for the danws C++ library.
/// DanProtocol v3.5 — Real-Time State Synchronization.

// Protocol layer
#include "protocol/types.h"
#include "protocol/error.h"
#include "protocol/codec.h"
#include "protocol/serializer.h"
#include "protocol/stream_parser.h"

// State layer
#include "state/key_registry.h"

// Connection layer
#include "connection/heartbeat_manager.h"
#include "connection/reconnect_engine.h"
#include "connection/bulk_queue.h"

// Client API
#include "api/topic_client_handle.h"
#include "api/client.h"

// Server API
#include "api/server_transport.h"
#include "api/flat_state.h"
#include "api/principal_tx.h"
#include "api/topic_handle.h"
#include "api/session.h"
#include "api/server.h"
