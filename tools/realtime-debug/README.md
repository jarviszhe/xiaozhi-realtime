# Realtime Debug

Standalone Go Debug Proxy, collector, and local browser view for XiaoZhi Realtime. Proxy mode transparently relays the ESP32 WebSocket to Aliyun, preserving application frame order and recording the raw bidirectional exchange. The legacy TCP collector remains available for device-side AEC and offline diagnostics.

## Run

Requires Go 1.22 or newer. From this directory:

```sh
go run ./cmd/realtime-debug --collector-listen 192.168.1.20:7319 --proxy-listen 0.0.0.0:7321 --upstream-url 'wss://<workspace>.<region>.maas.aliyuncs.com/api-ws/v1/realtime?model=qwen3.8-omni-flash-realtime'
```

Replace `192.168.1.20` with the development computer's LAN address. The browser opens at `http://127.0.0.1:7320`. Legacy device traces use `tcp://192.168.1.20:7319`; the ESP32 WebSocket Proxy URL is `ws://192.168.1.20:7321/realtime`. `--web-listen`, `--proxy-listen`, `--upstream-url`, `--data-dir`, and `--max-session-mib` override the defaults. Recordings default to the OS user cache directory under `xiaozhi-realtime-debug`, outside this repository.

For Proxy mode, set the ESP32 WebSocket address to `ws://192.168.1.20:7321/realtime` in the device configuration page. The ESP32 keeps sending its existing Aliyun authorization Header; the proxy copies it only for the upstream handshake and never records it. The upstream URL is configured on the Go command line, while WebSocket application frames are forwarded unchanged and in order.

After the board joins Wi-Fi, open `http://<device-ip>/`. Under **Debug capture**, enter the collector address, enable capture, and choose level 0 (local events), 1 (events and WebSocket payloads), or 2 (plus PCM). Save and reboot the board. The same page selects XiaoZhi or Aliyun and configures the Aliyun region, workspace ID, and API Key. A blank Key field retains the saved Key; the page never returns its value.

The collector accepts one device connection at a time. The device must send a `hello` frame first. The stream is plain TCP with no pairing token; use it only on a controlled development LAN. The browser listener defaults to loopback. The trace sender must never include Aliyun API keys or HTTP authentication headers.

## Frame protocol

Each frame is `uint32_be length`, followed by `uint16_be version` (`1`), `uint16_be kind`, `uint32_be metadata_length`, UTF-8 JSON metadata, and payload bytes. The maximum frame length is 256 KiB and metadata is limited to 4 KiB. Large messages are split into numbered frames by the sender. See `internal/trace/frame.go` for kinds and metadata fields.

The first `hello` frame needs `boot_id`, `device_id`, and `firmware_version`. Every later frame needs the same `boot_id`, a `trace_session_id`, decimal-string `seq`, and `mono_us`. PCM frames also require `sample_rate`, `channels`, `bits_per_sample`, `sample_format: "signed_int"`, and `byte_order: "little"`. Raw WebSocket payloads retain their exact application-level bytes; `ws_tx_result` reports the local send call result separately.

## Recordings

Each session has `manifest.json`, `events.jsonl`, `payloads.dat`, `pcm-uplink.dat`, and `pcm-playback.dat`. The browser reads event summaries, streams PCM as WAV without rewriting the source data, and exports a session ZIP. Audio format changes create separate WAV segments. A session that reaches `--max-session-mib` is marked truncated. On restart, the service rebuilds summaries from complete event-index lines, trims unindexed payload tails, and marks an unfinished session as interrupted. It does not append new frames to that old trace ID. Automatic retention cleanup is not implemented.

Run `go test ./...` for the collector's host tests. Validate it with a device by running the collector and inspecting a trace in the browser. A full trace queue creates a visible `trace_gap` marker and voice traffic continues.

For a manual smoke check without hardware, start the collector and run `node scripts/send-sample.mjs 127.0.0.1 7319`. Open the browser page and inspect the sample session, JSON payload, uplink WAV, and ZIP export. This script sends a synthetic trace; it does not contact Aliyun.
## AEC diagnostics

