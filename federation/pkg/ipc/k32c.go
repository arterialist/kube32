package ipc

import (
	"encoding/binary"
	"errors"
	"fmt"
	"sync"
	"time"
)

const (
	MagicK32C           uint32 = 0x4332334B // 'K', '3', '2', 'C' in little-endian
	VersionK32C         uint8  = 0x01
	MaxPacketLen        int    = 250 // ESP-NOW MTU
	HeaderLen           int    = 24  // Fixed header size
	CRCLen              int    = 2   // CRC16 size
	MaxPayloadChunk     int    = 224 // 250 - 24 - 2
	RingSlotSize        int    = 1024 // Max reassembled message size
	ReassemblyTimeoutMs int64  = 500

	FlagFragment  uint8 = 0x01
	FlagFirstFrag uint8 = 0x02
	FlagLastFrag  uint8 = 0x04
	FlagAckReq    uint8 = 0x08
	FlagPing      uint8 = 0x10
)

type Header struct {
	Magic       uint32
	Version     uint8
	Flags       uint8
	SeqNum      uint16
	ChannelHash uint32
	SenderHash  uint32
	TargetHash  uint32
	FragIndex   uint8
	FragCount   uint8
	PayloadLen  uint16
}

func (h *Header) Encode(buf []byte) {
	binary.LittleEndian.PutUint32(buf[0:4], h.Magic)
	buf[4] = h.Version
	buf[5] = h.Flags
	binary.LittleEndian.PutUint16(buf[6:8], h.SeqNum)
	binary.LittleEndian.PutUint32(buf[8:12], h.ChannelHash)
	binary.LittleEndian.PutUint32(buf[12:16], h.SenderHash)
	binary.LittleEndian.PutUint32(buf[16:20], h.TargetHash)
	buf[20] = h.FragIndex
	buf[21] = h.FragCount
	binary.LittleEndian.PutUint16(buf[22:24], h.PayloadLen)
}

func DecodeHeader(buf []byte) (*Header, error) {
	if len(buf) < HeaderLen {
		return nil, errors.New("buffer smaller than K32C header")
	}
	h := &Header{
		Magic:       binary.LittleEndian.Uint32(buf[0:4]),
		Version:     buf[4],
		Flags:       buf[5],
		SeqNum:      binary.LittleEndian.Uint16(buf[6:8]),
		ChannelHash: binary.LittleEndian.Uint32(buf[8:12]),
		SenderHash:  binary.LittleEndian.Uint32(buf[12:16]),
		TargetHash:  binary.LittleEndian.Uint32(buf[16:20]),
		FragIndex:   buf[20],
		FragCount:   buf[21],
		PayloadLen:  binary.LittleEndian.Uint16(buf[22:24]),
	}
	if h.Magic != MagicK32C {
		return nil, fmt.Errorf("invalid magic: 0x%08X (expected 0x%08X)", h.Magic, MagicK32C)
	}
	if h.Version != VersionK32C {
		return nil, fmt.Errorf("unsupported version: %d", h.Version)
	}
	return h, nil
}

// CRC16CCITT calculates CRC16-CCITT with polynomial 0x1021 and initial value 0xFFFF.
func CRC16CCITT(data []byte) uint16 {
	var crc uint16 = 0xFFFF
	for _, b := range data {
		crc ^= uint16(b) << 8
		for i := 0; i < 8; i++ {
			if (crc & 0x8000) != 0 {
				crc = (crc << 1) ^ 0x1021
			} else {
				crc = crc << 1
			}
		}
	}
	return crc
}

// Fnv1a32 computes 32-bit FNV-1a hash matching Kube32's C++ and Python implementations.
func Fnv1a32(text string) uint32 {
	var h uint32 = 0x811C9DC5
	for i := 0; i < len(text); i++ {
		h ^= uint32(text[i])
		h = (h * 0x01000193) & 0xFFFFFFFF
	}
	return h
}

// Packetizer breaks down payloads into K32C packets.
type Packetizer struct {
	seqGen uint16
	mu     sync.Mutex
}

func NewPacketizer() *Packetizer {
	return &Packetizer{}
}

func (p *Packetizer) Packetize(channelURI, senderID, targetPod string, data []byte) ([][]byte, error) {
	if len(data) > RingSlotSize {
		return nil, fmt.Errorf("data length %d exceeds max ring slot size %d", len(data), RingSlotSize)
	}
	p.mu.Lock()
	p.seqGen++
	seq := p.seqGen
	p.mu.Unlock()

	chanHash := Fnv1a32(channelURI)
	senderHash := Fnv1a32(senderID)
	targetHash := Fnv1a32(targetPod)

	fragCount := (len(data) + MaxPayloadChunk - 1) / MaxPayloadChunk
	if fragCount == 0 {
		fragCount = 1
	}
	if fragCount > 32 {
		return nil, fmt.Errorf("fragment count %d exceeds maximum 32", fragCount)
	}

	packets := make([][]byte, fragCount)
	for i := 0; i < fragCount; i++ {
		offset := i * MaxPayloadChunk
		chunkLen := len(data) - offset
		if chunkLen > MaxPayloadChunk {
			chunkLen = MaxPayloadChunk
		}

		hdr := Header{
			Magic:       MagicK32C,
			Version:     VersionK32C,
			SeqNum:      seq,
			ChannelHash: chanHash,
			SenderHash:  senderHash,
			TargetHash:  targetHash,
			FragIndex:   uint8(i),
			FragCount:   uint8(fragCount),
			PayloadLen:  uint16(chunkLen),
		}
		if fragCount > 1 {
			hdr.Flags |= FlagFragment
		}
		if i == 0 {
			hdr.Flags |= FlagFirstFrag
		}
		if i == fragCount-1 {
			hdr.Flags |= FlagLastFrag
		}

		frameBytes := HeaderLen + chunkLen
		pkt := make([]byte, frameBytes+CRCLen)
		hdr.Encode(pkt[0:HeaderLen])
		if chunkLen > 0 {
			copy(pkt[HeaderLen:frameBytes], data[offset:offset+chunkLen])
		}

		crc := CRC16CCITT(pkt[0:frameBytes])
		binary.LittleEndian.PutUint16(pkt[frameBytes:frameBytes+CRCLen], crc)
		packets[i] = pkt
	}
	return packets, nil
}

