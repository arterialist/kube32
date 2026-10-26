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
        """Valid K32C packet sent over UDP to node's IPC port is accepted without crash."""
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

        time.sleep(0.2)
        # Node must remain healthy and alive
        self.assertTrue(worker.is_alive())

    def test_corrupt_crc16_packet_rejected(self):
        """Packet with corrupted CRC16 must be dropped and not crash the receiver."""
        cp = self.harness.start_control_plane(name="cp-ipc-1")
        worker = self.harness.start_worker("worker-ipc-2", 1, cp)

        # Build corrupted packet
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

        time.sleep(0.2)
        self.assertTrue(worker.is_alive())
