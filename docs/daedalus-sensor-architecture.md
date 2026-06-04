# Daedalus — Sensor Architecture

## The Design Brief

Prevent the deer from ending up on the windshield.

The deer does not decide to jump. The jump is already executing before cognition catches up. The target is reflexive response latency — not deliberate response latency. The system must survive contact with an unpredictable world before it knows the world was unpredictable.

## Reflex Arc Model

The sensor layer does not feed a decision loop. It feeds a reflex arc.

```
sensor event
    │
    ├─► joint node actuator   (reflex arc — <10ms, no supervisor involvement)
    │
    └─► supervisor node       (telemetry — >10ms, terrain mapping, learning)
```

Speed-critical responses (slip detection, impact, sudden load shift) route directly to the joint actuator on the same node. The supervisor receives the data after the fact for pattern analysis and adaptation. This mirrors the spinal cord model — the knee-jerk reflex closes at the spine, not the brain.

This architecture distinction is the core IP: not just that the leg has sensors, but that the sensing layer closes the reflex loop at the joint before any deliberate processing occurs.

## Bio-Inspiration

**Bee filiform hairs** — thousands of micro-hairs on the bee's eyes detect wind speed and direction via pressure differential. No single sensor reads "the wind." The field is resolved from the aggregate of many small, spatially distributed readings firing in parallel. Fast, distributed, directional.

**Deer proprioception** — the deer's escape response is not a decision. Micro-load signals in the hoof and leg trigger a pre-programmed motor pattern. Cognition arrives after the body is already moving.

The Ungulate Leg adopts both principles:
- Many small sensors, not one large sensor
- Each sensor has spatial specificity (location on the structure matters)
- Sensors fire in parallel, not sequential poll
- Reflex patterns pre-loaded at the joint node level

## Sensor Types

### Joint nodes (slots 0–47)

Each joint node carries sensors appropriate to its position in the kinematic chain.

| Sensor | Location | Signal | Purpose |
|--------|----------|--------|---------|
| IMU (accel + gyro) | Joint housing | Angular velocity, orientation | Joint angle, impact detection |
| Strain gauge | Structural member | Micro-deformation | Load distribution, fatigue |
| Current sense | Actuator line | Motor current draw | Effort monitoring, slip detection |
| Force-sensitive resistor | Contact surfaces | Ground pressure | Terrain compliance, weight distribution |
| Piezo film | Contact surface underlay | Vibration signature | Surface texture, terrain type |

### Supervisor node (slot 48)

The Supervisor aggregates telemetry from all 48 joint nodes and builds the global picture:

- Terrain map from distributed FSR readings
- Gait signature over time (load patterns, timing)
- Fatigue model from strain gauge + current sense trends
- Anomaly detection (asymmetric loading, joint limit approach)

## Data Flow

```
                    ┌─────────────────────────────┐
                    │  Joint Node (e.g. slot 16)  │
                    │                             │
  IMU ─────────────►│                             │──► actuator (reflex, <10ms)
  strain gauge ────►│  schema-init PID 1          │
  current sense ───►│  sensor-reflex daemon       │──► supervisor telemetry (>10ms)
  FSR ─────────────►│                             │
  piezo ───────────►│                             │
                    └─────────────────────────────┘
```

The `sensor-reflex daemon` on each joint node is a tight loop — read sensors, evaluate against reflex thresholds, command actuator if threshold crossed, forward telemetry to supervisor. No blocking, no waiting for supervisor acknowledgment.

## Gel Neural Interface

The gel interface is the candidate for closing the reflex arc back to the wearer — translating mechanical reflex events into haptic or neural feedback so the user feels the terrain the leg is walking on without consciously processing it. Implementation TBD. The concept: the sensor layer and the gel interface together make the leg an extension of the nervous system, not a machine the wearer operates.

## Future Work

- Define reflex threshold values per joint and terrain class
- Sensor fusion algorithm for terrain classification from FSR + piezo aggregate
- Gait signature baseline + anomaly detection
- Fatigue model from cumulative strain gauge data
- Gel interface signal encoding (frequency, intensity mapping to terrain events)