type ReassemblyEntry struct {
	channelHash   uint32
	senderHash    uint32
	seqNum        uint16
	totalFrags    uint8
	receivedFrags uint8
	receivedMask  uint32
	totalBytes    uint32
	lastActivity  time.Time
	buffer        [RingSlotSize]byte
}

type ReassemblyTable struct {
	mu      sync.Mutex
	entries map[uint64]*ReassemblyEntry
}

func NewReassemblyTable() *ReassemblyTable {
	return &ReassemblyTable{
		entries: make(map[uint64]*ReassemblyEntry),
	}
}

// PurgeExpired removes entries that have exceeded ReassemblyTimeoutMs.
func (rt *ReassemblyTable) PurgeExpired() {
	rt.mu.Lock()
	defer rt.mu.Unlock()
	rt.purgeExpiredLocked(time.Now())
}

func (rt *ReassemblyTable) purgeExpiredLocked(now time.Time) {
	for k, v := range rt.entries {
		if now.Sub(v.lastActivity) > time.Duration(ReassemblyTimeoutMs)*time.Millisecond {
			delete(rt.entries, k)
		}
	}
}

// IngestFrame validates a single K32C frame and returns the reassembled payload if complete.
func (rt *ReassemblyTable) IngestFrame(frame []byte) ([]byte, *Header, error) {
	if len(frame) < HeaderLen+CRCLen {
		return nil, nil, errors.New("frame truncated below minimum length")
	}

	hdr, err := DecodeHeader(frame)
	if err != nil {
		return nil, nil, err
	}
	if int(hdr.PayloadLen) > MaxPayloadChunk || HeaderLen+int(hdr.PayloadLen)+CRCLen > len(frame) {
		return nil, nil, errors.New("invalid payload length in header")
	}

	expectedCRC := CRC16CCITT(frame[0 : HeaderLen+int(hdr.PayloadLen)])
	packetCRC := binary.LittleEndian.Uint16(frame[HeaderLen+int(hdr.PayloadLen) : HeaderLen+int(hdr.PayloadLen)+CRCLen])
	if expectedCRC != packetCRC {
		return nil, nil, fmt.Errorf("crc mismatch: expected 0x%04X, got 0x%04X", expectedCRC, packetCRC)
	}

	payload := frame[HeaderLen : HeaderLen+int(hdr.PayloadLen)]

	// Fast path: Single unfragmented packet
	if hdr.FragCount <= 1 {
		res := make([]byte, len(payload))
		copy(res, payload)
		return res, hdr, nil
	}

	// Multi-fragment reassembly
	if hdr.FragIndex >= hdr.FragCount || hdr.FragCount > 32 {
		return nil, nil, errors.New("fragment index out of bounds")
	}
	offset := int(hdr.FragIndex) * MaxPayloadChunk
	if offset+int(hdr.PayloadLen) > RingSlotSize {
		return nil, nil, errors.New("reassembled payload exceeds maximum slot buffer")
	}

	rt.mu.Lock()
	defer rt.mu.Unlock()

	now := time.Now()
	rt.purgeExpiredLocked(now)

	key := (uint64(hdr.ChannelHash) << 32) | uint64(hdr.SeqNum)
	entry, exists := rt.entries[key]
	if !exists {
		if len(rt.entries) >= 64 {
			return nil, nil, errors.New("reassembly table capacity exceeded")
		}
		entry = &ReassemblyEntry{
			channelHash:  hdr.ChannelHash,
			senderHash:   hdr.SenderHash,
			seqNum:       hdr.SeqNum,
			totalFrags:   hdr.FragCount,
			lastActivity: now,
		}
		rt.entries[key] = entry
	}

	entry.lastActivity = now
	bit := uint32(1) << hdr.FragIndex
	if (entry.receivedMask & bit) == 0 {
		entry.receivedMask |= bit
		entry.receivedFrags++
		copy(entry.buffer[offset:offset+int(hdr.PayloadLen)], payload)
		entry.totalBytes += uint32(hdr.PayloadLen)
	}

	if entry.receivedFrags == entry.totalFrags {
		completeData := make([]byte, entry.totalBytes)
		copy(completeData, entry.buffer[0:entry.totalBytes])
		delete(rt.entries, key)
		return completeData, hdr, nil
	}

	return nil, hdr, nil
}