On the Waveshare 3.5B software AEC build, dedicated `/api/test/pcm` tests at trace level 2 record `aec_mic`,
`aec_ref`, and `aec_out` as 16 kHz mono PCM. The collector stores each track in
its own `pcm-*.dat` file and exposes WAV segments through the existing audio
endpoint. `GET /api/sessions/{id}/aec-report` returns sample continuity,
clipping, RMS, and a full-track attenuation estimate. This estimate is meaningful
only during far-end-only playback. A complete report means the trace has all
three tracks and no recorded gaps; it does not prove acoustic AEC quality.

The firmware accepts deterministic 24 kHz mono signed 16-bit little-endian PCM
at `POST /api/test/pcm?delay_ms=0&aec=1`. `delay_ms` is optional (firmware default),
in the range 0–250; `aec=0` provides a bypass control. The endpoint requires an
idle device and accepts at most 20 seconds. It temporarily pauses wake detection,
keeps Wi-Fi awake, opens one trace, and includes an AFE preroll and acoustic tail.
It restores the previous wake, AEC, and Wi-Fi power-save states after the test.

The PCM response also includes `stats_before` (after preroll) and `stats_after`
(after playback and the acoustic tail). `GET /api/test/stats` returns an idle
snapshot; the single HTTP worker cannot serve that GET during a PCM POST.
Snapshots contain internal/PSRAM free memory, lifetime heap minima, AFE feed/fetch
counters and, when configured, FreeRTOS per-task runtime and stack high-water
marks in bytes. The Python runner saves the complete response and computes CPU
from unsigned runtime deltas. `one_core_percent` uses one CPU core as 100%;
`cpu_busy_total_percent` uses both cores together as 100%. Compare AEC on/off
with identical tracing and playback; the difference estimates incremental AEC
CPU, while bypass still retains AFE/AEC allocations and cannot measure its RAM.
The diagnostic PCM body and trace buffers also consume memory.

`feed_average_ms` measures elapsed time, including preemption or backpressure,
per feed frame (currently 32 ms). The maximum is since boot, not per capture.
Blocking fetch wait is excluded from processing time. Feed/fetch sample rates,
backlog change, overruns, fetch errors and trace continuity jointly check real
time throughput; they do not measure end-to-end cloud latency. Heap minima are
since boot and must not be described as the minimum within one test. Runtime
counter windows must be shorter than a counter wrap (about 71 minutes for a
32-bit microsecond counter). Onset energy ratios use the nominal track sample
indices for the first 0.5/1/2 seconds after reference onset; use bypass controls
and full waveforms when interpreting short startup intervals.

Use Python with NumPy to scan delays or test the compiled default. PC near speech
playback uses Windows `winsound`; the PC speaker must be beside the device.
The following examples use a prerecorded device voice and an independent near
voice. Keep the PC volume and device position fixed between baselines and trials.

```sh
python scripts/aec_test.py device-voice.wav --output captures --device http://192.168.5.15 --delays 0,20,40 --repeats 2 --peak 0.12 --label scan
python scripts/aec_test.py device-voice.wav --output captures --delays default --peak 0.12 --aec 0 --label far-control
python scripts/aec_test.py device-voice.wav --output captures --delays default --peak 0 --near near-voice.wav --near-start 3 --label near-only
python scripts/aec_test.py device-voice.wav --output captures --delays default --peak 0.12 --near near-voice.wav --near-start 3 --repeats 2 --label double-talk
```

The script saves the submitted waveform, four original WAV tracks, collector
report, and host analysis for every trial. It rejects interrupted, incomplete,
or gapped traces. Labels must be unique in the output directory. Whole/middle
energy attenuation is meaningful only for device-only speech; a double-talk
recording includes desired near speech and must not be scored by that ratio.
The script does not change the system speaker volume. `--peak` controls the
submitted digital waveform and is not an acoustic loudness measurement.
PC playback starts `--near-start` seconds after the collector observes the new
manual session, independently of PCM upload time. `near-trigger.json` records
the observed session and host playback timestamps. A 0.9 second offset roughly
overlaps initial device speech with the current 500 ms preroll and 500 ms source
silence; this is host-triggered timing, not sample-accurate hardware synchronization.

Compare double-talk recordings with the acoustic microphone baselines:

```sh
python scripts/aec_compare.py captures/double-talk-ddefault-aec1-r1 captures/double-talk-ddefault-aec1-r2 --near-only captures/near-only-ddefault-aec1-r1/aec_mic.wav --far-only captures/far-control-ddefault-aec0-r1/aec_mic.wav --output listening
```

