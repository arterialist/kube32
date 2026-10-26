package ipc

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"strconv"
	"strings"
	"sync"
	"time"
)

type ReceivedMessage struct {
	ChannelHash uint32    `json:"channel_hash"`
	SenderHash  uint32    `json:"sender_hash"`
	TargetHash  uint32    `json:"target_hash"`
	Payload     []byte    `json:"-"`
	PayloadText string    `json:"payload"`
	Length      int       `json:"length"`
	ReceivedAt  time.Time `json:"received_at"`
	RemoteAddr  string    `json:"remote_addr"`
}

type SendRequest struct {
	ChannelURI string `json:"channel_uri"`
	TargetPod  string `json:"target_pod"`
	SenderPod  string `json:"sender_pod"`
	Payload    string `json:"payload"`
	DestAddr   string `json:"dest_addr"`
	DestPort   int    `json:"dest_port,omitempty"`
}

type ChannelGateway struct {
	udpPort      int
	httpPort     int
	udpConn      *net.UDPConn
	httpServer   *http.Server
	packetizer   *Packetizer
	reassembler  *ReassemblyTable
	routingTable map[string]string // podName -> ip:port
	mailboxes    map[uint32]chan *ReceivedMessage
	mu           sync.RWMutex
	running      bool
	stopCh       chan struct{}
}

func NewChannelGateway(udpPort, httpPort int) *ChannelGateway {
	return &ChannelGateway{
		udpPort:      udpPort,
		httpPort:     httpPort,
		packetizer:   NewPacketizer(),
		reassembler:  NewReassemblyTable(),
		routingTable: make(map[string]string),
		mailboxes:    make(map[uint32]chan *ReceivedMessage),
		stopCh:       make(chan struct{}),
	}
}

func (g *ChannelGateway) RegisterRoute(targetPod, addr string) {
	g.mu.Lock()
	defer g.mu.Unlock()
	g.routingTable[targetPod] = addr
}

func (g *ChannelGateway) Start(ctx context.Context) error {
	g.mu.Lock()
	if g.running {
		g.mu.Unlock()
		return nil
	}
	g.running = true
	g.mu.Unlock()

	// 1. Bind UDP Socket (IPv4)
	laddr, err := net.ResolveUDPAddr("udp4", fmt.Sprintf("0.0.0.0:%d", g.udpPort))
	if err != nil {
		return fmt.Errorf("resolve udp addr error: %w", err)
	}
	conn, err := net.ListenUDP("udp4", laddr)
	if err != nil {
		return fmt.Errorf("listen udp error: %w", err)
	}
	g.udpConn = conn
	log.Printf("[IPC-GATEWAY] Bound UDP listener on :%d", g.udpPort)

	// Launch UDP listener goroutine
	go g.udpListenLoop()

	// 2. Start HTTP Server
	mux := http.NewServeMux()
	mux.HandleFunc("/healthz", func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusOK)
		w.Write([]byte(`{"status":"ok"}`))
	})
	mux.HandleFunc("/api/v1/channels/", g.handleChannelsAPI)

	g.httpServer = &http.Server{
		Addr:    fmt.Sprintf(":%d", g.httpPort),
		Handler: mux,
	}

	go func() {
		log.Printf("[IPC-GATEWAY] HTTP REST server listening on :%d", g.httpPort)
		if err := g.httpServer.ListenAndServe(); err != nil && err != http.ErrServerClosed {
			log.Printf("[IPC-GATEWAY] HTTP server error: %v", err)
		}
	}()

	// Monitor context cancellation
	go func() {
		select {
		case <-ctx.Done():
			g.Stop()
		case <-g.stopCh:
		}
	}()

	return nil
}

func (g *ChannelGateway) Stop() {
	g.mu.Lock()
	if !g.running {
		g.mu.Unlock()
		return
	}
	g.running = false
	close(g.stopCh)
	g.mu.Unlock()

	if g.udpConn != nil {
		g.udpConn.Close()
	}
	if g.httpServer != nil {
		ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
		defer cancel()
		g.httpServer.Shutdown(ctx)
	}
	log.Printf("[IPC-GATEWAY] Stopped gateway")
}

