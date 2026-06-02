# Daikin Madoka — Whole-Project Audit (2026-06-02)

Read-only hidden-bug audit of the ESPHome C++ component (`esphome_components/madoka`, mirrored at `esphome/components/madoka`) and the Home Assistant Python integration (`__init__.py`, `climate.py`, `config_flow.py`, `const.py`, `sensor.py`). No BLE hardware, `esphome compile`, or HA runtime was invoked.

**Method:** lens-based read of every entrypoint → candidate findings → adversarial per-finding verification (8 parallel independent reviewers, read-only, opus). Each issue's **Confidence** and **Severity** reflect the post-verification verdict. One candidate (sensor-platform `KeyError`) was **refuted** by verification and is documented under *Refuted* rather than dropped.

**Severity scale:** CRITICAL (breaks a shipped feature / crash on common path) · HIGH (crash or wrong behavior on a common path) · MEDIUM (robustness / efficiency / non-idiomatic) · LOW (cosmetic / latent / incremental).

**Threat-model note (C++ parsing issues):** the malformed-frame findings require BLE proximity to a malfunctioning, spoofed, or MITM'd controller — not internet-remote. Impact is bounded (out-of-bounds *read* / device reboot / garbage state); there is no write primitive or RCE.

---

## ISSUE-1 · `device_info` raises `TypeError` when the device is offline at startup
- **Severity:** HIGH · **Confidence:** 90 · **Status:** Confirmed (verified)
- **Area:** Home Assistant integration
- **File:line:** `climate.py:368-377` (`self.dev_info` initialized `None` at `climate.py:94`)
- **Problem:** `device_info` evaluates `"Model Number String" in self.dev_info` (`:370`) and `"Software Revision String" in self.dev_info` (`:375`). `self.dev_info` starts as `None` and is only assigned in `async_update` via `await self.controller.read_info()`, which sits inside a `try/except` that swallows `ConnectionAbortedError`/`ConnectionException`. If the device is offline/unreachable when the entity is added (a common condition for a BLE thermostat), `dev_info` stays `None`. Entities are added with `update_before_add=True` (`:85`), and HA reads `device_info` during entity registration even after the swallowed exception → `TypeError: argument of type 'NoneType' is not iterable`. The entity fails to register / gets no device-registry entry (isolated to this entity by HA's platform wrapper — not a whole-integration crash).
- **Suggested fix:** Guard at the top of `device_info`: `info = self.dev_info or {}` and read from `info` (or `if self.dev_info is None: return None`).

## ISSUE-2 · `parse_cb_` does not validate BLE frame lengths → out-of-bounds reads & possible non-termination
- **Severity:** MEDIUM · **Confidence:** 90 · **Status:** Confirmed (verified) — *root-cause cluster of 3 defects*
- **Area:** ESPHome C++ component
- **File:line:** `esphome_components/madoka/madoka.cpp:354-531` (and the mirror `esphome/components/madoka/madoka.cpp`)
- **Problem:** Device-controlled BLE notification bytes flow `gattc_event_handler` (`:204-214`) → `process_incoming_chunk_` → `parse_cb_`. The only validation is `validate_buffer` (`:260`): `buffer[0] == buffer.size()` — it checks the **outer** total length only, never the inner TLV `len` fields or the header. Three concrete defects:
  1. **Pre-loop header read, no minimum-size guard** (`:355`): `function_id = msg[2] << 8 | msg[3]`. A 2-byte chunk `{0x00,0x01}` → stripped to size-1 `{0x01}` passes `validate_buffer` (`buffer[0]==1==size`), then `msg[2]`/`msg[3]` read past the vector. *Most reliably triggerable OOB.*
  2. **TLV payload over-read, no `i+len<=message_size` guard** (`:365`, `:376`, `:421-427`, `:474`, etc.): each case does `len = msg[i++]; std::vector val(msg.begin()+i, msg.begin()+i+len)`. A `len` exceeding the remaining bytes makes `msg.begin()+i+len` an iterator past `end()` → UB. `CMD_GET_SETPOINT` reads `val[0]<<8|val[1]` with no `len>=2` check (`:422`,`:427`). The `len >= N` guards elsewhere compare to a constant, not to the remaining buffer.
  3. **`uint8_t` index wrap → potential non-termination** (`:356-357`, every `i += len`): `i` and `message_size` are `uint8_t`; `i += len` with a large `len` wraps mod-256 to a value `< message_size`, re-parsing earlier bytes — a crafted frame can keep the `while (i < message_size)` loop from terminating (task-watchdog reboot / DoS).
- **Suggested fix:** Add a single bounds-checked TLV reader used by all cases: bail the frame if `message_size < 4` before reading the header, and at each iteration require `i + 2 <= message_size` (arg+len bytes) and `i + len <= message_size` before consuming the payload; check `len >= N` before any `val[N-1]`. Use `size_t i` to remove the wrap. Apply to both trees (see ISSUE-6).

## ISSUE-3 · `async_setup_entry` forwards platform setups as fire-and-forget tasks
- **Severity:** MEDIUM · **Confidence:** 90 · **Status:** Confirmed (verified)
- **Area:** Home Assistant integration
- **File:line:** `__init__.py:82-86`
- **Problem:** The loop wraps `hass.config_entries.async_forward_entry_setups(entry, [component])` in `hass.async_create_task` per component and returns `True` immediately without awaiting. The idiomatic form is a single awaited call with the full list: `await hass.config_entries.async_forward_entry_setups(entry, COMPONENT_TYPES)`. Consequences: (1) the entry is reported set-up before `climate`/`sensor` platforms actually exist, racing against `async_unload_entry` (`:89-93`) on a reload; (2) platform-setup failures no longer fail/retry the config entry (they're logged by HA's task machinery but don't propagate). *Nuance from verification:* the errors are **logged** by HA (not fully silent), and in the common fast path startup works — the defect bites on reload races and masked platform failures.
- **Nearby (same function):** `:71-78` catches only `ConnectionAbortedError` from `controller.start()`; a controller that fails with any other exception (or none) is still stored (`:81`) and forwarded, handing a half-initialized controller to the platforms.
- **Suggested fix:** Replace the loop with one awaited `async_forward_entry_setups(entry, COMPONENT_TYPES)`; broaden the `start()` guard and skip storing controllers that failed to start.

## ISSUE-4 · Blocking `esphome::delay()` inside `query_` stalls the ESPHome main loop
- **Severity:** MEDIUM · **Confidence:** 87 · **Status:** Confirmed-with-correction (verified)
- **Area:** ESPHome C++ component
- **File:line:** `esphome_components/madoka/madoka.cpp:351` (`delay`), `:221-236` (`update`), `:65-141` (`control`)
- **Problem:** `query_` ends with a blocking `esphome::delay(t_d)`. `update()` issues 8 queries × 50 ms ≈ 400 ms; `control()` worst case ≈ 1.4 s (mode 600 + status 200 + setpoint 400 + fan 200), and `control()` sets `should_update_ = true` so a user action chains ~1.4 s then ~400 ms on the next loop — all synchronous on the ESPHome main loop.
- **Correction from verification:** the original draft overstated impact. On ESP32, `esphome::delay` uses `vTaskDelay` for ms-scale waits, which **yields** to FreeRTOS — the BLE host task (separate task/core) is **not** starved and the idle task still feeds the watchdog, so **no WDT reset**. Real impact: the main-loop task is blocked up to ~1 s → "Component took a long time" warnings and latency/stutter for other main-loop work (sensors, native API socket servicing). Classic MEDIUM blocking-loop profile, not a crash.
- **Suggested fix:** Drive per-command pacing from the cooperative `loop()`/a small state machine (queue commands, advance on a timer) instead of blocking; or reduce the cumulative per-poll delay.

## ISSUE-5 · `update()` polls clean-filter / version / eye-brightness even when those entities are unconfigured
- **Severity:** LOW · **Confidence:** 93 · **Status:** Confirmed (verified) · *related to ISSUE-4*
- **Area:** ESPHome C++ component
- **File:line:** `esphome_components/madoka/madoka.cpp:233-235`
- **Problem:** `update()` unconditionally queries `CMD_GET_CLEAN_FILTER`, `CMD_GET_VERSION`, `CMD_GET_EYE_BRIGHTNESS`. The pointers `clean_filter_binary_sensor_` / `firmware_version_text_sensor_` / `eye_brightness_number_` default to `nullptr` (`madoka.h:78-80`) and are only set by optional config. The parse side null-guards (data discarded), but each unused query is a real BLE write + ~50 ms blocking delay (≈150 ms/poll wasted) when those entities aren't configured. Outdoor temperature is *not* affected (it rides the existing `CMD_GET_SENSOR_INFORMATION`).
- **Suggested fix:** Guard each optional query on its pointer, matching the codebase's own pattern (`set_eye_brightness`/`reset_filter` already null-check these): `if (this->clean_filter_binary_sensor_) this->query_(CMD_GET_CLEAN_FILTER, …);`.

## ISSUE-6 · Two hand-synced component trees — drift has already caused a real bug
- **Severity:** MEDIUM · **Confidence:** 95 · **Status:** Confirmed (verified, primary evidence)
- **Area:** Repo structure
- **File:line:** `esphome_components/madoka/madoka.cpp:275-276` vs `esphome/components/madoka/madoka.cpp:275`
- **Problem:** Both trees are independently installable: `esphome_components/` is the `type: local` path (per `example-config.yaml:35-39`), `esphome/components/madoka/` is the `github://` external-components layout. Which path a user installs from decides which code runs. They are maintained byte-identical **by hand**, and the hazard is **not** hypothetical:
  - Commit `023ae87` ("propagate chunk_id=0 buffer reset to esphome/components") exists *solely* because a real functional fix landed in `esphome_components/` but was missed in `esphome/components/` — exactly this drift, on chunk-buffer-reset logic.
  - The trees are **still divergent right now**: `madoka.cpp:275-276` differs (French comment + `ESP_LOGW` string in `esphome_components/`, English in `esphome/components/`). Committed drift (`git status` clean). Currently cosmetic (log language), but unguarded.
  - CI (`.github/workflows/ci.yml`) runs only `ruff` + `compileall` — neither detects cross-tree divergence.
- **Suggested fix:** Make one tree the source of truth and symlink/generate the other, **or** add a CI step asserting `diff -rq` equality of the two `madoka.cpp`/`.h`/`climate.py` pairs. Reconcile the current `:275-276` drift now.

---

## Refuted (caught by verification — recorded, not dropped)

### ~~RF1 · sensor.py `KeyError` on `hass.data[DOMAIN][CONTROLLERS]`~~ — REFUTED (confidence 98)
A candidate finding claimed `sensor.py` read controllers via `hass.data[DOMAIN][CONTROLLERS]` (a `KeyError`, since `__init__.py:81` keys by `entry_id`). **False.** The current `sensor.py:24` reads `hass.data[DOMAIN][entry.entry_id][CONTROLLERS].values()` — the correct, `entry_id`-keyed path, matching `climate.py`. The candidate was generated against a **stale pre-merge snapshot** of `sensor.py` (read at session start, before the `36f7c3e` rewrite that also fixed this path and added the indoor/outdoor sensor classes). No bug. *Lesson: re-read files against current `HEAD` before filing — the adversarial-verify pass caught this.*

## Hypotheses (lower-confidence / design-intent — not confirmed bugs)

- **H1 · Single-setpoint collapse shows only the cooling setpoint outside HEAT** — `madoka.cpp:433-434`: `target_temperature = (mode == HEAT) ? heating : cooling`. For HEAT_COOL/AUTO/DRY/FAN this always surfaces the cooling setpoint. Appears to be a deliberate single-setpoint UI tradeoff; in a true heat/cool range it may misrepresent the active target. *Confirm design intent.*
- **H2 · `reset_filter` sends an extra `0x51` TLV not in `pymadoka`** — `madoka.cpp:253` writes `{0x51,0x01,0x01, 0xFE,0x01,0x01}`; `pymadoka`'s reset uses only `{0xFE:0x01}`. Unverified vs hardware; may be intentional or a harmless no-op. *Hardware-confirm.*
- **H3 · `config_flow` `CONF_DEVICES` default `[]` under a `cv.string` validator** — `config_flow.py:37` default-type mismatch; only bites if the field is omitted. LOW.

## Coverage map

| Lens | Subsystem | Status | Notes |
|---|---|---|---|
| BLE frame parsing / bounds | `madoka.cpp parse_cb_`, `process_incoming_chunk_` | REACHED | ISSUE-2 (3 defects) |
| ESPHome loop / blocking | `madoka.cpp query_/update/control` | REACHED | ISSUE-4, ISSUE-5 |
| Codegen / config schema | `madoka/climate.py`, esphome `__init__.py` | REACHED | no findings beyond mirror drift |
| HA data-flow / lifecycle | `__init__.py`, `climate.py`, `sensor.py` | REACHED | ISSUE-1, ISSUE-3; RF1 refuted |
| HA config flow | `config_flow.py`, `const.py` | REACHED | H3 only |
| Runtime↔source drift | two `madoka` trees | REACHED | ISSUE-6 (active drift + historical bug) |
| Vendored `ble_client` C++ | `esphome/components/ble_client/**` | NOT REACHED | out of scope — upstream ESPHome copy, not this project's code |

**Out of scope (with reason):** `esphome/components/ble_client/**` (vendored upstream ESPHome component); `deploy.ps1`, `.github/workflows/ci.yml`, docs/translations (not decision/state code).

## Verification budget

8 candidate findings → 8 parallel adversarial reviewers (opus, read-only); ~250k subagent tokens; 1 refuted, 1 severity-corrected (ISSUE-4 HIGH→MEDIUM), 2 severity-corrected at scope (parse cluster HIGH→MEDIUM, unconditional-poll MEDIUM→LOW). No finding fell below the 80-confidence threshold after verification.