This reports near/far waveform correlation and near projection gain, and selects
the trial with the strongest near correlation for listening. These estimates
depend on a stable acoustic setup and are not ERLE or a perceptual speech score.
The exported before/after WAVs use one constant time shift and exactly the same
gain; the manifest records both. Original captures are retained. The combined
file plays the entire raw capture, one second of silence, then the entire AEC
output, preserving the startup interval. Local tests do not prove cloud VAD,
continuous upload during speaking, or response cancellation.

### Conversation mode and Realtime integration

With device debug capture enabled, `POST /api/test/chat?action=cycle|click|stop`
queues the same application mode-cycle/single-click actions as the board button,
or ends a conversation. The endpoint rejects requests when debug is disabled.
`GET /api/test/stats` reports the current `mode`, `state`, and AEC enable state
on software-AEC builds. Physical single/double-click recognition still needs a
button test; the HTTP action cannot validate debouncing.
The snapshots also include `codec_reference`, `input_channels`, microphone
gain and output volume. An ES8311 internal DAC reference uses two capture slots
(microphone left, digital DAC reference right). Software reference delay scans
do not shift that hardware-supplied reference.

Prepare two local PCM WAV files: a request for a long reply and an interruption
request. The computer speaker should be beside the device. Run on Windows:

```powershell
python tools/realtime-debug/scripts/realtime_test.py --mode realtime `
  --question question.wav --interrupt interrupt.wav `
  --expect-input 'stop.*three.*five' --expect-reply '(eight|8)' `
  --output tmp-realtime-test
```

The runner waits for actual device playback before playing the interruption,
ends the session, waits for the trace to close, and exports event timing,
resource snapshots, computer speech markers and audio tracks. Markers include
the estimated device time and the clock synchronization uncertainty.
It writes `result.json` on run/export failures and exits nonzero on failure.
Interruption acceptance requires explicit input/reply regular expressions, a
speech-start event within `--max-detection-ms` (default 1500), continued uplink,
the matching completed answer, and at least five seconds of subsequent evidence.
It rejects early playback cancellation, later self-conversation, incomplete
traces, and sessions that close before the observation ends. Speech-start counts
alone are not acceptance: a real interruption can span several detected turns.
Server `audio_start_ms` is mapped through uploaded PCM to expose delayed event
delivery; this is an estimate of submission timing, not microphone onset or an
exact measurement of when the speaker stopped. Computer speech markers describe
the playback API call and do not measure the PC sound card's acoustic latency.
`--far-only --observe-seconds 30` tests a device reply without PC interruption.
`--mode manual` or `--mode auto` without `--interrupt` exercises turn-taking.

For controlled diagnostics, `--aec-policy playback|continuous` selects a temporary
Realtime policy; Manual/Auto still bypass AEC. `--aec-tracks` adds mic/reference/
output tracks to the next conversation. `--input-gain 18|24|30` temporarily sets
the codec's microphone gain in dB. These options require the matching diagnostic
firmware and an idle device. They are not persisted; ES8311 gain is quantized to
its 6 dB PGA steps. Lower gain must be tested for both clipping and near speech.
`--output-volume 1..100` records the original speaker volume, applies a test
volume and restores it after stopping, including on setup/run failures. This
uses the codec's persistent volume setter, so check `result.json` for restoration
errors. Other diagnostic choices last until changed or the device restarts.

With matching firmware and debug enabled, `POST /api/test/chat?action=volume-80`
sets the speaker volume through the existing codec settings. Values must be
integers from 1 to 100 and the device must be idle. HTTP 202 means queued; verify
`output_volume` in `/api/test/stats`. This setting persists across reboots.

At capture level 2, audio-bearing WebSocket JSON is omitted in favor of PCM
tracks; other cloud events remain available. Level 1 retains the raw WebSocket
audio messages. Playback PCM records samples submitted locally, not both received
downlink and submitted output. Live conversations record uplink (the processed
microphone signal) and playback, keeping diagnostic traffic below a simultaneous
five-track capture. The optional `--aec-tracks` profile and dedicated AEC tests
additionally record mic/reference/output
in 64 ms batches, including the final partial batch. Compare the two test profiles
separately; live conversation recordings alone cannot quantify echo suppression.