func (g *ChannelGateway) udpListenLoop() {
	buf := make([]byte, 2048)
	for {
		n, remoteAddr, err := g.udpConn.ReadFromUDP(buf)
		if err != nil {
			g.mu.RLock()
			running := g.running
			g.mu.RUnlock()
			if !running {
				return
			}
			continue
		}
		log.Printf("[IPC-GATEWAY] ReadFromUDP received %d bytes from %v", n, remoteAddr)

		frame := make([]byte, n)
		copy(frame, buf[:n])

		payload, hdr, err := g.reassembler.IngestFrame(frame)
		if err != nil {
			log.Printf("[IPC-GATEWAY] Malformed frame from %s: %v", remoteAddr, err)
			continue
		}

		if payload != nil && hdr != nil {
			// Message completely reassembled!
			msg := &ReceivedMessage{
				ChannelHash: hdr.ChannelHash,
				SenderHash:  hdr.SenderHash,
				TargetHash:  hdr.TargetHash,
				Payload:     payload,
				PayloadText: string(payload),
				Length:      len(payload),
				ReceivedAt:  time.Now(),
				RemoteAddr:  remoteAddr.String(),
			}

			g.mu.Lock()
			ch, exists := g.mailboxes[hdr.ChannelHash]
			if !exists {
				ch = make(chan *ReceivedMessage, 64)
				g.mailboxes[hdr.ChannelHash] = ch
			}
			g.mu.Unlock()

			select {
			case ch <- msg:
			default:
				// Mailbox full, drop oldest or discard
				log.Printf("[IPC-GATEWAY] Mailbox 0x%08X full, dropping message", hdr.ChannelHash)
			}
		}
	}
}

// SendMessage delivers payload over K32C UDP to target destination.
func (g *ChannelGateway) SendMessage(channelURI, senderPod, targetPod string, payload []byte, destAddr string) error {
	if destAddr == "" {
		g.mu.RLock()
		route, exists := g.routingTable[targetPod]
		g.mu.RUnlock()
		if exists {
			destAddr = route
		} else {
			return fmt.Errorf("no route for target pod %s and no destAddr provided", targetPod)
		}
	}

	if strings.HasPrefix(destAddr, ":") {
		destAddr = "127.0.0.1" + destAddr
	}

	packets, err := g.packetizer.Packetize(channelURI, senderPod, targetPod, payload)
	if err != nil {
		return fmt.Errorf("packetize error: %w", err)
	}

	raddr, err := net.ResolveUDPAddr("udp", destAddr)
	if err != nil {
		return fmt.Errorf("resolve destAddr %s error: %w", destAddr, err)
	}

	for _, pkt := range packets {
		if g.udpConn != nil {
			_, err = g.udpConn.WriteToUDP(pkt, raddr)
			if err != nil {
				return fmt.Errorf("sendto %s error: %w", destAddr, err)
			}
		} else {
			// Ephemeral socket fallback
			conn, err := net.DialUDP("udp", nil, raddr)
			if err != nil {
				return err
			}
			conn.Write(pkt)
			conn.Close()
		}
	}
	return nil
}

// RecvMessage retrieves the next reassembled message for a given channel URI or hash.
func (g *ChannelGateway) RecvMessage(channelURI string, timeout time.Duration) (*ReceivedMessage, error) {
	chanHash := Fnv1a32(channelURI)

	g.mu.Lock()
	ch, exists := g.mailboxes[chanHash]
	if !exists {
		ch = make(chan *ReceivedMessage, 64)
		g.mailboxes[chanHash] = ch
	}
	g.mu.Unlock()

	select {
	case msg := <-ch:
		return msg, nil
	case <-time.After(timeout):
		return nil, nil // Timed out
	}
}

func normalizeChannelURI(uri string) string {
	if strings.HasPrefix(uri, "pod:/") && !strings.HasPrefix(uri, "pod://") {
		return "pod://" + strings.TrimPrefix(uri, "pod:/")
	}
	return uri
}

func (g *ChannelGateway) handleChannelsAPI(w http.ResponseWriter, r *http.Request) {
	// Pattern: /api/v1/channels/{channel_uri}/send or /recv, or ?channel_uri=...
	reqURI := r.RequestURI
	if idx := strings.Index(reqURI, "?"); idx != -1 {
		reqURI = reqURI[:idx]
	}

	channelURIQuery := r.URL.Query().Get("channel_uri")

	// Check if path ends in /send or /recv
	if strings.HasSuffix(reqURI, "/send") {
		uriPart := strings.TrimPrefix(reqURI, "/api/v1/channels/")
		uriPart = strings.TrimSuffix(uriPart, "/send")
		if channelURIQuery != "" {
			uriPart = channelURIQuery
		}
		g.handleSend(w, r, normalizeChannelURI(uriPart))
		return
	} else if strings.HasSuffix(reqURI, "/recv") {
		uriPart := strings.TrimPrefix(reqURI, "/api/v1/channels/")
		uriPart = strings.TrimSuffix(uriPart, "/recv")
		if channelURIQuery != "" {
			uriPart = channelURIQuery
		}
		g.handleRecv(w, r, normalizeChannelURI(uriPart))
		return
	} else if strings.HasSuffix(reqURI, "/channels/send") {
		g.handleSend(w, r, normalizeChannelURI(channelURIQuery))
		return
	} else if strings.HasSuffix(reqURI, "/channels/recv") {
		g.handleRecv(w, r, normalizeChannelURI(channelURIQuery))
		return
	}

	http.Error(w, `{"error":"Not Found"}`, http.StatusNotFound)
}

