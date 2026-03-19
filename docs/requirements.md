# Requirements: Anti-Tailgating ACAP

## Purpose and Scope

This document defines the requirements for the Anti-Tailgating ACAP — a self-contained Axis Camera Application Platform application that runs directly on an Axis camera. It replaces a Node-RED flow that previously handled anti-tailgating logic on a separate server.

The application subscribes natively to AXIS Object Analytics (AOA) line crossing events, manages access tokens issued by badge readers or door controllers, detects tailgating (entry without a valid token), and publishes a native Axis event to the camera's event system to drive action rules.

---

## Functional Requirements

### FR-001: AOA Line Crossing Subscription
The application SHALL subscribe to AXIS Object Analytics line crossing events via the AXEvent API using a GLib GMainLoop. The AOA scenario ID SHALL be configurable (default: 1).

### FR-002: Badge Read HTTP Endpoint
The application SHALL expose an HTTP endpoint (`/badge-read`) accepting POST requests on port 8080. A successful request SHALL create one access token.

### FR-003: Access Token Creation with Configurable TTL
When a badge read is received, the application SHALL create an access token with a Time-To-Live (TTL) configurable via the `TokenExpirationSeconds` parameter (default: 7 seconds). Tokens are held in a FIFO queue.

### FR-004: Token Validation on Line Crossing
When an AOA line crossing event is received, the application SHALL consume the oldest valid access token from the queue (FIFO order). A consumed token prevents an alarm.

### FR-005: Tailgating Detection
When an AOA line crossing event is received and no valid token exists in the queue, the application SHALL record a tailgating alarm event.

### FR-006: Native Axis Event on Tailgating Alarm
When a tailgating alarm is detected, the application SHALL publish a stateful Axis event with topic `tnsaxis:CameraApplicationPlatform/antitailgate/TailgatingAlarm` and key `active` (bool). This event SHALL appear in the camera's System → Events → Rules trigger list as "Application → Anti-Tailgating → TailgatingAlarm" and can drive recordings, relay outputs, PTZ moves, or notifications without any external system.

### FR-007: Configuration Persistence
All configuration parameters SHALL persist across application restarts and device reboots using the AXParameter API.

### FR-008: Web UI via Embedded Server and Apache Reverse Proxy
The application SHALL serve a web interface. Static HTML/CSS/JS files SHALL be served by Apache at `/local/antitailgate/`. Dynamic API calls SHALL be handled by an embedded CivetWeb HTTP server on port 8080. The web UI SHALL use the direct port 8080 URL (`http://<camera-ip>:8080`) for API calls rather than relying on the Apache proxy path.

### FR-009: Dashboard
The web UI SHALL display a live dashboard showing:
- Current number of active (unexpired) access tokens (token gauge)
- Session statistics
- Simulate buttons: badge read, line crossing, clear history
- Event history table: Time, Type, Source, Badge ID, Outcome
- Alarm history table
The dashboard SHALL refresh automatically by polling `/status`.

### FR-010: Settings UI
The web UI SHALL provide a settings panel allowing configuration of:
- Token TTL (`TokenExpirationSeconds`)
- AOA Scenario ID (`AoaScenarioId`)
- Badge Read Endpoint card showing the direct URL with a copy button
- Door Controller Integration collapsible info card

### FR-011: HTTP Fallback Threshold Crossing Endpoint
The application SHALL expose `/threshold-crossing` (POST) as a manual trigger to simulate a line crossing event. This endpoint is used for testing and as a fallback if AOA is not available.

### FR-013: Clear History
The web UI SHALL include a button to clear the in-memory event and alarm history.

### FR-014: Reset to Defaults
The web UI SHALL include a button to reset all configuration parameters to their factory defaults.

### FR-015: Graceful Handling When AOA Is Not Installed
If AOA is not installed or the AXEvent subscription fails, the application SHALL log a warning, continue running, and the HTTP fallback endpoint (FR-011) SHALL remain available.

### FR-016: Door Controller Badge-Read Enrichment
The `/badge-read` endpoint SHALL accept an optional JSON request body of the form `{"door":"<name>","card":"<id>"}`. When present, `door` SHALL be stored as the `source` field and `card` as the `badge_id` field in the event history. If no body is provided, the endpoint SHALL behave as before (source="http", badge_id from query string or "unknown"), maintaining backward compatibility.

### FR-017: Native Axis Stateful Event Declaration
The application SHALL declare the `TailgatingAlarm` event on startup as a stateful (property) event using `ax_event_handler_declare(..., TRUE, ...)`. This ensures the event appears as a trigger in the camera's action rule system immediately upon application start.

---

## Non-Functional Requirements

### NFR-001: SDK Version
The application SHALL be built with ACAP Native SDK 12.8.

### NFR-002: Architecture Support
The application SHALL be built for both `armv7hf` (ARTPEC-7) and `aarch64` (ARTPEC-8) target architectures.

### NFR-003: Thread Safety
All access to the shared token store SHALL be protected by a `GMutex`. CivetWeb handler threads and the GLib main loop thread SHALL not race on shared state. AXEvent sends SHALL be dispatched on the GLib main loop thread using `g_idle_add()` when called from CivetWeb handler threads.

### NFR-005: Admin-Only Web UI Access
The Apache reverse proxy SHALL enforce `access: "admin"` for the API path, requiring valid Axis administrator credentials.

### NFR-006: Config Survives Restart and Firmware Upgrade
Configuration stored via AXParameter SHALL survive application restart, device restart, and firmware upgrade.

### NFR-007: No External Network Dependencies at Runtime
The application SHALL NOT require internet access or any external server to function. All logic runs on-device. Alarm notification is delivered via the native Axis event system.

### NFR-008: Web UI Works Without a Build Step
The web UI (HTML/CSS/JS) SHALL use only vanilla browser APIs. No bundler, framework, or compilation step is required.
