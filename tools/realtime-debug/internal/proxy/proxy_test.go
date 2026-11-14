package proxy

import (
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	"github.com/gorilla/websocket"
	"github.com/jarviszhe/xiaozhi-realtime/tools/realtime-debug/internal/store"
)

func TestProxyForwardsHeadersAndApplicationFramesInOrder(t *testing.T) {
	upgrader := websocket.Upgrader{CheckOrigin: func(*http.Request) bool { return true }}
	gotAuth := make(chan string, 1)
	gotMessages := make(chan string, 2)
	upstream := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		gotAuth <- r.Header.Get("Authorization")
		conn, err := upgrader.Upgrade(w, r, nil)
		if err != nil {
			t.Errorf("upstream upgrade: %v", err)
			return
		}
		defer conn.Close()
		for i := 0; i < 2; i++ {
			kind, payload, err := conn.ReadMessage()
			if err != nil {
				return
			}
			gotMessages <- string(payload)
			if err := conn.WriteMessage(kind, payload); err != nil {
				return
			}
		}
	}))
	defer upstream.Close()

	data, err := store.Open(t.TempDir(), 16*1024*1024)
	if err != nil {
		t.Fatal(err)
	}
	defer data.Close()
	upstreamURL := "ws" + upstream.URL[len("http"):]
	service, err := New(Config{UpstreamURL: upstreamURL, Store: data})
	if err != nil {
		t.Fatal(err)
	}
	defer service.Close()
	proxyServer := httptest.NewServer(service.Handler())
	defer proxyServer.Close()
	proxyURL := "ws" + proxyServer.URL[len("http"):] + "/realtime"
	header := http.Header{"Authorization": []string{"Bearer test-key"}, "Device-Id": []string{"device-test"}}
	client, _, err := websocket.DefaultDialer.Dial(proxyURL, header)
	if err != nil {
		t.Fatal(err)
	}
	if err := client.WriteMessage(websocket.TextMessage, []byte(`{"type":"first"}`)); err != nil {
		t.Fatal(err)
	}
	if err := client.WriteMessage(websocket.BinaryMessage, []byte{1, 2, 3}); err != nil {
		t.Fatal(err)
	}
	for i := 0; i < 2; i++ {
		if _, _, err := client.ReadMessage(); err != nil {
			t.Fatal(err)
		}
	}
	client.Close()

	select {
	case auth := <-gotAuth:
		if auth != "Bearer test-key" {
			t.Fatalf("authorization = %q", auth)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("upstream did not receive handshake")
	}
	select {
	case first := <-gotMessages:
		if first != `{"type":"first"}` {
			t.Fatalf("first payload = %q", first)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("missing first frame")
	}
	select {
	case second := <-gotMessages:
		if second != string([]byte{1, 2, 3}) {
			t.Fatalf("second payload = %q", second)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("missing second frame")
	}
	service.Close()
	for _, session := range data.List() {
		events, err := data.Events(session.ID, "", 100)
		if err != nil {
			t.Fatal(err)
		}
		for _, event := range events {
			if event.Kind != "ws_tx" && event.Kind != "ws_rx" {
				continue
			}
			received, err := time.Parse(time.RFC3339Nano, event.Meta.ProxyReceivedAt)
			if err != nil {
				t.Fatal(err)
			}
			started, err := time.Parse(time.RFC3339Nano, event.Meta.ForwardStartAt)
			if err != nil {
				t.Fatal(err)
			}
			done, err := time.Parse(time.RFC3339Nano, event.Meta.ForwardDoneAt)
			if err != nil {
				t.Fatal(err)
			}
			if received.After(started) || started.After(done) {
				t.Fatalf("invalid forwarding times: %v, %v, %v", received, started, done)
			}
		}
	}
}