func (g *ChannelGateway) handleSend(w http.ResponseWriter, r *http.Request, defaultURI string) {
	if r.Method != http.MethodPost {
		http.Error(w, `{"error":"Method Not Allowed"}`, http.StatusMethodNotAllowed)
		return
	}

	body, err := io.ReadAll(r.Body)
	if err != nil {
		http.Error(w, fmt.Sprintf(`{"error":"read body failed: %v"}`, err), http.StatusBadRequest)
		return
	}

	var req SendRequest
	if len(body) > 0 {
		_ = json.Unmarshal(body, &req)
	}

	if req.ChannelURI == "" {
		req.ChannelURI = defaultURI
	}
	if req.ChannelURI == "" {
		req.ChannelURI = r.URL.Query().Get("channel_uri")
	}
	if req.TargetPod == "" {
		req.TargetPod = r.URL.Query().Get("target_pod")
	}
	if req.SenderPod == "" {
		req.SenderPod = r.URL.Query().Get("sender_pod")
	}
	if req.Payload == "" {
		req.Payload = r.URL.Query().Get("payload")
	}
	if req.DestAddr == "" {
		req.DestAddr = r.URL.Query().Get("dest_addr")
	}
	if req.DestAddr == "" && req.DestPort > 0 {
		req.DestAddr = fmt.Sprintf("127.0.0.1:%d", req.DestPort)
	} else if req.DestPort > 0 && !strings.Contains(req.DestAddr, ":") {
		req.DestAddr = fmt.Sprintf("%s:%d", req.DestAddr, req.DestPort)
	} else if strings.HasPrefix(req.DestAddr, ":") {
		req.DestAddr = "127.0.0.1" + req.DestAddr
	}

	if req.ChannelURI == "" || req.Payload == "" {
		http.Error(w, `{"error":"channel_uri and payload required"}`, http.StatusBadRequest)
		return
	}

	err = g.SendMessage(req.ChannelURI, req.SenderPod, req.TargetPod, []byte(req.Payload), req.DestAddr)
	if err != nil {
		http.Error(w, fmt.Sprintf(`{"error":"send failed: %v"}`, err), http.StatusInternalServerError)
		return
	}

	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusOK)
	_ = json.NewEncoder(w).Encode(map[string]interface{}{
		"status":      "Sent",
		"channel_uri": req.ChannelURI,
		"bytes":       len(req.Payload),
		"dest_addr":   req.DestAddr,
	})
}

func (g *ChannelGateway) handleRecv(w http.ResponseWriter, r *http.Request, channelURI string) {
	if r.Method != http.MethodGet {
		http.Error(w, `{"error":"Method Not Allowed"}`, http.StatusMethodNotAllowed)
		return
	}

	timeoutStr := r.URL.Query().Get("timeout")
	timeout := 1000 * time.Millisecond
	if timeoutStr != "" {
		if strings.HasSuffix(timeoutStr, "ms") {
			ms, _ := strconv.Atoi(strings.TrimSuffix(timeoutStr, "ms"))
			if ms > 0 {
				timeout = time.Duration(ms) * time.Millisecond
			}
		} else if strings.HasSuffix(timeoutStr, "s") {
			sec, _ := strconv.Atoi(strings.TrimSuffix(timeoutStr, "s"))
			if sec > 0 {
				timeout = time.Duration(sec) * time.Second
			}
		}
	}

	msg, err := g.RecvMessage(channelURI, timeout)
	if err != nil {
		http.Error(w, fmt.Sprintf(`{"error":"recv failed: %v"}`, err), http.StatusInternalServerError)
		return
	}

	if msg == nil {
		w.WriteHeader(http.StatusNoContent)
		return
	}

	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusOK)
	_ = json.NewEncoder(w).Encode(map[string]interface{}{
		"channel_uri":  channelURI,
		"channel_hash": msg.ChannelHash,
		"sender_hash":  msg.SenderHash,
		"target_hash":  msg.TargetHash,
		"payload":      msg.PayloadText,
		"length":       msg.Length,
		"remote_addr":  msg.RemoteAddr,
	})
}
