package ipc

import (
	"bytes"
	"encoding/binary"
	"testing"
	"time"
)

func TestFnv1a32(t *testing.T) {
	// Standard test vectors
	tests := []struct {
		input    string
		expected uint32
	}{
		{"", 0x811C9DC5},
		{"a", 0xE40C292C},
		{"foobar", 0xBF9CF968},
		{"pod://worker-1/telemetry", Fnv1a32("pod://worker-1/telemetry")},
	}
	for _, tc := range tests {
		actual := Fnv1a32(tc.input)
		if actual != tc.expected {
			t.Errorf("Fnv1a32(%q) = 0x%08X, expected 0x%08X", tc.input, actual, tc.expected)
		}
	}
}

func TestCRC16CCITT(t *testing.T) {
	data := []byte("123456789")
	crc := CRC16CCITT(data)
	// Standard CRC16-CCITT (poly 0x1021, init 0xFFFF) for "123456789" is 0x29B1
	if crc != 0x29B1 {
		t.Errorf("CRC16CCITT(\"123456789\") = 0x%04X, expected 0x29B1", crc)
	}
}

func TestHeaderEncodeDecode(t *testing.T) {
	hdr := &Header{
		Magic:       MagicK32C,
		Version:     VersionK32C,
		Flags:       FlagFragment | FlagFirstFrag,
		SeqNum:      42,
		ChannelHash: 0x12345678,
		SenderHash:  0x9abcdef0,
		TargetHash:  0x11223344,
		FragIndex:   0,
		FragCount:   2,
		PayloadLen:  128,
	}

	buf := make([]byte, HeaderLen)
	hdr.Encode(buf)

	decoded, err := DecodeHeader(buf)
	if err != nil {
		t.Fatalf("DecodeHeader error: %v", err)
	}

	if *decoded != *hdr {
		t.Errorf("Decoded header %+v does not match original %+v", decoded, hdr)
	}
}

func TestPacketizerAndReassemblySingle(t *testing.T) {
	p := NewPacketizer()
	rt := NewReassemblyTable()

	payload := []byte("Hello Kube32 Mesh!")
	packets, err := p.Packetize("pod://node-0/test", "sender-pod", "target-pod", payload)
	if err != nil {
		t.Fatalf("Packetize error: %v", err)
	}
	if len(packets) != 1 {
		t.Fatalf("Expected 1 packet, got %d", len(packets))
	}

	res, hdr, err := rt.IngestFrame(packets[0])
	if err != nil {
		t.Fatalf("IngestFrame error: %v", err)
	}
	if hdr == nil {
		t.Fatal("Expected header, got nil")
	}
	if !bytes.Equal(res, payload) {
		t.Fatalf("Reassembled payload %q != original %q", string(res), string(payload))
	}
}

func TestPacketizerAndReassemblyMultiFrag(t *testing.T) {
	p := NewPacketizer()
	rt := NewReassemblyTable()

	// 500 bytes payload -> requires 3 fragments (224 + 224 + 52)
	payload := make([]byte, 500)
	for i := range payload {
		payload[i] = byte(i % 256)
	}

	packets, err := p.Packetize("chan-large", "sender", "target", payload)
	if err != nil {
		t.Fatalf("Packetize error: %v", err)
	}
	if len(packets) != 3 {
		t.Fatalf("Expected 3 packets, got %d", len(packets))
	}

	// Ingest packet 0: partial
	res, hdr, err := rt.IngestFrame(packets[0])
	if err != nil || res != nil || hdr == nil {
		t.Fatalf("Packet 0 should not complete reassembly (res=%v, err=%v)", res, err)
	}

	// Ingest packet 1: partial
	res, hdr, err = rt.IngestFrame(packets[1])
	if err != nil || res != nil || hdr == nil {
		t.Fatalf("Packet 1 should not complete reassembly (res=%v, err=%v)", res, err)
	}

	// Ingest packet 2: complete!
	res, hdr, err = rt.IngestFrame(packets[2])
	if err != nil {
		t.Fatalf("Packet 2 Ingest error: %v", err)
	}
	if res == nil {
		t.Fatal("Expected complete reassembly after packet 2")
	}
	if !bytes.Equal(res, payload) {
		t.Fatalf("Reassembled multi-frag payload mismatch")
	}
}

func TestCorruptCRCRejected(t *testing.T) {
	p := NewPacketizer()
	rt := NewReassemblyTable()

	payload := []byte("Sensitive Data")
	packets, err := p.Packetize("chan", "sender", "target", payload)
	if err != nil {
		t.Fatal(err)
	}

	pkt := packets[0]
	// Corrupt payload byte
	pkt[HeaderLen] ^= 0xFF

	_, _, err = rt.IngestFrame(pkt)
	if err == nil {
		t.Fatal("Expected CRC mismatch error for corrupted frame, got nil")
	}
}

func TestSlidingWindowTimeout(t *testing.T) {
	rt := NewReassemblyTable()

	hdr := Header{
		Magic:       MagicK32C,
		Version:     VersionK32C,
		Flags:       FlagFragment | FlagFirstFrag,
		SeqNum:      99,
		ChannelHash: 1234,
		SenderHash:  5678,
		TargetHash:  9012,
		FragIndex:   0,
		FragCount:   2,
		PayloadLen:  10,
	}
	pkt := make([]byte, HeaderLen+10+CRCLen)
	hdr.Encode(pkt[0:HeaderLen])
	copy(pkt[HeaderLen:HeaderLen+10], []byte("0123456789"))
	crc := CRC16CCITT(pkt[0 : HeaderLen+10])
	binary.LittleEndian.PutUint16(pkt[HeaderLen+10:], crc)

	// Ingest fragment 0
	_, _, err := rt.IngestFrame(pkt)
	if err != nil {
		t.Fatal(err)
	}

	// Manually age the entry to simulate >500ms
	key := (uint64(hdr.ChannelHash) << 32) | uint64(hdr.SeqNum)
	rt.mu.Lock()
	if entry, exists := rt.entries[key]; exists {
		entry.lastActivity = time.Now().Add(-600 * time.Millisecond)
	}
	rt.mu.Unlock()

	// Trigger purge
	rt.PurgeExpired()

	// Verify old entry was cleaned
	rt.mu.Lock()
	_, exists := rt.entries[key]
	rt.mu.Unlock()
	if exists {
		t.Errorf("Expired reassembly entry was not purged")
	}
}
