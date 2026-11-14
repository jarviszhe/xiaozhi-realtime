package proxy

import (
	"encoding/base64"
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/gorilla/websocket"
	"github.com/jarviszhe/xiaozhi-realtime/tools/realtime-debug/internal/store"
	"github.com/jarviszhe/xiaozhi-realtime/tools/realtime-debug/internal/trace"
)

const (
	maxMessageSize  = 8 * 1024 * 1024
	recordQueueSize = 256
)

type Config struct {
	UpstreamURL string
	Store       *store.Store
}

type Server struct {
	upstreamURL string
	store       *store.Store
	upgrader    websocket.Upgrader
	mu          sync.Mutex
	connections map[*websocket.Conn]*websocket.Conn
	wg          sync.WaitGroup
}

func New(config Config) (*Server, error) {
	if config.UpstreamURL == "" || config.Store == nil {
		return nil, fmt.Errorf("proxy requires upstream URL and store")
	}
	if !strings.HasPrefix(config.UpstreamURL, "ws://") && !strings.HasPrefix(config.UpstreamURL, "wss://") {
		return nil, fmt.Errorf("unsupported upstream URL %q", config.UpstreamURL)
	}
	return &Server{
		upstreamURL: config.UpstreamURL,
		store:       config.Store,
		upgrader: websocket.Upgrader{
			ReadBufferSize:  16 * 1024,
			WriteBufferSize: 16 * 1024,
			CheckOrigin:     func(*http.Request) bool { return true },
		},
		connections: make(map[*websocket.Conn]*websocket.Conn),
	}, nil
}

func (s *Server) Close() {
	s.mu.Lock()
	type pair struct {
		device   *websocket.Conn
		upstream *websocket.Conn
	}
	connections := make([]pair, 0, len(s.connections))
	for device, upstream := range s.connections {
		connections = append(connections, pair{device: device, upstream: upstream})
	}
	s.connections = make(map[*websocket.Conn]*websocket.Conn)
	s.mu.Unlock()
	for _, pair := range connections {
		pair.device.Close()
		pair.upstream.Close()
	}
	s.wg.Wait()
}

func (s *Server) Handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/realtime", s.handle)
	mux.HandleFunc("/", s.handle)
	return mux
}

func (s *Server) handle(w http.ResponseWriter, r *http.Request) {
	if !websocket.IsWebSocketUpgrade(r) {
		http.Error(w, "websocket upgrade required", http.StatusUpgradeRequired)
		return
	}

	upstreamHeader := cloneUpstreamHeaders(r.Header)
	upstream, response, err := websocket.DefaultDialer.Dial(s.upstreamURL, upstreamHeader)
	if err != nil {
		status := http.StatusBadGateway
		if response != nil && response.StatusCode >= 400 {
			status = http.StatusBadGateway
		}
		http.Error(w, "upstream websocket connection failed", status)
		log.Printf("proxy upstream connect failed: %v", err)
		return
	}
	device, err := s.upgrader.Upgrade(w, r, nil)
	if err != nil {
		upstream.Close()
		return
	}
	s.mu.Lock()
	s.connections[device] = upstream
	s.mu.Unlock()
	s.wg.Add(1)
	defer s.wg.Done()
	defer func() {
		s.mu.Lock()
		delete(s.connections, device)
		s.mu.Unlock()
	}()

	device.SetReadLimit(maxMessageSize)
	upstream.SetReadLimit(maxMessageSize)
	bootID := fmt.Sprintf("proxy-%d", time.Now().UnixNano())
	traceID := fmt.Sprintf("proxy-%d", time.Now().UnixNano())
	deviceID := firstHeader(r.Header, "Device-Id", "Client-Id")
	if deviceID == "" {
		deviceID = r.RemoteAddr
	}
	started := time.Now().UTC()
	s.store.SetDevice(store.DeviceStatus{Connected: true, DeviceID: deviceID, BootID: bootID, Remote: r.RemoteAddr, Since: started})
	defer s.store.SetDevice(store.DeviceStatus{})

	recorder := newRecorder(s.store, bootID, traceID, deviceID)
	recorder.enqueue(trace.SessionOpen, trace.Meta{Mode: "proxy", Detail: "websocket proxy session"}, nil)

	var closeOnce sync.Once
	closeBoth := func() {
		closeOnce.Do(func() {
			device.Close()
			upstream.Close()
		})
	}
	var workers sync.WaitGroup
	workers.Add(2)
	go func() {
		defer workers.Done()
		forward(device, upstream, "uplink", recorder, closeBoth)
	}()
	go func() {
		defer workers.Done()
		forward(upstream, device, "downlink", recorder, closeBoth)
	}()
	workers.Wait()
	closeBoth()
	recorder.enqueue(trace.SessionEnd, trace.Meta{Mode: "proxy", Detail: "websocket proxy closed"}, nil)
	recorder.close()
	log.Printf("proxy session closed: device=%s remote=%s", deviceID, r.RemoteAddr)
}

func cloneUpstreamHeaders(src http.Header) http.Header {
	dst := make(http.Header)
	for key, values := range src {
		lower := strings.ToLower(key)
		switch lower {
		case "connection", "upgrade", "host", "content-length", "sec-websocket-key", "sec-websocket-version", "sec-websocket-extensions", "sec-websocket-accept", "origin":
			continue
		}
		for _, value := range values {
			if strings.EqualFold(key, "authorization") || strings.EqualFold(key, "api-key") || strings.EqualFold(key, "workspace") || strings.EqualFold(key, "workspace-id") || strings.EqualFold(key, "client-id") || strings.EqualFold(key, "device-id") {
				dst.Add(key, value)
			}
		}
	}
	return dst
}

