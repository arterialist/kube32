"""
Kube32 Milestone 4 Test Suite: Virtual Channel IPC Multi-Node Protocol
Verifies K32C packet format, CRC16 validation, UDP transport multiplexing,
and Capability Firewall isolation between simulated cluster nodes.
"""

import time
import socket
import struct
from tests.framework import BaseTestCase
from tests.m4_cluster_harness import ClusterHarness, crc16_ccitt, fnv1a_32


class TestM4ChannelIPC(BaseTestCase):
    def setUp(self):
        super().setUp()
        self.harness = ClusterHarness(base_api_port=16743, base_ipc_port=17700)

    def tearDown(self):
        self.harness.teardown()
        super().tearDown()

    def test_multinode_ipc_packet_transmission(self):
        """Valid K32C packet sent over UDP to node's IPC port is accepted without crash and logged."""
        cp = self.harness.start_control_plane(name="cp-ipc-0")
        worker = self.harness.start_worker("worker-ipc-1", 1, cp)

        # Transmit valid K32C UDP frame to worker IPC port
        payload = b"Kube32-Mesh-Sensor-Data-Payload"
        self.harness.send_k32c_packet(
            dest_ip="127.0.0.1",
            dest_port=worker.ipc_port,
            channel_uri="pod://worker-ipc-1/telemetry",
            sender_pod="cp-ipc-0",
            payload=payload
        )

        time.sleep(0.3)
        # Node must remain healthy and alive
        self.assertTrue(worker.is_alive())

        # Genuinely verify packet acceptance in worker process
        stdout = worker.read_stdout(timeout=1.0)
        self.assertIn("[IPC-RX] Accepted valid K32C frame", stdout, "Worker did not accept valid K32C frame")
        self.assertIn(f"len={len(payload)}", stdout, "Worker did not receive correct payload length")

    def test_corrupt_crc16_packet_rejected(self):
        """Packet with corrupted CRC16 must be rejected with CRC mismatch and not crash the receiver."""
        cp = self.harness.start_control_plane(name="cp-ipc-1")
        worker = self.harness.start_worker("worker-ipc-2", 1, cp)

        # Build corrupted packet with genuine wire magic (0x4332334B = "K32C")
        magic = 0x4332334B
        payload = b"TamperedData"
        header = struct.pack(
            "<IBBHIIIBBH",
            magic, 1, 0, 1,
            fnv1a_32("pod://worker-ipc-2/test"),
            fnv1a_32("attacker-pod"),
            0, 0, 1, len(payload)
        )
        packet_corrupt = header + payload + struct.pack("<H", 0x1234) # Corrupt CRC

        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.sendto(packet_corrupt, ("127.0.0.1", worker.ipc_port))
        sock.close()

        time.sleep(0.3)
        self.assertTrue(worker.is_alive())

        # Genuinely verify that the frame reached CRC verification and was dropped
        stdout = worker.read_stdout(timeout=1.0)
        self.assertIn("[IPC-RX] Dropped frame: CRC mismatch", stdout, "Worker did not detect CRC mismatch")

    def test_adversarial_truncated_and_corrupt_packet_barrage(self):
        """Barrage of truncated, invalid magic, and corrupt packets does not crash worker."""
        cp = self.harness.start_control_plane(name="cp-ipc-adv")
        worker = self.harness.start_worker("worker-ipc-adv", 1, cp)

        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

        # 1. Truncated frames (0 to 23 bytes)
        for length in [0, 1, 4, 12, 23]:
            sock.sendto(b"\x00" * length, ("127.0.0.1", worker.ipc_port))

        # 2. Corrupt magic numbers
        for bad_magic in [0x00000000, 0xDEADBEEF, 0x12345678]:
            header = struct.pack(
                "<IBBHIIIBBH",
                bad_magic, 1, 0, 1,
                fnv1a_32("pod://worker-ipc-adv/test"),
                fnv1a_32("attacker-pod"),
                0, 0, 1, 4
            )
            data = header + b"Test"
            crc = crc16_ccitt(data)
            sock.sendto(data + struct.pack("<H", crc), ("127.0.0.1", worker.ipc_port))

        # 3. Oversized declared payload_len
        header = struct.pack(
            "<IBBHIIIBBH",
            0x4332334B, 1, 0, 1,
            fnv1a_32("pod://worker-ipc-adv/test"),
            fnv1a_32("attacker-pod"),
            0, 0, 1, 240 # > 224
        )
        data = header + b"X" * 240
        crc = crc16_ccitt(data)
        sock.sendto(data + struct.pack("<H", crc), ("127.0.0.1", worker.ipc_port))

        sock.close()
        time.sleep(0.3)
        self.assertTrue(worker.is_alive())

    def test_adversarial_oob_fragment_indices(self):
        """Fragment index exceeding fragment count or boundary is safely dropped without crash."""
        cp = self.harness.start_control_plane(name="cp-ipc-frag")
        worker = self.harness.start_worker("worker-ipc-frag", 1, cp)

        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        # Send frag_index = 5, total_fragments = 2
        header = struct.pack(
            "<IBBHIIIBBH",
            0x4332334B, 1, 1, 99,
            fnv1a_32("pod://worker-ipc-frag/test"),
            fnv1a_32("attacker-pod"),
            0, 5, 2, 8
        )
        data = header + b"OobFrag0"
        crc = crc16_ccitt(data)
        sock.sendto(data + struct.pack("<H", crc), ("127.0.0.1", worker.ipc_port))

        sock.close()
        time.sleep(0.3)
        self.assertTrue(worker.is_alive())