func firstHeader(headers http.Header, names ...string) string {
	for _, name := range names {
		if value := headers.Get(name); value != "" {
			return value
		}
	}
	return ""
}

func forward(src, dst *websocket.Conn, direction string, recorder *recorder, closeBoth func()) {
	for {
		kind, payload, err := src.ReadMessage()
		if err != nil {
			closeBoth()
			return
		}
		receivedAt := time.Now().UTC()
		forwardStartAt := time.Now().UTC()
		writeErr := dst.WriteMessage(kind, payload)
		forwardDoneAt := time.Now().UTC()
		recorder.message(direction, kind, payload, receivedAt, forwardStartAt, forwardDoneAt)
		if writeErr != nil {
			closeBoth()
			return
		}
	}
}

type recorder struct {
	store     *store.Store
	bootID    string
	traceID   string
	device    string
	seq       atomic.Uint64
	queue     chan trace.Frame
	done      chan struct{}
	closeOnce sync.Once
	mu        sync.Mutex
	pending   uint64
}

func newRecorder(s *store.Store, bootID, traceID, device string) *recorder {
	r := &recorder{store: s, bootID: bootID, traceID: traceID, device: device, queue: make(chan trace.Frame, recordQueueSize), done: make(chan struct{})}
	go r.run()
	return r
}

func (r *recorder) close() {
	r.closeOnce.Do(func() {
		close(r.queue)
		<-r.done
	})
}

func (r *recorder) run() {
	defer close(r.done)
	for frame := range r.queue {
		if _, err := r.store.Append(frame); err != nil {
			log.Printf("proxy trace write: %v", err)
		}
	}
}

func (r *recorder) enqueue(kind uint16, meta trace.Meta, payload []byte) {
	meta.BootID = r.bootID
	meta.TraceSessionID = r.traceID
	meta.DeviceID = r.device
	if kind != trace.SessionOpen && kind != trace.SessionEnd {
		meta.Seq = fmt.Sprintf("%d", r.seq.Add(1))
	}
	frame := trace.Frame{Kind: kind, Meta: meta, Payload: append([]byte(nil), payload...)}
	r.mu.Lock()
	if r.pending > 0 {
		gapMeta := trace.Meta{
			BootID: r.bootID, TraceSessionID: r.traceID, DeviceID: r.device,
			Seq: fmt.Sprintf("%d", r.seq.Add(1)), Mode: "proxy",
			EventType: "trace_gap", Detail: fmt.Sprintf("dropped_records=%d", r.pending),
		}
		select {
		case r.queue <- trace.Frame{Kind: trace.TraceGap, Meta: gapMeta}:
			r.pending = 0
		default:
			r.pending++
			r.mu.Unlock()
			return
		}
	}
	select {
	case r.queue <- frame:
	default:
		r.pending++
	}
	r.mu.Unlock()
}

func (r *recorder) message(direction string, kind int, payload []byte, receivedAt, forwardStartAt, forwardDoneAt time.Time) {
	if kind != websocket.TextMessage && kind != websocket.BinaryMessage {
		return
	}
	meta := trace.Meta{
		Mode: "proxy", ContentType: direction, Detail: fmt.Sprintf("ws_type=%d", kind),
		ProxyReceivedAt: receivedAt.Format(time.RFC3339Nano),
		ForwardStartAt:  forwardStartAt.Format(time.RFC3339Nano),
		ForwardDoneAt:   forwardDoneAt.Format(time.RFC3339Nano),
	}
	if kind == websocket.TextMessage {
		var event struct {
			Type       string `json:"type"`
			EventID    string `json:"event_id"`
			ResponseID string `json:"response_id"`
			Response   struct {
				ID string `json:"id"`
			} `json:"response"`
			Audio string `json:"audio"`
			Delta string `json:"delta"`
		}
		if json.Unmarshal(payload, &event) == nil {
			meta.EventType, meta.ResponseID, meta.MessageID = event.Type, event.ResponseID, event.EventID
			if meta.ResponseID == "" {
				meta.ResponseID = event.Response.ID
			}
			if event.Type == "input_audio_buffer.append" && event.Audio != "" {
				r.enqueueAudio(direction, event.Audio, 16000)
			} else if event.Type == "response.audio.delta" && event.Delta != "" {
				r.enqueueAudio(direction, event.Delta, 24000)
			}
		}
	}
	if meta.EventType == "" {
		meta.EventType = "websocket.message"
	}
	kindToRecord := uint16(trace.WSRX)
	if direction == "uplink" {
		kindToRecord = trace.WSTX
	}
	r.enqueue(kindToRecord, meta, payload)
}

func (r *recorder) enqueueAudio(direction, encoded string, sampleRate int) {
	pcm, err := base64.StdEncoding.DecodeString(encoded)
	if err != nil || len(pcm) == 0 || len(pcm)%2 != 0 {
		return
	}
	meta := trace.Meta{Mode: "proxy", ContentType: direction, SampleRate: sampleRate, Channels: 1, BitsPerSample: 16, SampleFormat: "signed_int", ByteOrder: "little"}
	if direction == "uplink" {
		r.enqueue(trace.PCMUplink, meta, pcm)
	} else {
		r.enqueue(trace.PCMPlayback, meta, pcm)
	}
}
